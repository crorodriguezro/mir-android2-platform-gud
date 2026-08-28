/*
 * Small hardware-only H.264 receiver for the Pi Zero 2 W.
 *
 * Wire format: the sender's 32-byte MH264FRM header followed by one Annex-B
 * access unit.  The decoder capture queue is exported as linear NV12 DMA-BUFs
 * and those buffers are imported into the already-mastering GUD DRM owner.
 * There is deliberately no FFmpeg/VLC/libswscale path here.
 */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "h264_latest_presenter_state.h"

#define WIDTH 1920U
#define DISPLAY_HEIGHT 1080U
#define CODED_HEIGHT 1088U
#define OUTPUT_COUNT 4U
#define CAPTURE_COUNT 6U
#define MAX_AU (2U * 1024U * 1024U)
#define META_COUNT 128U
#define DRM_FORMAT_NV12 0x3231564eU

struct output_buffer {
	void *address;
	size_t length;
	unsigned int queued;
};

struct capture_buffer {
	void *address;
	size_t length;
	int dma_fd;
	uint32_t handle;
	uint32_t framebuffer;
	unsigned int queued;
};

struct metadata {
	uint64_t sequence;
	uint64_t source_ns;
	uint64_t receive_ns;
	uint64_t decoder_submit_ns;
};

struct sample {
	int64_t value_ns;
};

#define PI_SAMPLE_COUNT 4096U

struct pi_present_sample {
	uint64_t receive_to_submit_ns;
	uint64_t decode_ns;
	uint64_t decode_to_drm_ns;
	uint64_t drm_submit_ns;
	uint64_t total_ns;
	int64_t relative_age_ns;
};

struct pi_stats {
	uint64_t decoder_submitted;
	uint64_t decoder_completed;
	uint64_t drm_submitted;
	uint64_t drm_completed;
	uint64_t immediate_submits;
	uint64_t pending_frames;
	uint64_t pending_replacements;
	unsigned int max_presentation_in_flight;
	unsigned int max_presentation_pending;
	uint64_t decoder_durations[PI_SAMPLE_COUNT];
	unsigned int decoder_duration_count;
	struct pi_present_sample presented[PI_SAMPLE_COUNT];
	unsigned int presented_count;
};

struct presenter_frame {
	struct metadata metadata;
	uint64_t decode_end_ns;
};

struct latest_presenter {
	pthread_mutex_t mutex;
	pthread_cond_t wakeup;
	pthread_t thread;
	struct h264_latest_state state;
	struct presenter_frame frame[CAPTURE_COUNT];
	struct capture_buffer *capture;
	struct pi_stats *stats;
	struct sample *ages;
	unsigned int *age_count;
	uint64_t *first_source_ns;
	uint64_t *first_rx_ns;
	int decoder;
	int drmfd;
	uint32_t plane_id;
	uint32_t crtc_id;
	int displayed;
	int stopping;
	int failure;
};

static volatile sig_atomic_t stop_requested;

static void stop_handler(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static uint64_t monotonic_ns(void)
{
	struct timespec value;
	clock_gettime(CLOCK_MONOTONIC, &value);
	return (uint64_t)value.tv_sec * 1000000000ULL + value.tv_nsec;
}

static int xioctl(int fd, unsigned long request, void *argument)
{
	int result;
	do result = ioctl(fd, request, argument); while (result < 0 && errno == EINTR);
	return result;
}

static int read_full(int fd, void *data, size_t length)
{
	uint8_t *cursor = data;
	while (length) {
		ssize_t count = read(fd, cursor, length);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return count == 0 ? 0 : -1;
		cursor += count;
		length -= (size_t)count;
	}
	return 1;
}

static uint32_t get_be32(uint8_t const *source)
{
	return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) |
		((uint32_t)source[2] << 8) | source[3];
}

static uint64_t get_be64(uint8_t const *source)
{
	return ((uint64_t)get_be32(source) << 32) | get_be32(source + 4);
}

