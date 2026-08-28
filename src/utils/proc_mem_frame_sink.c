/* Write raw RGB565 frames into two already-mapped DRM dumb buffers. */
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static int write_full_at(int fd, void const *data, size_t length, off_t offset)
{
	uint8_t const *cursor = data;
	while (length) {
		ssize_t count = pwrite(fd, cursor, length, offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return -1;
		cursor += count;
		offset += count;
		length -= (size_t)count;
	}
	return 0;
}

int main(int argc, char **argv)
{
	char path[64];
	unsigned long long first, second;
	size_t frame_bytes = 1920U * 1080U * 2U;
	uint8_t *frame;
	unsigned long long frames = 0;
	int fd;
	if (argc != 4) {
		fprintf(stderr, "usage: %s pid address1 address2\n", argv[0]);
		return EXIT_FAILURE;
	}
	first = strtoull(argv[2], NULL, 16);
	second = strtoull(argv[3], NULL, 16);
	snprintf(path, sizeof(path), "/proc/%s/mem", argv[1]);
	fd = open(path, O_RDWR);
	frame = malloc(frame_bytes);
	if (fd < 0 || !frame) {
		perror("open process memory/allocate frame");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "proc-frame-sink pid=%s buffers=%llx,%llx bytes=%zu\n",
		argv[1], first, second, frame_bytes);
	while (read_full(frame, frame_bytes) > 0) {
		if (write_full_at(fd, frame, frame_bytes, (off_t)first) < 0 ||
		    write_full_at(fd, frame, frame_bytes, (off_t)second) < 0) {
			perror("write process framebuffer");
			return EXIT_FAILURE;
		}
		++frames;
	}
	fprintf(stderr, "proc-frame-sink frames=%llu\n", frames);
	return EXIT_SUCCESS;
}
