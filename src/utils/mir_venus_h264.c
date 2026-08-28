/*
 * Minimal Mir screencast -> Qualcomm Venus H.264 proof of concept.
 *
 * This deliberately resolves the legacy Mir client ABI at runtime.  The
 * target image contains libmirclient.so.9 but not its development headers;
 * keeping the small ABI boundary here makes the hardware experiment
 * reproducible without rebuilding the complete Ubuntu Touch platform.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NBUF 4
#define NPLANE VIDEO_MAX_PLANES

/* Qualcomm's downstream msm_vidc ABI predates the standard equivalents. */
#define V4L2_CID_MPEG_MSM_VIDC_BASE (V4L2_CTRL_CLASS_MPEG | 0x2000)
#define V4L2_CID_MPEG_VIDC_VIDEO_NUM_B_FRAMES \
	(V4L2_CID_MPEG_MSM_VIDC_BASE + 7)

typedef struct MirConnection MirConnection;
typedef struct MirScreencastSpec MirScreencastSpec;
typedef struct MirScreencast MirScreencast;
typedef struct MirBufferStream MirBufferStream;

typedef struct {
	int left;
	int top;
	unsigned int width;
	unsigned int height;
} MirRectangle;

typedef struct {
	int width;
	int height;
	int stride;
	int pixel_format;
	void *vaddr;
} MirGraphicsRegion;

struct mir_api {
	void *library;
	MirConnection *(*connect_sync)(char const *, char const *);
	int (*connection_is_valid)(MirConnection *);
	char const *(*connection_get_error_message)(MirConnection *);
	void (*connection_release)(MirConnection *);
	MirScreencastSpec *(*create_screencast_spec)(MirConnection *);
	void (*spec_set_width)(MirScreencastSpec *, unsigned int);
	void (*spec_set_height)(MirScreencastSpec *, unsigned int);
	void (*spec_set_pixel_format)(MirScreencastSpec *, int);
	void (*spec_set_capture_region)(MirScreencastSpec *, MirRectangle const *);
	void (*spec_set_number_of_buffers)(MirScreencastSpec *, unsigned int);
	void (*spec_set_mirror_mode)(MirScreencastSpec *, int);
	MirScreencast *(*screencast_create_sync)(MirScreencastSpec *);
	void (*screencast_spec_release)(MirScreencastSpec *);
	MirBufferStream *(*screencast_get_buffer_stream)(MirScreencast *);
	void (*screencast_release_sync)(MirScreencast *);
	void (*buffer_stream_get_graphics_region)(MirBufferStream *, MirGraphicsRegion *);
	void (*buffer_stream_swap_buffers_sync)(MirBufferStream *);
};

struct mapped_buffer {
	void *ptr[NPLANE];
	size_t len[NPLANE];
	int fd[NPLANE];
	unsigned int planes;
};

struct ts_mux {
	int enabled;
	int framed;
	unsigned int fps;
	uint8_t pat_continuity;
	uint8_t pmt_continuity;
	uint8_t video_continuity;
	uint64_t access_unit;
};

struct frame_meta {
	uint64_t sequence;
	uint64_t source_ns;
	uint64_t conversion_start_ns;
	uint64_t conversion_end_ns;
	uint64_t submit_ns;
};

#define PHONE_SAMPLE_COUNT 4096

struct phone_sample {
	uint64_t source_interval_ns;
	uint64_t conversion_ns;
	uint64_t encode_ns;
	uint64_t send_ns;
	uint64_t total_ns;
};

struct phone_stats {
	uint64_t mir_frames;
	uint64_t encoder_submitted;
	uint64_t encoder_completed;
	uint64_t transport_sent;
	uint64_t previous_source_ns;
	unsigned int sample_count;
	struct phone_sample samples[PHONE_SAMPLE_COUNT];
};

typedef int ion_user_handle_t;
struct ion_allocation_data {
	size_t len, align;
	unsigned int heap_id_mask, flags;
	ion_user_handle_t handle;
};
struct ion_fd_data { ion_user_handle_t handle; int fd; };
#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_SHARE _IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data)
#define ION_SYSTEM_HEAP_MASK (1U << 25)

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