static int listen_tcp(unsigned int port)
{
	struct sockaddr_in address = {0};
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	if (fd < 0) return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
		listen(fd, 1) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int duplicate_drm_fd(unsigned int pid, unsigned int target_fd)
{
	int pidfd = (int)syscall(SYS_pidfd_open, (pid_t)pid, 0);
	int drmfd;
	if (pidfd < 0) return -1;
	drmfd = (int)syscall(SYS_pidfd_getfd, pidfd, (int)target_fd, 0);
	close(pidfd);
	return drmfd;
}

static int import_dmabuf(int drmfd, int dma_fd, uint32_t *handle)
{
	struct drm_prime_handle prime = {
		.fd = dma_fd,
		.flags = O_CLOEXEC,
	};
	if (xioctl(drmfd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0)
		return -1;
	*handle = prime.handle;
	return 0;
}

static int add_nv12_fb(int drmfd, uint32_t handle, uint32_t *framebuffer)
{
	struct drm_mode_fb_cmd2 command = {
		.width = WIDTH,
		.height = CODED_HEIGHT,
		.pixel_format = DRM_FORMAT_NV12,
		.handles = {handle, handle},
		.pitches = {WIDTH, WIDTH},
		.offsets = {0, WIDTH * CODED_HEIGHT},
	};
	if (xioctl(drmfd, DRM_IOCTL_MODE_ADDFB2, &command) < 0)
		return -1;
	*framebuffer = command.fb_id;
	return 0;
}

static int set_plane(int drmfd, uint32_t plane, uint32_t crtc,
			     uint32_t framebuffer, unsigned int source_height)
{
	struct drm_mode_set_plane command = {
		.plane_id = plane,
		.crtc_id = crtc,
		.fb_id = framebuffer,
		.crtc_w = WIDTH,
		.crtc_h = DISPLAY_HEIGHT,
		.src_w = WIDTH << 16,
		.src_h = source_height << 16,
	};
	return xioctl(drmfd, DRM_IOCTL_MODE_SETPLANE, &command);
}

static int disable_plane(int drmfd, uint32_t plane)
{
	struct drm_mode_set_plane command = {.plane_id = plane};
	return xioctl(drmfd, DRM_IOCTL_MODE_SETPLANE, &command);
}

static void set_plane_property(int drmfd, uint32_t plane, uint32_t property,
			       uint64_t value, char const *name)
{
	struct drm_mode_obj_set_property command = {
		.value = value,
		.prop_id = property,
		.obj_id = plane,
		.obj_type = DRM_MODE_OBJECT_PLANE,
	};
	if (property && xioctl(drmfd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &command) < 0)
		fprintf(stderr, "drm property %s=%llu failed: %s\n", name,
			(unsigned long long)value, strerror(errno));
	else if (property)
		fprintf(stderr, "drm property %s=%llu accepted\n", name,
			(unsigned long long)value);
}

static int queue_capture(int decoder, unsigned int index,
			 struct capture_buffer *buffer)
{
	struct v4l2_plane plane = {.length = (uint32_t)buffer->length};
	struct v4l2_buffer command = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
		.length = 1,
		.m.planes = &plane,
	};
	if (xioctl(decoder, VIDIOC_QBUF, &command) < 0) return -1;
	buffer->queued = 1;
	return 0;
}

static int queue_output(int decoder, unsigned int index,
			struct output_buffer *buffer, size_t bytesused)
{
	struct v4l2_plane plane = {
		.bytesused = (uint32_t)bytesused,
		.length = (uint32_t)buffer->length,
	};
	struct v4l2_buffer command = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
		.length = 1,
		.m.planes = &plane,
	};
	if (xioctl(decoder, VIDIOC_QBUF, &command) < 0) return -1;
	buffer->queued = 1;
	return 0;
}

static void sort_samples(struct sample *samples, unsigned int count)
{
	unsigned int i;
	for (i = 1; i < count; ++i) {
		struct sample current = samples[i];
		unsigned int j = i;
		while (j && samples[j - 1].value_ns > current.value_ns) {
			samples[j] = samples[j - 1];
			--j;
		}
		samples[j] = current;
	}
}

static uint64_t present_sample_value(struct pi_present_sample const *sample, int metric)
{
	switch (metric) {
	case 0: return sample->receive_to_submit_ns;
	case 1: return sample->decode_ns;
	case 2: return sample->decode_to_drm_ns;
	case 3: return sample->drm_submit_ns;
	default: return sample->total_ns;
	}
}

static void report_pi_metric(struct pi_present_sample const *samples, unsigned int count,
	char const *name, int metric)
{
	uint64_t values[PI_SAMPLE_COUNT];
	uint64_t sum = 0, maximum = 0;
	unsigned int i, p50, p95;
	if (!count) return;
	for (i = 0; i < count; ++i) {
		values[i] = present_sample_value(&samples[i], metric);
		sum += values[i];
		if (values[i] > maximum) maximum = values[i];
	}
	for (i = 1; i < count; ++i) {
		uint64_t value = values[i];
		unsigned int j = i;
		while (j && values[j - 1] > value) {
			values[j] = values[j - 1];
			--j;
		}
		values[j] = value;
	}
	p50 = count / 2;
	p95 = (count * 95) / 100;
	if (p95 >= count) p95 = count - 1;
	fprintf(stderr, "pi_timing metric=%s count=%u mean_us=%.1f p50_us=%.1f "
		"p95_us=%.1f max_us=%.1f\n", name, count,
		sum / 1000.0 / count, values[p50] / 1000.0,
		values[p95] / 1000.0, maximum / 1000.0);
}

