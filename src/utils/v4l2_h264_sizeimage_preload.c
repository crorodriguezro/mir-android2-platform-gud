/*
 * Raise the compressed H.264 input allocation requested by FFmpeg's V4L2 M2M
 * backend.  Raspberry Pi's bcm2835-codec accepts a larger sizeimage, but the
 * distro FFmpeg build otherwise selects 1,632,256 bytes and rejects larger
 * access units before they ever reach the driver.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>

#define H264_SIZEIMAGE (2U * 1024U * 1024U)

typedef int (*ioctl_function)(int, unsigned long, void *);

int ioctl(int fd, unsigned long request, ...)
{
	static ioctl_function real_ioctl;
	va_list arguments;
	void *argument;
	int changed = 0;
	int result;

	va_start(arguments, request);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	if (!real_ioctl)
		*(void **)(&real_ioctl) = dlsym(RTLD_NEXT, "ioctl");
	if (!real_ioctl) return -1;

	if (request == VIDIOC_S_FMT && argument) {
		struct v4l2_format *format = argument;
		if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
		    format->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_H264 &&
		    format->fmt.pix_mp.plane_fmt[0].sizeimage < H264_SIZEIMAGE) {
			format->fmt.pix_mp.plane_fmt[0].sizeimage = H264_SIZEIMAGE;
			changed = 1;
		}
	}
	result = real_ioctl(fd, request, argument);
	if (changed)
		fprintf(stderr, "h264-sizeimage-shim requested=%u accepted=%u result=%d\n",
			H264_SIZEIMAGE,
			((struct v4l2_format *)argument)->fmt.pix_mp.plane_fmt[0].sizeimage,
			result);
	return result;
}