static uint8_t clamp8(int value)
{
	return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static void mir_abgr_to_nv12(struct mapped_buffer *buffer,
	MirGraphicsRegion const *region, unsigned int width, unsigned int height,
	unsigned int y_stride, unsigned int y_scanlines)
{
	uint8_t *y_plane = buffer->ptr[0];
	uint8_t *uv_plane = buffer->planes > 1 ? buffer->ptr[1] :
		y_plane + (size_t)y_stride * y_scanlines;
	unsigned int x, y;

	memset(y_plane, 16, buffer->len[0]);
	if (buffer->planes > 1)
		memset(uv_plane, 128, buffer->len[1]);
	else
		memset(uv_plane, 128, buffer->len[0] - (size_t)y_stride * y_scanlines);
	for (y = 0; y < height; ++y) {
		uint8_t const *source = (uint8_t const *)region->vaddr +
			(size_t)(height - 1 - y) * region->stride;
		for (x = 0; x < width; ++x) {
			uint8_t const *p = source + 4 * x;
			/* Mir ABGR8888 is [R, G, B, A] in memory on this target. */
			y_plane[(size_t)y * y_stride + x] =
				clamp8(((47*p[0] + 157*p[1] + 16*p[2] + 128) >> 8) + 16);
		}
	}
	for (y = 0; y < height; y += 2) {
		for (x = 0; x < width; x += 2) {
			unsigned int sum_r = 0, sum_g = 0, sum_b = 0;
			unsigned int dy, dx;
			for (dy = 0; dy != 2; ++dy) {
				uint8_t const *source = (uint8_t const *)region->vaddr +
					(size_t)(height - 1 - y - dy) * region->stride;
				for (dx = 0; dx != 2; ++dx) {
					uint8_t const *p = source + 4 * (x + dx);
					sum_r += p[0];
					sum_g += p[1];
					sum_b += p[2];
				}
			}
			sum_r = (sum_r + 2) / 4;
			sum_g = (sum_g + 2) / 4;
			sum_b = (sum_b + 2) / 4;
			uv_plane[(size_t)(y / 2) * y_stride + x] =
				clamp8(((-26*(int)sum_r - 87*(int)sum_g + 112*(int)sum_b + 128) >> 8) + 128);
			uv_plane[(size_t)(y / 2) * y_stride + x + 1] =
				clamp8(((112*(int)sum_r - 102*(int)sum_g - 10*(int)sum_b + 128) >> 8) + 128);
		}
	}
}

static void set_rec709_limited(struct v4l2_pix_format_mplane *format)
{
	format->colorspace = V4L2_COLORSPACE_REC709;
	format->ycbcr_enc = V4L2_YCBCR_ENC_709;
	format->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_709;
}

static void print_color_format(char const *name,
	struct v4l2_pix_format_mplane const *format)
{
	fprintf(stderr, "%s colorspace=%u ycbcr_enc=%u quantization=%u xfer_func=%u\n",
		name, format->colorspace, format->ycbcr_enc, format->quantization,
		format->xfer_func);
}

static void fill_test_pattern(uint8_t *pixels, unsigned int width,
	unsigned int height)
{
	static uint8_t const colors[][3] = {
		{0, 0, 0}, {255, 255, 255}, {128, 128, 128},
		{255, 0, 0}, {0, 255, 0}, {0, 0, 255},
		{0, 255, 255}, {255, 0, 255}, {255, 255, 0}
	};
	unsigned int x, y;
	unsigned int const bar_height = height / 2;
	unsigned int const bar_width = width / (unsigned int)(sizeof(colors) / sizeof(colors[0]));
	for (y = 0; y < height; ++y) {
		for (x = 0; x < width; ++x) {
			uint8_t const *color;
			unsigned int bar = x / bar_width;
			if (bar >= sizeof(colors) / sizeof(colors[0]))
				bar = (unsigned int)(sizeof(colors) / sizeof(colors[0])) - 1;
			if (y < bar_height)
				color = colors[bar];
			else if (y < bar_height + 64)
				color = (x < width / 2) ? (uint8_t const[]) {16, 16, 16} :
					(uint8_t const[]) {235, 235, 235};
			else if (y < bar_height + 128)
				color = (x & 1) ? (uint8_t const[]) {255, 0, 0} :
					(uint8_t const[]) {0, 0, 255};
			else
				color = (uint8_t const[]) {((x * 255U) / (width - 1)),
					((y * 255U) / (height - 1)), 128};
			pixels[(size_t)y * width * 4 + 4 * x + 0] = color[0];
			pixels[(size_t)y * width * 4 + 4 * x + 1] = color[1];
			pixels[(size_t)y * width * 4 + 4 * x + 2] = color[2];
			pixels[(size_t)y * width * 4 + 4 * x + 3] = 255;
		}
	}
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;
	do ret = ioctl(fd, request, arg); while (ret < 0 && errno == EINTR);
	return ret;
}

static void print_encoder_input_formats(int fd, enum v4l2_buf_type type)
{
	struct v4l2_fmtdesc description = {.type = type};
	for (description.index = 0;
	     xioctl(fd, VIDIOC_ENUM_FMT, &description) == 0;
	     ++description.index)
		fprintf(stderr, "venus input format %c%c%c%c description=%s\n",
			description.pixelformat & 0xff,
			(description.pixelformat >> 8) & 0xff,
			(description.pixelformat >> 16) & 0xff,
			(description.pixelformat >> 24) & 0xff,
			description.description);
}

static int write_full(int fd, void const *data, size_t length)
{
	uint8_t const *cursor = data;
	while (length) {
		ssize_t written = write(fd, cursor, length);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) return -1;
		cursor += written;
		length -= (size_t)written;
	}
	return 0;
}

static uint32_t mpeg_crc32(uint8_t const *data, size_t length)
{
	uint32_t crc = 0xffffffffU;
	size_t index;
	for (index = 0; index < length; ++index) {
		unsigned int bit;
		crc ^= (uint32_t)data[index] << 24;
		for (bit = 0; bit < 8; ++bit)
			crc = (crc << 1) ^ ((crc & 0x80000000U) ? 0x04c11db7U : 0);
	}
	return crc;
}

static int ts_write_section(int fd, uint16_t pid, uint8_t *continuity,
			    uint8_t const *section, size_t section_length)
{
	uint8_t packet[188];
	if (section_length + 5 > sizeof(packet)) return -1;
	memset(packet, 0xff, sizeof(packet));
	packet[0] = 0x47;
	packet[1] = 0x40 | (uint8_t)(pid >> 8);
	packet[2] = (uint8_t)pid;
	packet[3] = 0x10 | (*continuity & 0x0f);
	packet[4] = 0;
	memcpy(packet + 5, section, section_length);
	*continuity = (uint8_t)((*continuity + 1) & 0x0f);
	return write_full(fd, packet, sizeof(packet));
}

