/* Enumerate and negotiate Qualcomm Venus encoder input formats. */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long request, void *argument)
{
	int result;
	do result = ioctl(fd, request, argument); while (result < 0 && errno == EINTR);
	return result;
}

static void fourcc(char text[5], __u32 value)
{
	text[0] = value & 0xff;
	text[1] = (value >> 8) & 0xff;
	text[2] = (value >> 16) & 0xff;
	text[3] = (value >> 24) & 0xff;
	text[4] = 0;
}

static void report(char const *operation, __u32 requested, int result,
	struct v4l2_format const *format)
{
	char requested_text[5], actual_text[5];
	unsigned int plane;
	fourcc(requested_text, requested);
	fourcc(actual_text, format->fmt.pix_mp.pixelformat);
	printf("operation=%s requested=%s result=%d errno=%d actual=%s width=%u height=%u "
	       "planes=%u colorspace=%u ycbcr=%u quantization=%u xfer=%u",
		operation, requested_text, result, result < 0 ? errno : 0, actual_text,
		format->fmt.pix_mp.width, format->fmt.pix_mp.height,
		format->fmt.pix_mp.num_planes, format->fmt.pix_mp.colorspace,
		format->fmt.pix_mp.ycbcr_enc, format->fmt.pix_mp.quantization,
		format->fmt.pix_mp.xfer_func);
	for (plane = 0; plane < format->fmt.pix_mp.num_planes; ++plane)
		printf(" plane%u_bpl=%u plane%u_size=%u", plane,
			format->fmt.pix_mp.plane_fmt[plane].bytesperline,
			plane, format->fmt.pix_mp.plane_fmt[plane].sizeimage);
	putchar('\n');
}

static void negotiate(int fd, __u32 pixel_format)
{
	struct v4l2_format format = {0};
	int result;
	format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	format.fmt.pix_mp.width = 1920;
	format.fmt.pix_mp.height = 1080;
	format.fmt.pix_mp.pixelformat = pixel_format;
	format.fmt.pix_mp.field = V4L2_FIELD_NONE;
	format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	format.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
	format.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	format.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
	errno = 0;
	result = xioctl(fd, VIDIOC_TRY_FMT, &format);
	report("TRY_FMT", pixel_format, result, &format);

	format = (struct v4l2_format){0};
	format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	format.fmt.pix_mp.width = 1920;
	format.fmt.pix_mp.height = 1080;
	format.fmt.pix_mp.pixelformat = pixel_format;
	format.fmt.pix_mp.field = V4L2_FIELD_NONE;
	format.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	format.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
	format.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	format.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
	errno = 0;
	result = xioctl(fd, VIDIOC_S_FMT, &format);
	report("S_FMT", pixel_format, result, &format);
}

int main(int argc, char **argv)
{
	char const *device = argc > 1 ? argv[1] : "/dev/video33";
	struct v4l2_fmtdesc description = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	};
	static __u32 const candidates[] = {
		V4L2_PIX_FMT_NV12,
#ifdef V4L2_PIX_FMT_NV12M
		V4L2_PIX_FMT_NV12M,
#endif
		V4L2_PIX_FMT_RGB32,
		V4L2_PIX_FMT_BGR32,
#ifdef V4L2_PIX_FMT_ABGR32
		V4L2_PIX_FMT_ABGR32, V4L2_PIX_FMT_XBGR32,
		V4L2_PIX_FMT_RGBA32, V4L2_PIX_FMT_RGBX32,
		V4L2_PIX_FMT_ARGB32, V4L2_PIX_FMT_XRGB32,
#endif
		V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_BGR24,
		V4L2_PIX_FMT_RGB565,
	};
	unsigned int index;
	__u32 enumerated[32];
	unsigned int enumerated_count = 0;
	int fd = open(device, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror(device);
		return 1;
	}
	{
		struct v4l2_format capture = {0};
		capture.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		capture.fmt.pix_mp.width = 1920;
		capture.fmt.pix_mp.height = 1080;
		capture.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
		capture.fmt.pix_mp.field = V4L2_FIELD_NONE;
		capture.fmt.pix_mp.num_planes = 1;
		capture.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024;
		if (xioctl(fd, VIDIOC_S_FMT, &capture) < 0) {
			perror("S_FMT H264 capture");
			close(fd);
			return 1;
		}
	}
	for (description.index = 0;
	     xioctl(fd, VIDIOC_ENUM_FMT, &description) == 0;
	     ++description.index) {
		char text[5];
		fourcc(text, description.pixelformat);
		printf("operation=ENUM_FMT index=%u actual=%s description=%s flags=%u\n",
			description.index, text, description.description, description.flags);
		if (enumerated_count < sizeof(enumerated) / sizeof(enumerated[0]))
			enumerated[enumerated_count++] = description.pixelformat;
	}
	for (index = 0; index < enumerated_count; ++index)
		negotiate(fd, enumerated[index]);
	for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); ++index)
		negotiate(fd, candidates[index]);
	close(fd);
	return 0;
}