static void report_pi_decode_metric(struct pi_stats const *stats)
{
	uint64_t values[PI_SAMPLE_COUNT];
	uint64_t sum = 0, maximum = 0;
	unsigned int i, p50, p95, count = stats->decoder_duration_count;
	if (!count) return;
	for (i = 0; i < count; ++i) {
		values[i] = stats->decoder_durations[i];
		sum += values[i];
		if (values[i] > maximum) maximum = values[i];
	}
	for (i = 1; i < count; ++i) {
		uint64_t value = values[i];
		unsigned int j = i;
		while (j && values[j - 1] > value) {
			values[j] = values[j - 1];
			--j;
		}
		values[j] = value;
	}
	p50 = count / 2;
	p95 = (count * 95) / 100;
	if (p95 >= count) p95 = count - 1;
	fprintf(stderr, "pi_timing metric=decode count=%u mean_us=%.1f p50_us=%.1f "
		"p95_us=%.1f max_us=%.1f\n", count,
		sum / 1000.0 / count, values[p50] / 1000.0,
		values[p95] / 1000.0, maximum / 1000.0);
}

static void report_pi_stats(struct pi_stats const *stats, unsigned int received,
	unsigned int decoded, unsigned int presented, unsigned int dropped)
{
	fprintf(stderr, "pi_counts received=%u decoder_submitted=%llu decoder_completed=%u "
		"drm_submitted=%u decoder_replaced=%u decoder_missing=%u dropped_before_decode=%u "
		"presentation_completed=%llu immediate_submits=%llu pending_frames=%llu "
		"pending_replacements=%llu max_presentation_in_flight=%u "
		"max_presentation_pending=%u\n", received,
		(unsigned long long)stats->decoder_submitted, decoded, presented,
		decoded >= presented ? decoded - presented : 0,
		stats->decoder_submitted >= decoded ?
			(unsigned int)(stats->decoder_submitted - decoded) : 0, dropped,
		(unsigned long long)stats->drm_completed,
		(unsigned long long)stats->immediate_submits,
		(unsigned long long)stats->pending_frames,
		(unsigned long long)stats->pending_replacements,
		stats->max_presentation_in_flight, stats->max_presentation_pending);
	report_pi_decode_metric(stats);
	report_pi_metric(stats->presented, stats->presented_count,
		"receive_to_decoder_submit", 0);
	report_pi_metric(stats->presented, stats->presented_count, "decode_to_drm", 2);
	report_pi_metric(stats->presented, stats->presented_count, "drm_submit", 3);
	report_pi_metric(stats->presented, stats->presented_count, "receive_to_presentation_submit", 4);
}

