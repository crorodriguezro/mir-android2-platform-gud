/* Map an existing framebuffer through the DRM owner's duplicated file. */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int read_full(void *data, size_t length)
{
	uint8_t *cursor = data;
	while (length) {
		ssize_t count = read(STDIN_FILENO, cursor, length);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return count == 0 ? 0 : -1;
		cursor += count;
		length -= (size_t)count;
	}
	return 1;
}

int main(int argc, char **argv)
{
	struct drm_mode_fb_cmd2 fb = {0};
	struct drm_mode_map_dumb map = {0};
	unsigned int pid, target_fd, input_width, input_height, flip, scale_full;
	size_t frame_bytes;
	uint8_t *frame, *pixels;
	unsigned int *source_x;
	unsigned long long frames = 0;
	unsigned int dirtyfb = 1;
	unsigned int syncfb = 1;
	struct timespec started;
	int pidfd, drmfd;
	if (argc < 4 || argc > 8) {
		fprintf(stderr, "usage: %s drm-owner-pid drm-fd framebuffer-id [input-width input-height flip-y scale-full]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (getenv("ACTIVE_DRM_SINK_NO_DIRTY"))
		dirtyfb = 0;
	if (getenv("ACTIVE_DRM_SINK_NO_SYNC"))
		syncfb = 0;
	pid = (unsigned int)strtoul(argv[1], NULL, 10);
	target_fd = (unsigned int)strtoul(argv[2], NULL, 10);
	fb.fb_id = (uint32_t)strtoul(argv[3], NULL, 10);
	input_width = argc > 4 ? (unsigned int)strtoul(argv[4], NULL, 10) : 1920;
	input_height = argc > 5 ? (unsigned int)strtoul(argv[5], NULL, 10) : 1080;
	flip = argc > 6 ? (unsigned int)strtoul(argv[6], NULL, 10) : 0;
	scale_full = argc > 7 ? (unsigned int)strtoul(argv[7], NULL, 10) : 0;
	pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
	drmfd = pidfd < 0 ? -1 : (int)syscall(SYS_pidfd_getfd, pidfd, target_fd, 0);
	if (drmfd < 0) {
		perror("duplicate DRM owner fd");
		return EXIT_FAILURE;
	}
	if (ioctl(drmfd, DRM_IOCTL_MODE_GETFB2, &fb) < 0 || !fb.handles[0]) {
		perror("DRM_IOCTL_MODE_GETFB2");
		return EXIT_FAILURE;
	}
	if (fb.pixel_format != 0x36314752 || fb.width != 1920 || fb.height != 1080) {
		fprintf(stderr, "unexpected framebuffer %ux%u format=%08x pitch=%u\n",
			fb.width, fb.height, fb.pixel_format, fb.pitches[0]);
		return EXIT_FAILURE;
	}
	map.handle = fb.handles[0];
	if (ioctl(drmfd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		return EXIT_FAILURE;
	}
	if (!input_width || !input_height || input_width > fb.width || input_height > fb.height) {
		fprintf(stderr, "invalid input size %ux%u for %ux%u framebuffer\n",
			input_width, input_height, fb.width, fb.height);
		return EXIT_FAILURE;
	}
	frame_bytes = (size_t)input_width * input_height * 2;
	pixels = mmap(NULL, (size_t)fb.pitches[0] * fb.height,
		PROT_READ | PROT_WRITE, MAP_SHARED, drmfd, map.offset);
	frame = malloc(frame_bytes);
	source_x = malloc((size_t)fb.width * sizeof(*source_x));
	if (pixels == MAP_FAILED || !frame || !source_x) {
		perror("map/allocate framebuffer");
		return EXIT_FAILURE;
	}
	for (unsigned int x = 0; x < fb.width; ++x)
		source_x[x] = x * input_width / fb.width;
	fprintf(stderr, "active-drm-sink pid=%u fd=%u fb=%u handle=%u pitch=%u input=%ux%u flip-y=%u scale-full=%u dirtyfb=%u syncfb=%u\n",
		pid, target_fd, fb.fb_id, fb.handles[0], fb.pitches[0],
		input_width, input_height, flip, scale_full, dirtyfb, syncfb);
	memset(pixels, 0, (size_t)fb.pitches[0] * fb.height);
	clock_gettime(CLOCK_MONOTONIC, &started);
	while (read_full(frame, frame_bytes) > 0) {
		struct drm_mode_fb_dirty_cmd dirty = {.fb_id = fb.fb_id};
		unsigned int const x_offset = (fb.width - input_width) / 2;
		unsigned int const y_offset = (fb.height - input_height) / 2;
		unsigned int row;
		if (scale_full) {
			for (row = 0; row < fb.height; ++row) {
				unsigned int source_row = row * input_height / fb.height;
				uint16_t const *source;
				uint16_t *destination = (uint16_t *)(pixels + (size_t)row * fb.pitches[0]);
				if (flip) source_row = input_height - 1 - source_row;
				source = (uint16_t const *)frame + (size_t)source_row * input_width;
				for (unsigned int x = 0; x < fb.width; ++x)
					destination[x] = source[source_x[x]];
			}
		} else {
			for (row = 0; row < input_height; ++row) {
				unsigned int const source_row = flip ? input_height - 1 - row : row;
				memcpy(pixels + (size_t)(row + y_offset) * fb.pitches[0] + x_offset * 2,
					frame + (size_t)source_row * input_width * 2, (size_t)input_width * 2);
			}
		}
		++frames;
		/*
		 * The dumb-buffer mapping is coherent on the Pi's VC4 path.  A
		 * synchronous 8 MiB cache flush here throttles the receiver to about
		 * 4 FPS and creates the stale-frame backlog this POC is measuring.
		 * Schedule the flush and let the DRM dirty operation provide the
		 * presentation boundary; the pipe remains bounded by this one-frame
		 * input buffer.
		 */
		if (syncfb)
			msync(pixels, (size_t)fb.pitches[0] * fb.height, MS_ASYNC);
		if (dirtyfb)
			ioctl(drmfd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
		if (frames == 1) {
			fprintf(stderr, "active-drm-sink first-frame-presented\n");
			fflush(stderr);
		}
		if (frames % 30 == 0) {
			struct timespec now;
			double seconds;
			clock_gettime(CLOCK_MONOTONIC, &now);
			seconds = now.tv_sec - started.tv_sec + (now.tv_nsec - started.tv_nsec) / 1e9;
			fprintf(stderr, "active-drm-sink frames=%llu seconds=%.3f fps=%.2f\n",
				frames, seconds, frames / seconds);
			fflush(stderr);
		}
	}
	fprintf(stderr, "active-drm-sink frames=%llu\n", frames);
	return EXIT_SUCCESS;
}
