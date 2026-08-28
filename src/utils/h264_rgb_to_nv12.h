#ifndef MIR_UTILS_H264_RGB_TO_NV12_H_
#define MIR_UTILS_H264_RGB_TO_NV12_H_

#include <stddef.h>
#include <stdint.h>

enum h264_converter {
	H264_CONVERTER_SCALAR,
	H264_CONVERTER_OPTIMIZED,
};

void h264_abgr_to_nv12(uint8_t *y_plane, size_t y_length,
	uint8_t *uv_plane, size_t uv_length, unsigned int y_stride,
	unsigned int y_scanlines, uint8_t const *source,
	unsigned int source_stride, unsigned int width, unsigned int height,
	enum h264_converter converter);

char const *h264_converter_name(enum h264_converter converter);

#endif