static int ts_write_tables(int fd, struct ts_mux *mux)
{
	uint8_t pat[16] = {0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
		0x00, 0x01, 0xf0, 0x00};
	uint8_t pmt[21] = {0x02, 0xb0, 0x12, 0x00, 0x01, 0xc1, 0x00, 0x00,
		0xe1, 0x00, 0xf0, 0x00, 0x1b, 0xe1, 0x00, 0xf0, 0x00};
	uint32_t crc = mpeg_crc32(pat, 12);
	pat[12] = (uint8_t)(crc >> 24); pat[13] = (uint8_t)(crc >> 16);
	pat[14] = (uint8_t)(crc >> 8); pat[15] = (uint8_t)crc;
	crc = mpeg_crc32(pmt, 17);
	pmt[17] = (uint8_t)(crc >> 24); pmt[18] = (uint8_t)(crc >> 16);
	pmt[19] = (uint8_t)(crc >> 8); pmt[20] = (uint8_t)crc;
	if (ts_write_section(fd, 0x0000, &mux->pat_continuity, pat, sizeof(pat)) < 0)
		return -1;
	return ts_write_section(fd, 0x1000, &mux->pmt_continuity, pmt, sizeof(pmt));
}

static void encode_pts(uint8_t destination[5], uint64_t pts)
{
	pts &= (1ULL << 33) - 1;
	destination[0] = 0x21 | (uint8_t)((pts >> 29) & 0x0e);
	destination[1] = (uint8_t)(pts >> 22);
	destination[2] = (uint8_t)(((pts >> 14) & 0xfe) | 1);
	destination[3] = (uint8_t)(pts >> 7);
	destination[4] = (uint8_t)(((pts << 1) & 0xfe) | 1);
}

static void encode_pcr(uint8_t destination[6], uint64_t base)
{
	base &= (1ULL << 33) - 1;
	destination[0] = (uint8_t)(base >> 25);
	destination[1] = (uint8_t)(base >> 17);
	destination[2] = (uint8_t)(base >> 9);
	destination[3] = (uint8_t)(base >> 1);
	destination[4] = (uint8_t)(((base & 1) << 7) | 0x7e);
	destination[5] = 0;
}

static void put_be32(uint8_t *destination, uint32_t value)
{
	destination[0] = (uint8_t)(value >> 24);
	destination[1] = (uint8_t)(value >> 16);
	destination[2] = (uint8_t)(value >> 8);
	destination[3] = (uint8_t)value;
}

static void put_be64(uint8_t *destination, uint64_t value)
{
	put_be32(destination, (uint32_t)(value >> 32));
	put_be32(destination + 4, (uint32_t)value);
}

static int framed_write_access_unit(int fd, struct ts_mux *mux,
					uint8_t const *access_unit, size_t length,
					uint64_t source_ns)
{
	uint8_t header[32] = {0};
	memcpy(header, "MH264FRM", 8);
	header[8] = 1;
	put_be64(header + 12, mux->access_unit);
	put_be64(header + 20, source_ns);
	put_be32(header + 28, (uint32_t)length);
	if (write_full(fd, header, sizeof(header)) < 0 ||
		write_full(fd, access_unit, length) < 0)
		return -1;
	++mux->access_unit;
	return 0;
}

static int ts_write_access_unit(int fd, struct ts_mux *mux,
				uint8_t const *access_unit, size_t access_unit_length)
{
	uint8_t pes_header[14] = {0x00, 0x00, 0x01, 0xe0, 0x00, 0x00,
		0x80, 0x80, 0x05};
	uint64_t pts = mux->access_unit * 90000ULL / mux->fps;
	size_t total = sizeof(pes_header) + access_unit_length;
	size_t position = 0;
	int first = 1;
	if (mux->access_unit % mux->fps == 0 && ts_write_tables(fd, mux) < 0)
		return -1;
	encode_pts(pes_header + 9, pts);
	while (position < total) {
		uint8_t packet[188];
		size_t maximum_payload = first ? 176 : 184;
		size_t remaining = total - position;
		size_t payload = remaining < maximum_payload ? remaining : maximum_payload;
		size_t cursor = 4;
		int adaptation = first || payload < 184;
		memset(packet, 0xff, sizeof(packet));
		packet[0] = 0x47;
		packet[1] = (first ? 0x40 : 0x00) | 0x01;
		packet[2] = 0x00;
		packet[3] = (adaptation ? 0x30 : 0x10) | (mux->video_continuity & 0x0f);
		mux->video_continuity = (uint8_t)((mux->video_continuity + 1) & 0x0f);
		if (adaptation) {
			size_t adaptation_length = 183 - payload;
			packet[cursor++] = (uint8_t)adaptation_length;
			if (adaptation_length) {
				packet[cursor++] = first ? 0x10 : 0x00;
				if (first) {
					encode_pcr(packet + cursor, pts);
					cursor += 6;
				}
				cursor += adaptation_length - 1 - (first ? 6 : 0);
			}
		}
		while (payload) {
			size_t header_remaining = position < sizeof(pes_header) ?
				sizeof(pes_header) - position : 0;
			size_t chunk = header_remaining ? header_remaining : payload;
			if (chunk > payload) chunk = payload;
			if (header_remaining)
				memcpy(packet + cursor, pes_header + position, chunk);
			else
				memcpy(packet + cursor,
					access_unit + position - sizeof(pes_header), chunk);
			cursor += chunk;
			position += chunk;
			payload -= chunk;
		}
		if (write_full(fd, packet, sizeof(packet)) < 0) return -1;
		first = 0;
	}
	++mux->access_unit;
	return 0;
}

