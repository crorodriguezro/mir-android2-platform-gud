/* Minimal raw RGB565 frame sink for a Linux HDMI framebuffer. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
	char const *path = argc > 1 ? argv[1] : "/dev/fb0";
	unsigned int width = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 1920;
	unsigned int height = argc > 3 ? (unsigned int)strtoul(argv[3], NULL, 10) : 1080;
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	size_t row_bytes = (size_t)width * 2;
	size_t frame_bytes = row_bytes * height;
	uint8_t *frame, *mapped;
	uint64_t frames = 0;
	int fd = open(path, O_RDWR);
	if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fixed) < 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &variable) < 0) {
		perror("open/query framebuffer");
		return EXIT_FAILURE;
	}
	if (variable.xres < width || variable.yres < height ||
	    variable.bits_per_pixel != 16 || fixed.line_length < row_bytes) {
		fprintf(stderr, "unsupported framebuffer %ux%u bpp=%u pitch=%u\n",
			variable.xres, variable.yres, variable.bits_per_pixel,
			fixed.line_length);
		return EXIT_FAILURE;
	}
	mapped = mmap(NULL, fixed.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	frame = malloc(frame_bytes);
	if (mapped == MAP_FAILED || !frame) {
		perror("map/allocate framebuffer");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "fb-frame-sink output=%s mode=%ux%u rgb565 pitch=%u\n",
		path, width, height, fixed.line_length);
	for (;;) {
		unsigned int row;
		int result = read_full(STDIN_FILENO, frame, frame_bytes);
		if (result <= 0) break;
		for (row = 0; row < height; ++row)
			memcpy(mapped + (size_t)row * fixed.line_length,
				frame + (size_t)row * row_bytes, row_bytes);
		++frames;
	}
	fprintf(stderr, "fb-frame-sink frames=%llu\n", (unsigned long long)frames);
	return EXIT_SUCCESS;
}