static void *latest_presenter_worker(void *argument)
{
	struct latest_presenter *presenter = argument;
	for (;;) {
		struct presenter_frame frame;
		uint64_t commit_start, commit_end;
		int index, old_displayed;
		pthread_mutex_lock(&presenter->mutex);
		while ((presenter->state.in_flight < 0 || presenter->state.started) &&
		       !presenter->stopping)
			pthread_cond_wait(&presenter->wakeup, &presenter->mutex);
		if (presenter->state.in_flight < 0 && presenter->stopping) {
			pthread_mutex_unlock(&presenter->mutex);
			break;
		}
		index = h264_latest_begin(&presenter->state);
		frame = presenter->frame[index];
		presenter->stats->drm_submitted = presenter->state.submitted;
		pthread_mutex_unlock(&presenter->mutex);

		commit_start = monotonic_ns();
		if (set_plane(presenter->drmfd, presenter->plane_id, presenter->crtc_id,
			presenter->capture[index].framebuffer, DISPLAY_HEIGHT) < 0) {
			perror("SETPLANE NV12");
			pthread_mutex_lock(&presenter->mutex);
			presenter->failure = 1;
			presenter->stopping = 1;
			h264_latest_complete(&presenter->state);
			pthread_cond_broadcast(&presenter->wakeup);
			pthread_mutex_unlock(&presenter->mutex);
			break;
		}
		commit_end = monotonic_ns();

		pthread_mutex_lock(&presenter->mutex);
		h264_latest_complete(&presenter->state);
		presenter->stats->drm_completed = presenter->state.completed;
		old_displayed = presenter->displayed;
		presenter->displayed = index;
		if (presenter->stats->presented_count < PI_SAMPLE_COUNT &&
		    frame.metadata.receive_ns) {
			struct pi_present_sample *sample =
				&presenter->stats->presented[presenter->stats->presented_count++];
			sample->receive_to_submit_ns = frame.metadata.decoder_submit_ns -
				frame.metadata.receive_ns;
			sample->decode_ns = frame.decode_end_ns - frame.metadata.decoder_submit_ns;
			sample->decode_to_drm_ns = commit_start - frame.decode_end_ns;
			sample->drm_submit_ns = commit_end - commit_start;
			sample->total_ns = commit_end - frame.metadata.receive_ns;
		}
		if (frame.metadata.source_ns && *presenter->first_source_ns &&
		    *presenter->first_rx_ns && *presenter->age_count < META_COUNT) {
			int64_t relative_age = (int64_t)(commit_end - *presenter->first_rx_ns) -
				(int64_t)(frame.metadata.source_ns - *presenter->first_source_ns);
			presenter->ages[(*presenter->age_count)++].value_ns = relative_age;
			if (presenter->state.completed == 1 || presenter->state.completed % 30 == 0)
				fprintf(stderr, "present seq=%llu relative_age_ms=%.3f pending_replaced=%llu\n",
					(unsigned long long)frame.metadata.sequence,
					relative_age / 1000000.0,
					(unsigned long long)presenter->state.pending_replaced);
		}
		pthread_mutex_unlock(&presenter->mutex);

		/* The previous buffer is safe only after this SETPLANE completed. */
		if (old_displayed >= 0 && queue_capture(presenter->decoder,
			(unsigned int)old_displayed, &presenter->capture[old_displayed]) < 0) {
			perror("QBUF previously displayed capture");
			pthread_mutex_lock(&presenter->mutex);
			presenter->failure = 1;
			presenter->stopping = 1;
			pthread_cond_broadcast(&presenter->wakeup);
			pthread_mutex_unlock(&presenter->mutex);
			break;
		}
	}
	return NULL;
}

static int latest_presenter_start(struct latest_presenter *presenter,
	int decoder, int drmfd, uint32_t plane_id, uint32_t crtc_id,
	struct capture_buffer capture[CAPTURE_COUNT], struct pi_stats *stats,
	struct sample ages[META_COUNT], unsigned int *age_count,
	uint64_t *first_source_ns, uint64_t *first_rx_ns)
{
	memset(presenter, 0, sizeof(*presenter));
	presenter->decoder = decoder;
	presenter->drmfd = drmfd;
	presenter->plane_id = plane_id;
	presenter->crtc_id = crtc_id;
	presenter->capture = capture;
	presenter->stats = stats;
	presenter->ages = ages;
	presenter->age_count = age_count;
	presenter->first_source_ns = first_source_ns;
	presenter->first_rx_ns = first_rx_ns;
	presenter->displayed = -1;
	h264_latest_state_init(&presenter->state);
	if (pthread_mutex_init(&presenter->mutex, NULL) ||
	    pthread_cond_init(&presenter->wakeup, NULL))
		return -1;
	return pthread_create(&presenter->thread, NULL, latest_presenter_worker, presenter);
}

static int latest_presenter_offer(struct latest_presenter *presenter, int index,
	struct metadata const *metadata, uint64_t decode_end_ns)
{
	int replaced;
	pthread_mutex_lock(&presenter->mutex);
	if (presenter->failure || presenter->stopping) {
		pthread_mutex_unlock(&presenter->mutex);
		return -1;
	}
	replaced = h264_latest_offer(&presenter->state, index);
	presenter->frame[index].metadata = *metadata;
	presenter->frame[index].decode_end_ns = decode_end_ns;
	presenter->stats->immediate_submits = presenter->state.immediate;
	presenter->stats->pending_frames = presenter->state.pending_stored;
	presenter->stats->pending_replacements = presenter->state.pending_replaced;
	presenter->stats->max_presentation_in_flight = presenter->state.max_in_flight;
	presenter->stats->max_presentation_pending = presenter->state.max_pending;
	pthread_cond_signal(&presenter->wakeup);
	pthread_mutex_unlock(&presenter->mutex);
	if (replaced >= 0 && queue_capture(presenter->decoder, (unsigned int)replaced,
		&presenter->capture[replaced]) < 0) {
		perror("QBUF replaced pending capture");
		return -1;
	}
	return 0;
}