static int write_access_unit(int fd, struct ts_mux *mux,
				     void const *data, size_t length, uint64_t source_ns)
{
	if (mux->enabled)
		return ts_write_access_unit(fd, mux, data, length);
	if (mux->framed)
		return framed_write_access_unit(fd, mux, data, length, source_ns);
	++mux->access_unit;
	return write_full(fd, data, length);
}

static int set_ctrl(int fd, uint32_t id, int value)
{
	struct v4l2_control control = {.id = id, .value = value};
	if (xioctl(fd, VIDIOC_S_CTRL, &control) == 0) {
		fprintf(stderr, "control 0x%x=%d accepted\n", id, value);
		return 0;
	}
	fprintf(stderr, "control 0x%x=%d: %s (continuing)\n", id, value,
		strerror(errno));
	return -1;
}

static int alloc_ion(int ionfd, size_t len, struct mapped_buffer *buffer,
		     unsigned int plane)
{
	struct ion_allocation_data alloc = {.len = (len + 4095) & ~4095UL,
		.align = 4096, .heap_id_mask = ION_SYSTEM_HEAP_MASK};
	struct ion_fd_data share;
	if (xioctl(ionfd, ION_IOC_ALLOC, &alloc) < 0) return -1;
	memset(&share, 0, sizeof(share));
	share.handle = alloc.handle;
	if (xioctl(ionfd, ION_IOC_SHARE, &share) < 0) return -1;
	buffer->fd[plane] = share.fd;
	buffer->len[plane] = alloc.len;
	buffer->ptr[plane] = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE,
		MAP_SHARED, share.fd, 0);
	return buffer->ptr[plane] == MAP_FAILED ? -1 : 0;
}

static int map_queue(int fd, int ionfd, enum v4l2_buf_type type,
		     unsigned int num_planes, size_t const sizes[NPLANE],
		     struct mapped_buffer buffers[NBUF])
{
	struct v4l2_requestbuffers request = {.count = NBUF, .type = type,
		.memory = V4L2_MEMORY_USERPTR};
	unsigned int i, plane;
	if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) return -1;
	for (i = 0; i < request.count; ++i) {
		buffers[i].planes = num_planes;
		for (plane = 0; plane < num_planes; ++plane)
			if (alloc_ion(ionfd, sizes[plane], &buffers[i], plane) < 0)
				return -1;
	}
	return (int)request.count;
}

static int load_mir(struct mir_api *api)
{
#define LOAD(field, symbol) do { \
	*(void **)(&api->field) = dlsym(api->library, symbol); \
	if (!api->field) { fprintf(stderr, "missing Mir symbol %s\n", symbol); return -1; } \
} while (0)
	memset(api, 0, sizeof(*api));
	api->library = dlopen("libmirclient.so.9", RTLD_NOW | RTLD_LOCAL);
	if (!api->library) {
		fprintf(stderr, "dlopen libmirclient.so.9: %s\n", dlerror());
		return -1;
	}
	LOAD(connect_sync, "mir_connect_sync");
	LOAD(connection_is_valid, "mir_connection_is_valid");
	LOAD(connection_get_error_message, "mir_connection_get_error_message");
	LOAD(connection_release, "mir_connection_release");
	LOAD(create_screencast_spec, "mir_create_screencast_spec");
	LOAD(spec_set_width, "mir_screencast_spec_set_width");
	LOAD(spec_set_height, "mir_screencast_spec_set_height");
	LOAD(spec_set_pixel_format, "mir_screencast_spec_set_pixel_format");
	LOAD(spec_set_capture_region, "mir_screencast_spec_set_capture_region");
	LOAD(spec_set_number_of_buffers, "mir_screencast_spec_set_number_of_buffers");
	LOAD(spec_set_mirror_mode, "mir_screencast_spec_set_mirror_mode");
	LOAD(screencast_create_sync, "mir_screencast_create_sync");
	LOAD(screencast_spec_release, "mir_screencast_spec_release");
	LOAD(screencast_get_buffer_stream, "mir_screencast_get_buffer_stream");
	LOAD(screencast_release_sync, "mir_screencast_release_sync");
	LOAD(buffer_stream_get_graphics_region, "mir_buffer_stream_get_graphics_region");
	LOAD(buffer_stream_swap_buffers_sync, "mir_buffer_stream_swap_buffers_sync");
#undef LOAD
	return 0;
}

static void prepare_planes(struct mapped_buffer *mapped,
			   struct v4l2_plane planes[NPLANE])
{
	unsigned int plane;
	memset(planes, 0, sizeof(struct v4l2_plane) * NPLANE);
	for (plane = 0; plane < mapped->planes; ++plane) {
		planes[plane].m.userptr = (unsigned long)mapped->ptr[plane];
		planes[plane].reserved[0] = mapped->fd[plane];
		planes[plane].length = mapped->len[plane];
	}
}