static int latest_presenter_stop(struct latest_presenter *presenter)
{
	int failure;
	pthread_mutex_lock(&presenter->mutex);
	presenter->stopping = 1;
	pthread_cond_broadcast(&presenter->wakeup);
	pthread_mutex_unlock(&presenter->mutex);
	pthread_join(presenter->thread, NULL);
	pthread_mutex_lock(&presenter->mutex);
	presenter->stats->immediate_submits = presenter->state.immediate;
	presenter->stats->pending_frames = presenter->state.pending_stored;
	presenter->stats->pending_replacements = presenter->state.pending_replaced;
	presenter->stats->max_presentation_in_flight = presenter->state.max_in_flight;
	presenter->stats->max_presentation_pending = presenter->state.max_pending;
	failure = presenter->failure || !h264_latest_state_valid(&presenter->state);
	pthread_mutex_unlock(&presenter->mutex);
	pthread_cond_destroy(&presenter->wakeup);
	pthread_mutex_destroy(&presenter->mutex);
	return failure ? -1 : 0;
}

static int drain_decoded_frames(int decoder,
	struct capture_buffer capture[CAPTURE_COUNT],
	struct metadata metadata[META_COUNT], unsigned int *metadata_head,
	unsigned int metadata_tail, unsigned int *decoded,
	uint64_t *total_dequeue_ns, struct pi_stats *stats,
	struct latest_presenter *presenter)
{
	for (;;) {
		struct v4l2_plane plane = {0};
		struct v4l2_buffer buffer = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.length = 1,
			.m.planes = &plane,
		};
		struct metadata frame_metadata = {0};
		uint64_t dequeue_start = monotonic_ns();
		uint64_t decode_end;
		if (xioctl(decoder, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN) return 0;
			perror("DQBUF capture");
			return -1;
		}
		decode_end = monotonic_ns();
		*total_dequeue_ns += decode_end - dequeue_start;
		if (buffer.index >= CAPTURE_COUNT) {
			fprintf(stderr, "invalid decoder capture index=%u\n", buffer.index);
			return -1;
		}
		capture[buffer.index].queued = 0;
		if (*metadata_head != metadata_tail)
			frame_metadata = metadata[(*metadata_head)++ % META_COUNT];
		++*decoded;
		++stats->decoder_completed;
		if (stats->decoder_duration_count < PI_SAMPLE_COUNT &&
		    frame_metadata.decoder_submit_ns)
			stats->decoder_durations[stats->decoder_duration_count++] =
				decode_end - frame_metadata.decoder_submit_ns;
		if (latest_presenter_offer(presenter, (int)buffer.index,
			&frame_metadata, decode_end) < 0)
			return -1;
	}
}