static int dequeue_capture(int fd, int output_fd, enum v4l2_buf_type type,
			   struct mapped_buffer buffers[NBUF], unsigned int planes,
			   uint64_t *bytes, uint64_t *access_units,
			   size_t *maximum_access_unit, struct ts_mux *mux,
			   struct frame_meta metadata[64], unsigned int *metadata_head,
			   unsigned int *metadata_tail, struct phone_stats *stats)
{
	for (;;) {
		struct v4l2_plane p[NPLANE];
		struct v4l2_buffer buffer = {.type = type,
			.memory = V4L2_MEMORY_USERPTR, .length = planes, .m.planes = p};
		memset(p, 0, sizeof(p));
		if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN) return 0;
			perror("DQBUF capture");
			return -1;
		}
		if (p[0].bytesused) {
			struct frame_meta meta = {0};
			if (*metadata_head != *metadata_tail) {
				meta = metadata[*metadata_head % 64];
				++*metadata_head;
			}
			if (p[0].bytesused > *maximum_access_unit)
				*maximum_access_unit = p[0].bytesused;
			uint64_t encode_end_ns = monotonic_ns();
			uint64_t send_start_ns = monotonic_ns();
			int send_result = write_access_unit(output_fd, mux,
				buffers[buffer.index].ptr[0], p[0].bytesused, meta.source_ns);
			uint64_t send_end_ns = monotonic_ns();
			if (send_result < 0) {
				perror("write H264");
				return -1;
			}
			if (meta.submit_ns && stats->sample_count < PHONE_SAMPLE_COUNT) {
				struct phone_sample *sample = &stats->samples[stats->sample_count++];
				sample->source_interval_ns = stats->previous_source_ns &&
					meta.source_ns > stats->previous_source_ns ?
					meta.source_ns - stats->previous_source_ns : 0;
				sample->conversion_ns = meta.conversion_end_ns - meta.conversion_start_ns;
				sample->encode_ns = encode_end_ns - meta.submit_ns;
				sample->send_ns = send_end_ns - send_start_ns;
				sample->total_ns = send_end_ns - meta.source_ns;
				stats->previous_source_ns = meta.source_ns;
			}
			++stats->encoder_completed;
			++stats->transport_sent;
			*bytes += p[0].bytesused;
			++*access_units;
		}
		prepare_planes(&buffers[buffer.index], p);
		buffer.length = buffers[buffer.index].planes;
		buffer.m.planes = p;
		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("re-QBUF capture");
			return -1;
		}
	}
}

static int dequeue_output(int fd, enum v4l2_buf_type type,
			  struct mapped_buffer buffers[NBUF], unsigned char free_map[NBUF],
			  unsigned int *in_flight)
{
	for (;;) {
		struct v4l2_plane p[NPLANE];
		struct v4l2_buffer buffer = {.type = type,
			.memory = V4L2_MEMORY_USERPTR, .length = buffers[0].planes, .m.planes = p};
		memset(p, 0, sizeof(p));
		if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN) return 0;
			perror("DQBUF output");
			return -1;
		}
		if (buffer.index >= NBUF || free_map[buffer.index]) {
			fprintf(stderr, "invalid output completion index=%u\n", buffer.index);
			return -1;
		}
		free_map[buffer.index] = 1;
		if (*in_flight) --*in_flight;
	}
}

static int next_free(unsigned char const free_map[NBUF], unsigned int count)
{
	unsigned int index;
	for (index = 0; index < count; ++index)
		if (free_map[index]) return (int)index;
	return -1;
}

static uint64_t phone_sample_value(struct phone_sample const *sample, int metric)
{
	switch (metric) {
	case 0: return sample->source_interval_ns;
	case 1: return sample->conversion_ns;
	case 2: return sample->encode_ns;
	case 3: return sample->send_ns;
	default: return sample->total_ns;
	}
}

static void report_phone_metric(struct phone_stats const *stats, char const *name,
	int metric)
{
	uint64_t values[PHONE_SAMPLE_COUNT];
	uint64_t sum = 0, maximum = 0;
	unsigned int i, p50, p95;
	if (!stats->sample_count) return;
	for (i = 0; i < stats->sample_count; ++i) {
		values[i] = phone_sample_value(&stats->samples[i], metric);
		sum += values[i];
		if (values[i] > maximum) maximum = values[i];
	}
	for (i = 1; i < stats->sample_count; ++i) {
		uint64_t value = values[i];
		unsigned int j = i;
		while (j && values[j - 1] > value) {
			values[j] = values[j - 1];
			--j;
		}
		values[j] = value;
	}
	p50 = stats->sample_count / 2;
	p95 = (stats->sample_count * 95) / 100;
	if (p95 >= stats->sample_count) p95 = stats->sample_count - 1;
	fprintf(stderr, "phone_timing metric=%s count=%u mean_us=%.1f p50_us=%.1f "
		"p95_us=%.1f max_us=%.1f\n", name, stats->sample_count,
		sum / 1000.0 / stats->sample_count, values[p50] / 1000.0,
		values[p95] / 1000.0, maximum / 1000.0);
}

static void report_phone_stats(struct phone_stats const *stats, unsigned int submitted)
{
	fprintf(stderr, "phone_counts mir=%llu encoder_submitted=%llu encoder_completed=%llu "
		"transport_sent=%llu samples=%u submitted=%u\n",
		(unsigned long long)stats->mir_frames,
		(unsigned long long)stats->encoder_submitted,
		(unsigned long long)stats->encoder_completed,
		(unsigned long long)stats->transport_sent,
		stats->sample_count, submitted);
	report_phone_metric(stats, "mir_interval", 0);
	report_phone_metric(stats, "rgb_to_nv12", 1);
	report_phone_metric(stats, "venus_encode", 2);
	report_phone_metric(stats, "transport_send", 3);
	report_phone_metric(stats, "phone_total", 4);
}