int main(int argc, char **argv)
{
	unsigned int port = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 10) : 5505;
	unsigned int drm_pid = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 0;
	unsigned int drm_fd_number = argc > 3 ? (unsigned int)strtoul(argv[3], NULL, 10) : 3;
	uint32_t plane_id = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 84;
	uint32_t crtc_id = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 95;
	unsigned int frame_limit = argc > 6 ? (unsigned int)strtoul(argv[6], NULL, 10) : 900;
	int listen_fd = -1, connection = -1, decoder = -1, drmfd = -1;
	struct output_buffer output[OUTPUT_COUNT] = {0};
	struct capture_buffer capture[CAPTURE_COUNT] = {0};
	struct metadata metadata[META_COUNT] = {0};
	struct sample ages[META_COUNT] = {0};
	unsigned int metadata_head = 0, metadata_tail = 0, age_count = 0;
	unsigned int frames = 0, decoded = 0, presented = 0, dropped = 0;
	struct pi_stats stats = {0};
	uint64_t total_decode_ns = 0, total_commit_ns = 0;
	uint64_t first_source_ns = 0, first_rx_ns = 0;
	struct latest_presenter presenter;
	int presenter_started = 0;
	struct rusage usage;
	int result = EXIT_FAILURE;
	unsigned int index;
	for (index = 0; index < CAPTURE_COUNT; ++index) capture[index].dma_fd = -1;

	if (argc > 7 || !port || !drm_pid || !frame_limit) {
		fprintf(stderr, "usage: %s [port] drm-owner-pid [drm-fd] [plane] [crtc] [frames]\n", argv[0]);
		return EXIT_FAILURE;
	}
	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	listen_fd = listen_tcp(port);
	if (listen_fd < 0) { perror("listen"); goto done; }
	fprintf(stderr, "direct receiver listening port=%u decoder=/dev/video10 plane=%u crtc=%u\n",
		port, plane_id, crtc_id);
	connection = accept(listen_fd, NULL, NULL);
	if (connection < 0) { perror("accept"); goto done; }
	decoder = open("/dev/video10", O_RDWR | O_NONBLOCK);
	if (decoder < 0) { perror("open decoder"); goto done; }
	drmfd = duplicate_drm_fd(drm_pid, drm_fd_number);
	if (drmfd < 0) { perror("duplicate DRM owner fd"); goto done; }

	{
		struct v4l2_format format = {0};
		format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		format.fmt.pix_mp.width = WIDTH;
		format.fmt.pix_mp.height = DISPLAY_HEIGHT;
		format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
		format.fmt.pix_mp.field = V4L2_FIELD_NONE;
		format.fmt.pix_mp.num_planes = 1;
		format.fmt.pix_mp.plane_fmt[0].sizeimage = MAX_AU;
		if (xioctl(decoder, VIDIOC_S_FMT, &format) < 0) { perror("S_FMT H264"); goto done; }
		fprintf(stderr, "decoder output accepted %ux%u format=H264 planes=%u size=%u\n",
			format.fmt.pix_mp.width, format.fmt.pix_mp.height,
			format.fmt.pix_mp.num_planes, format.fmt.pix_mp.plane_fmt[0].sizeimage);
		format = (struct v4l2_format){0};
		format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		format.fmt.pix_mp.width = WIDTH;
		format.fmt.pix_mp.height = DISPLAY_HEIGHT;
		format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
		format.fmt.pix_mp.field = V4L2_FIELD_NONE;
		format.fmt.pix_mp.num_planes = 1;
		if (xioctl(decoder, VIDIOC_S_FMT, &format) < 0) { perror("S_FMT NV12"); goto done; }
		fprintf(stderr, "decoder capture accepted %ux%u format=NV12 planes=%u pitch=%u size=%u colorspace=%u ycbcr=%u quant=%u\n",
			format.fmt.pix_mp.width, format.fmt.pix_mp.height,
			format.fmt.pix_mp.num_planes, format.fmt.pix_mp.plane_fmt[0].bytesperline,
			format.fmt.pix_mp.plane_fmt[0].sizeimage, format.fmt.pix_mp.colorspace,
			format.fmt.pix_mp.ycbcr_enc, format.fmt.pix_mp.quantization);
	}
	{
		struct v4l2_requestbuffers request = {
			.count = OUTPUT_COUNT, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
		};
		if (xioctl(decoder, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
			perror("REQBUFS output"); goto done;
		}
		for (index = 0; index < request.count; ++index) {
			struct v4l2_plane plane = {0};
			struct v4l2_buffer buffer = {.type = request.type, .memory = request.memory,
				.index = index, .length = 1, .m.planes = &plane};
			if (xioctl(decoder, VIDIOC_QUERYBUF, &buffer) < 0) { perror("QUERYBUF output"); goto done; }
			output[index].length = plane.length;
			output[index].address = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
				MAP_SHARED, decoder, plane.m.mem_offset);
			if (output[index].address == MAP_FAILED) { perror("mmap output"); goto done; }
		}
	}
	{
		struct v4l2_requestbuffers request = {
			.count = CAPTURE_COUNT, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
		};
		if (xioctl(decoder, VIDIOC_REQBUFS, &request) < 0 || request.count < 4) {
			perror("REQBUFS capture"); goto done;
		}
		fprintf(stderr, "decoder queue depths output=%u capture=%u\n", OUTPUT_COUNT, request.count);
		for (index = 0; index < request.count; ++index) {
			struct v4l2_plane plane = {0};
			struct v4l2_buffer buffer = {.type = request.type, .memory = request.memory,
				.index = index, .length = 1, .m.planes = &plane};
			struct v4l2_exportbuffer export = {.type = request.type, .index = index,
				.plane = 0, .flags = O_CLOEXEC};
			if (xioctl(decoder, VIDIOC_QUERYBUF, &buffer) < 0 ||
				xioctl(decoder, VIDIOC_EXPBUF, &export) < 0) {
				perror("QUERYBUF/EXPBUF capture"); goto done;
			}
			capture[index].length = plane.length;
			capture[index].address = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
				MAP_SHARED, decoder, plane.m.mem_offset);
			capture[index].dma_fd = export.fd;
			if (capture[index].address == MAP_FAILED) { perror("mmap capture"); goto done; }
			if (import_dmabuf(drmfd, capture[index].dma_fd, &capture[index].handle) < 0 ||
				add_nv12_fb(drmfd, capture[index].handle, &capture[index].framebuffer) < 0) {
				perror("DRM PRIME/ADDFB2 NV12"); goto done;
			}
			fprintf(stderr, "capture[%u] dmabuf=%d handle=%u fb=%u length=%zu\n", index,
				capture[index].dma_fd, capture[index].handle, capture[index].framebuffer,
				capture[index].length);
			if (queue_capture(decoder, index, &capture[index]) < 0) { perror("QBUF capture"); goto done; }
		}
	}
	set_plane_property(drmfd, plane_id, 89, 1, "COLOR_ENCODING_BT709");
	set_plane_property(drmfd, plane_id, 90, 0, "COLOR_RANGE_LIMITED");
	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (xioctl(decoder, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON capture"); goto done; }
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		if (xioctl(decoder, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON output"); goto done; }
	}
	fprintf(stderr, "direct receiver active: decoded NV12 DMABUF -> DRM PRIME NV12 -> VC4 1920x1080\n");
	if (latest_presenter_start(&presenter, decoder, drmfd, plane_id, crtc_id,
		capture, &stats, ages, &age_count, &first_source_ns, &first_rx_ns) < 0) {
		fprintf(stderr, "cannot start LatestDecodedFramePresenter\n");
		goto done;
	}
	presenter_started = 1;
	fprintf(stderr, "LatestDecodedFramePresenter active max_in_flight=1 max_pending=1\n");

	while (!stop_requested && frames < frame_limit) {
		uint8_t header[32];
		unsigned int output_index;
		uint32_t length;
		uint64_t receive_ns, decoder_submit_ns;
		struct pollfd wait_fd;
		for (;;) {
			for (output_index = 0; output_index < OUTPUT_COUNT; ++output_index)
				if (!output[output_index].queued) break;
			if (output_index < OUTPUT_COUNT) break;
			wait_fd = (struct pollfd){.fd = decoder, .events = POLLIN | POLLOUT};
			if (poll(&wait_fd, 1, 1000) <= 0) { perror("poll output space"); goto done; }
			{
				struct v4l2_buffer buffer = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
					.memory = V4L2_MEMORY_MMAP, .length = 1};
				struct v4l2_plane plane = {0};
				buffer.m.planes = &plane;
				while (xioctl(decoder, VIDIOC_DQBUF, &buffer) == 0) {
					if (buffer.index < OUTPUT_COUNT) output[buffer.index].queued = 0;
				}
			}
		}
		/* Wait for the next wire frame and decoder completions together.  A
		 * blocking socket read here used to defer ready capture buffers until
		 * the following 33 ms input tick and manufactured decoder bursts. */
		for (;;) {
			struct pollfd ready[2] = {
				{.fd = connection, .events = POLLIN},
				{.fd = decoder, .events = POLLIN},
			};
			int poll_result = poll(ready, 2, 1000);
			if (poll_result < 0 && errno == EINTR) continue;
			if (poll_result <= 0) {
				if (!poll_result) continue;
				perror("poll socket/decoder"); goto done;
			}
			if (ready[1].revents & POLLIN)
				if (drain_decoded_frames(decoder, capture, metadata,
					&metadata_head, metadata_tail, &decoded,
					&total_decode_ns, &stats, &presenter) < 0)
					goto done;
			if (ready[0].revents & (POLLIN | POLLHUP | POLLERR)) break;
		}
		if (read_full(connection, header, sizeof(header)) <= 0) break;
		if (memcmp(header, "MH264FRM", 8) || header[8] != 1) {
			fprintf(stderr, "invalid frame header\n"); goto done;
		}
		length = get_be32(header + 28);
		if (!length || length > output[output_index].length || length > MAX_AU) {
			fprintf(stderr, "invalid access unit length=%u capacity=%zu\n", length,
				output[output_index].length); goto done;
		}
		if (read_full(connection, output[output_index].address, length) <= 0) break;
		receive_ns = monotonic_ns();
		if (metadata_tail - metadata_head >= META_COUNT) {
			fprintf(stderr, "metadata queue overflow\n"); goto done;
		}
		metadata[metadata_tail % META_COUNT].sequence = get_be64(header + 12);
		metadata[metadata_tail % META_COUNT].source_ns = get_be64(header + 20);
		if (!first_rx_ns) first_rx_ns = monotonic_ns();
		if (!first_source_ns) first_source_ns = metadata[metadata_tail % META_COUNT].source_ns;
		++metadata_tail;
		decoder_submit_ns = monotonic_ns();
		if (queue_output(decoder, output_index, &output[output_index], length) < 0) {
			perror("QBUF H264"); goto done;
		}
		metadata[(metadata_tail - 1) % META_COUNT].receive_ns = receive_ns;
		metadata[(metadata_tail - 1) % META_COUNT].decoder_submit_ns = decoder_submit_ns;
		++stats.decoder_submitted;
		++frames;

		/* Every decoded frame reaches the bounded presenter.  It coalesces only
		 * while SETPLANE is genuinely in flight on its dedicated worker. */
		if (drain_decoded_frames(decoder, capture, metadata, &metadata_head,
			metadata_tail, &decoded, &total_decode_ns, &stats, &presenter) < 0)
			goto done;
		if (frames % 30 == 0)
			fprintf(stderr, "direct frames received=%u decoded=%u presented=%llu dropped=%u input_queue=%u metadata_queue=%u\n",
				frames, decoded, (unsigned long long)stats.drm_completed, dropped, frames - decoded,
				metadata_tail - metadata_head);
	}

	/* Drain decoder output after EOF, but stop after a bounded 3 seconds. */
	{
		uint64_t deadline = monotonic_ns() + 3000000000ULL;
		while (metadata_head != metadata_tail && monotonic_ns() < deadline) {
			struct pollfd wait_fd = {.fd = decoder, .events = POLLIN};
			if (poll(&wait_fd, 1, 10) <= 0) continue;
			if (drain_decoded_frames(decoder, capture, metadata, &metadata_head,
				metadata_tail, &decoded, &total_decode_ns, &stats, &presenter) < 0)
				goto done;
		}
	}
	if (latest_presenter_stop(&presenter) < 0) {
		presenter_started = 0;
		fprintf(stderr, "LatestDecodedFramePresenter failed\n");
		goto done;
	}
	presenter_started = 0;
	presented = (unsigned int)stats.drm_completed;
	for (index = 0; index < stats.presented_count; ++index)
		total_commit_ns += stats.presented[index].drm_submit_ns;
	if (metadata_head != metadata_tail)
		fprintf(stderr, "direct end_of_stream pending_decoder_metadata=%u\n",
			metadata_tail - metadata_head);
	report_pi_stats(&stats, frames, decoded, presented, dropped);
	if (age_count) {
		sort_samples(ages, age_count);
		fprintf(stderr, "pi_frame_age metric=relative_start_offset count=%u p50_ms=%.3f "
			"p95_ms=%.3f max_ms=%.3f\n", age_count,
			ages[age_count / 2].value_ns / 1000000.0,
			ages[(age_count * 95) / 100 < age_count ? (age_count * 95) / 100 : age_count - 1].value_ns / 1000000.0,
			ages[age_count - 1].value_ns / 1000000.0);
	}
	if (presented)
		fprintf(stderr, "GUD scanout was not restored; H.264 plane will be disabled at teardown\n");
	getrusage(RUSAGE_SELF, &usage);
	if (age_count) {
		sort_samples(ages, age_count);
		fprintf(stderr, "direct result received=%u decoded=%u presented=%u dropped=%u "
			"relative_age_ms_p50=%.3f relative_age_ms_p95=%.3f "
			"decode_us_avg=%.1f commit_us_avg=%.1f cpu_user_ms=%.1f cpu_sys_ms=%.1f\n",
			frames, decoded, presented, dropped,
			ages[age_count / 2].value_ns / 1000000.0,
			ages[(age_count * 95) / 100].value_ns / 1000000.0,
			decoded ? total_decode_ns / 1000.0 / decoded : 0.0,
			presented ? total_commit_ns / 1000.0 / presented : 0.0,
			usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0,
			usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0);
	}
	result = presented ? EXIT_SUCCESS : EXIT_FAILURE;

done:
	if (presenter_started) {
		latest_presenter_stop(&presenter);
		presenter_started = 0;
	}
	if (drmfd >= 0 && disable_plane(drmfd, plane_id) < 0)
		perror("disable H.264 plane");
	if (decoder >= 0) {
		enum v4l2_buf_type output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		enum v4l2_buf_type capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(decoder, VIDIOC_STREAMOFF, &output_type);
		xioctl(decoder, VIDIOC_STREAMOFF, &capture_type);
	}
	if (drmfd >= 0) {
		for (index = 0; index < CAPTURE_COUNT; ++index) {
			if (capture[index].framebuffer)
				xioctl(drmfd, DRM_IOCTL_MODE_RMFB, &capture[index].framebuffer);
			if (capture[index].handle) {
				struct drm_gem_close close_handle = {.handle = capture[index].handle};
				xioctl(drmfd, DRM_IOCTL_GEM_CLOSE, &close_handle);
			}
			if (capture[index].dma_fd >= 0) close(capture[index].dma_fd);
		}
	}
	for (index = 0; index < OUTPUT_COUNT; ++index)
		if (output[index].address && output[index].address != MAP_FAILED)
			munmap(output[index].address, output[index].length);
	for (index = 0; index < CAPTURE_COUNT; ++index)
		if (capture[index].address && capture[index].address != MAP_FAILED)
			munmap(capture[index].address, capture[index].length);
	if (connection >= 0) close(connection);
	if (listen_fd >= 0) close(listen_fd);
	if (decoder >= 0) close(decoder);
	if (drmfd >= 0) close(drmfd);
	return result;
}