int main(int argc, char **argv)
{
	char const *output_path = argc > 1 ? argv[1] : "/tmp/mir-venus.h264";
	unsigned int frame_limit = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 900;
	unsigned int width = argc > 3 ? (unsigned int)strtoul(argv[3], NULL, 10) : 1920;
	unsigned int height = argc > 4 ? (unsigned int)strtoul(argv[4], NULL, 10) : 1080;
	int left = argc > 5 ? (int)strtol(argv[5], NULL, 10) : 1080;
	unsigned int target_fps = argc > 6 ? (unsigned int)strtoul(argv[6], NULL, 10) : 30;
	char const *transport = argc > 7 ? argv[7] : "annexb";
	unsigned int bitrate = argc > 8 ? (unsigned int)strtoul(argv[8], NULL, 10) : 15000000;
	unsigned int capture_width = argc > 9 ? (unsigned int)strtoul(argv[9], NULL, 10) : width;
	unsigned int capture_height = argc > 10 ? (unsigned int)strtoul(argv[10], NULL, 10) : height;
	struct ts_mux mux = {0};
	struct mir_api mir;
	MirConnection *connection = NULL;
	MirScreencastSpec *spec = NULL;
	MirScreencast *screencast = NULL;
	MirBufferStream *stream = NULL;
	MirRectangle rectangle = {left, 0, capture_width, capture_height};
	struct mapped_buffer output[NBUF] = {0}, capture[NBUF] = {0};
	uint8_t *test_pattern = NULL;
	struct v4l2_format format = {0};
	struct v4l2_streamparm parm = {0};
	enum v4l2_buf_type output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	enum v4l2_buf_type capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	size_t output_sizes[NPLANE] = {0}, capture_sizes[NPLANE] = {0};
	unsigned int output_planes, capture_planes, index, plane;
	int encoder_fd = -1, ion_fd = -1, output_fd = -1;
	int output_count, capture_count, result = EXIT_FAILURE;
	unsigned char output_free[NBUF] = {0};
	unsigned int in_flight = 0, max_in_flight = 0, submitted = 0;
	uint64_t bytes = 0, access_units = 0, copy_ns = 0, swap_ns = 0;
	size_t maximum_access_unit = 0;
	uint64_t start_ns, end_ns;
	struct frame_meta metadata[64] = {0};
	unsigned int metadata_head = 0, metadata_tail = 0;
	struct phone_stats phone_stats = {0};

	if (!frame_limit || !width || !height || !capture_width || !capture_height ||
	    target_fps > 240 ||
	    (strcmp(transport, "annexb") && strcmp(transport, "mpegts") &&
	     strcmp(transport, "framed")) || !bitrate) {
		fprintf(stderr, "usage: %s [output|-] [frames] [width] [height] [capture-left] [fps] [annexb|mpegts|framed] [bitrate] [capture-width] [capture-height]\n", argv[0]);
		return EXIT_FAILURE;
	}
	mux.enabled = !strcmp(transport, "mpegts");
	mux.framed = !strcmp(transport, "framed");
	mux.fps = target_fps;
	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	if (load_mir(&mir) < 0) goto done;
	connection = mir.connect_sync("/run/mir_socket", "mir-venus-h264");
	if (!connection || !mir.connection_is_valid(connection)) {
		fprintf(stderr, "Mir connection failed: %s\n", connection ?
			mir.connection_get_error_message(connection) : "null connection");
		goto done;
	}
	spec = mir.create_screencast_spec(connection);
	if (!spec) { fprintf(stderr, "cannot create Mir screencast spec\n"); goto done; }
	mir.spec_set_width(spec, width);
	mir.spec_set_height(spec, height);
	mir.spec_set_pixel_format(spec, 1); /* mir_pixel_format_abgr_8888 */
	mir.spec_set_capture_region(spec, &rectangle);
	/*
	 * This is the Mir external-output contract used by mirgud.  Keep it on the
	 * standalone encoder too, so it requests the same virtual-output surface
	 * when it is used as the H.264 replacement child.
	 * mir_mirror_mode_vertical is 1 in the legacy Mir client ABI.
	 */
	mir.spec_set_mirror_mode(spec, 1);
	mir.spec_set_number_of_buffers(spec, 2);
	screencast = mir.screencast_create_sync(spec);
	mir.screencast_spec_release(spec);
	spec = NULL;
	if (!screencast) { fprintf(stderr, "cannot create Mir screencast\n"); goto done; }
	stream = mir.screencast_get_buffer_stream(screencast);
	if (!stream) { fprintf(stderr, "Mir screencast has no stream\n"); goto done; }

	encoder_fd = open("/dev/video33", O_RDWR | O_NONBLOCK);
	ion_fd = open("/dev/ion", O_RDWR);
	output_fd = !strcmp(output_path, "-") ? STDOUT_FILENO :
		open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (encoder_fd < 0 || ion_fd < 0 || output_fd < 0) {
		perror("open encoder/ion/output");
		goto done;
	}

	format.type = capture_type;
	format.fmt.pix_mp.width = width;
	format.fmt.pix_mp.height = height;
	format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	format.fmt.pix_mp.field = V4L2_FIELD_NONE;
	format.fmt.pix_mp.num_planes = 1;
	format.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024;
	set_rec709_limited(&format.fmt.pix_mp);
	if (xioctl(encoder_fd, VIDIOC_S_FMT, &format) < 0) { perror("S_FMT capture"); goto done; }
	print_color_format("venus capture", &format.fmt.pix_mp);
	capture_planes = format.fmt.pix_mp.num_planes;
	for (plane = 0; plane < capture_planes; ++plane)
		capture_sizes[plane] = format.fmt.pix_mp.plane_fmt[plane].sizeimage;

	memset(&format, 0, sizeof(format));
	format.type = output_type;
	format.fmt.pix_mp.width = width;
	format.fmt.pix_mp.height = height;
	format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	format.fmt.pix_mp.field = V4L2_FIELD_NONE;
	set_rec709_limited(&format.fmt.pix_mp);
	print_encoder_input_formats(encoder_fd, output_type);
	if (xioctl(encoder_fd, VIDIOC_S_FMT, &format) < 0) { perror("S_FMT output"); goto done; }
	print_color_format("venus input", &format.fmt.pix_mp);
	fprintf(stderr,
		"venus accepted input=%c%c%c%c width=%u height=%u planes=%u pitch=%u size=%u\n",
		format.fmt.pix_mp.pixelformat & 0xff,
		(format.fmt.pix_mp.pixelformat >> 8) & 0xff,
		(format.fmt.pix_mp.pixelformat >> 16) & 0xff,
		(format.fmt.pix_mp.pixelformat >> 24) & 0xff,
		format.fmt.pix_mp.width, format.fmt.pix_mp.height,
		format.fmt.pix_mp.num_planes,
		format.fmt.pix_mp.plane_fmt[0].bytesperline,
		format.fmt.pix_mp.plane_fmt[0].sizeimage);
	output_planes = format.fmt.pix_mp.num_planes;
	for (plane = 0; plane < output_planes; ++plane)
		output_sizes[plane] = format.fmt.pix_mp.plane_fmt[plane].sizeimage;
	fprintf(stderr, "mir_venus config=%ux%u source=Mir-ABGR8888 encoder=Venus-NV12 output=H264 "
		"transport=%s bitrate=%u capture_buffers=%u requested_frames=%u\n",
		width, height, transport, bitrate, NBUF, frame_limit);

	parm.type = output_type;
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = target_fps;
	if (xioctl(encoder_fd, VIDIOC_S_PARM, &parm) < 0) perror("S_PARM");
	set_ctrl(encoder_fd, V4L2_CID_MPEG_VIDC_VIDEO_NUM_B_FRAMES, 0);
	set_ctrl(encoder_fd, V4L2_CID_MPEG_VIDEO_BITRATE, (int)bitrate);

	output_count = map_queue(encoder_fd, ion_fd, output_type, output_planes, output_sizes, output);
	capture_count = map_queue(encoder_fd, ion_fd, capture_type, capture_planes, capture_sizes, capture);
	if (output_count < 0 || capture_count < 0) { perror("REQBUFS/map"); goto done; }
	for (index = 0; index < (unsigned int)output_count; ++index) output_free[index] = 1;
	for (index = 0; index < (unsigned int)capture_count; ++index) {
		struct v4l2_plane p[NPLANE];
		struct v4l2_buffer buffer = {.type = capture_type,
			.memory = V4L2_MEMORY_USERPTR, .index = index,
			.length = capture[index].planes, .m.planes = p};
		prepare_planes(&capture[index], p);
		if (xioctl(encoder_fd, VIDIOC_QBUF, &buffer) < 0) { perror("QBUF capture"); goto done; }
	}
	if (xioctl(encoder_fd, VIDIOC_STREAMON, &capture_type) < 0 ||
	    xioctl(encoder_fd, VIDIOC_STREAMON, &output_type) < 0) {
		perror("STREAMON");
		goto done;
	}

	start_ns = monotonic_ns();
	while (submitted < frame_limit && !stop_requested) {
		int free_index;
		if (submitted) {
			uint64_t deadline_ns = start_ns + (uint64_t)submitted * 1000000000ULL / target_fps;
			struct timespec deadline = {(time_t)(deadline_ns / 1000000000ULL),
				(long)(deadline_ns % 1000000000ULL)};
			while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) < 0 &&
			       errno == EINTR && !stop_requested) {}
		}
		while ((free_index = next_free(output_free, (unsigned int)output_count)) < 0) {
			struct pollfd poll_fd = {.fd = encoder_fd, .events = POLLIN | POLLOUT};
			if (poll(&poll_fd, 1, 2000) <= 0) { perror("poll waiting for output"); goto done; }
		if (dequeue_capture(encoder_fd, output_fd, capture_type, capture,
				capture_planes, &bytes, &access_units, &maximum_access_unit,
				&mux, metadata, &metadata_head, &metadata_tail, &phone_stats) < 0 ||
			    dequeue_output(encoder_fd, output_type, output, output_free, &in_flight) < 0)
				goto done;
		}
		{
		MirGraphicsRegion region = {0};
			struct v4l2_plane p[NPLANE];
			struct v4l2_buffer buffer = {.type = output_type,
				.memory = V4L2_MEMORY_USERPTR, .index = (unsigned int)free_index,
				.length = output[free_index].planes, .m.planes = p};
			uint64_t source_ns, copy_start, conversion_end, submit_ns, swap_start;
			mir.buffer_stream_get_graphics_region(stream, &region);
			source_ns = monotonic_ns();
			if (!region.vaddr || region.width != (int)width || region.height != (int)height ||
			    region.stride < (int)(width * 4) || region.pixel_format != 1) {
				fprintf(stderr, "unexpected Mir region %dx%d stride=%d format=%d ptr=%p\n",
					region.width, region.height, region.stride, region.pixel_format, region.vaddr);
				goto done;
			}
			if (getenv("MIR_VENUS_TEST_PATTERN")) {
				if (!test_pattern)
					test_pattern = malloc((size_t)width * height * 4);
				if (!test_pattern) {
					fprintf(stderr, "cannot allocate test pattern\n");
					goto done;
				}
				fill_test_pattern(test_pattern, width, height);
				region.vaddr = test_pattern;
				region.stride = (int)(width * 4);
			}
			copy_start = monotonic_ns();
			mir_abgr_to_nv12(&output[free_index], &region, width, height,
				format.fmt.pix_mp.plane_fmt[0].bytesperline ?: width,
				height);
			conversion_end = monotonic_ns();
			copy_ns += conversion_end - copy_start;
			prepare_planes(&output[free_index], p);
			p[0].bytesused = output_sizes[0];
			buffer.timestamp.tv_sec = submitted / target_fps;
			buffer.timestamp.tv_usec =
				(submitted % target_fps) * (1000000U / target_fps);
			submit_ns = monotonic_ns();
			if (xioctl(encoder_fd, VIDIOC_QBUF, &buffer) < 0) { perror("QBUF output"); goto done; }
			if (metadata_tail - metadata_head >= 64) {
				fprintf(stderr, "frame metadata queue overflow\n");
				goto done;
			}
			metadata[metadata_tail % 64].sequence = submitted;
			metadata[metadata_tail % 64].source_ns = source_ns;
			metadata[metadata_tail % 64].conversion_start_ns = copy_start;
			metadata[metadata_tail % 64].conversion_end_ns = conversion_end;
			metadata[metadata_tail % 64].submit_ns = submit_ns;
			++metadata_tail;
			output_free[free_index] = 0;
			++in_flight;
			if (in_flight > max_in_flight) max_in_flight = in_flight;
			++submitted;
			++phone_stats.mir_frames;
			++phone_stats.encoder_submitted;
			swap_start = monotonic_ns();
			mir.buffer_stream_swap_buffers_sync(stream);
			swap_ns += monotonic_ns() - swap_start;
		}
			if (dequeue_capture(encoder_fd, output_fd, capture_type, capture,
				capture_planes, &bytes, &access_units, &maximum_access_unit,
				&mux, metadata, &metadata_head, &metadata_tail, &phone_stats) < 0 ||
		    dequeue_output(encoder_fd, output_type, output, output_free, &in_flight) < 0)
			goto done;
	}
	while (in_flight) {
		struct pollfd poll_fd = {.fd = encoder_fd, .events = POLLIN | POLLOUT};
		if (poll(&poll_fd, 1, 2000) <= 0) { perror("poll drain"); goto done; }
		if (dequeue_capture(encoder_fd, output_fd, capture_type, capture,
			capture_planes, &bytes, &access_units, &maximum_access_unit,
			&mux, metadata, &metadata_head, &metadata_tail, &phone_stats) < 0 ||
		    dequeue_output(encoder_fd, output_type, output, output_free, &in_flight) < 0)
			goto done;
	}
	/* Capture completions can trail the returned input buffers briefly. */
	for (index = 0; index < 20 && access_units < submitted; ++index) {
		struct pollfd poll_fd = {.fd = encoder_fd, .events = POLLIN};
		if (poll(&poll_fd, 1, 100) < 0 && errno != EINTR) { perror("poll capture drain"); goto done; }
		if (dequeue_capture(encoder_fd, output_fd, capture_type, capture,
			capture_planes, &bytes, &access_units, &maximum_access_unit,
			&mux, metadata, &metadata_head, &metadata_tail, &phone_stats) < 0) goto done;
	}
	end_ns = monotonic_ns();
	{
		double seconds = (end_ns - start_ns) / 1000000000.0;
		fprintf(stderr, "mir_venus result submitted=%u access_units=%llu bytes=%llu seconds=%.3f "
			"fps=%.2f mbps=%.2f max_access_unit=%zu copy_ms_avg=%.3f "
			"swap_ms_avg=%.3f max_in_flight=%u drained=%s\n",
			submitted, (unsigned long long)access_units, (unsigned long long)bytes, seconds,
			submitted / seconds, bytes * 8.0 / 1000000.0 / seconds, maximum_access_unit,
			copy_ns / 1000000.0 / submitted, swap_ns / 1000000.0 / submitted,
			max_in_flight, access_units == submitted ? "true" : "false");
		report_phone_stats(&phone_stats, submitted);
	}
	result = access_units == submitted ? EXIT_SUCCESS : EXIT_FAILURE;

done:
	if (encoder_fd >= 0) {
		xioctl(encoder_fd, VIDIOC_STREAMOFF, &output_type);
		xioctl(encoder_fd, VIDIOC_STREAMOFF, &capture_type);
	}
	if (output_fd >= 0 && output_fd != STDOUT_FILENO) close(output_fd);
	free(test_pattern);
	if (ion_fd >= 0) close(ion_fd);
	if (encoder_fd >= 0) close(encoder_fd);
	/*
	 * The legacy client library can wait forever in screencast_release_sync()
	 * after a root-owned CPU screencast has been consumed concurrently with
	 * Venus.  All encoder/ION resources are already stopped above.  Let the
	 * process boundary disconnect this short-lived POC client instead of
	 * turning a completed run into an unbounded shutdown.
	 */
	if (screencast) {
		fflush(NULL);
		_exit(result);
	}
	if (screencast) mir.screencast_release_sync(screencast);
	if (spec) mir.screencast_spec_release(spec);
	if (connection) mir.connection_release(connection);
	if (mir.library) dlclose(mir.library);
	return result;
}
