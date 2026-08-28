#include "h264_rgb_to_nv12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
	enum { width = 34, height = 18, source_stride = width * 4 + 12,
		y_stride = 48, y_scanlines = 24, y_length = y_stride * y_scanlines,
		uv_length = y_stride * 12, guard = 64 };
	uint8_t source[source_stride * height];
	uint8_t scalar[y_length + uv_length + guard];
	uint8_t optimized[y_length + uv_length + guard];
	unsigned int index;
	for (index = 0; index < sizeof(source); ++index)
		source[index] = (uint8_t)(index * 37U + index / 7U + 19U);
	memset(scalar, 0xa5, sizeof(scalar));
	memset(optimized, 0xa5, sizeof(optimized));
	h264_abgr_to_nv12(scalar, y_length, scalar + y_length, uv_length,
		y_stride, y_scanlines, source, source_stride, width, height,
		H264_CONVERTER_SCALAR);
	h264_abgr_to_nv12(optimized, y_length, optimized + y_length, uv_length,
		y_stride, y_scanlines, source, source_stride, width, height,
		H264_CONVERTER_OPTIMIZED);
	if (memcmp(scalar, optimized, y_length + uv_length)) {
		for (index = 0; index < y_length + uv_length; ++index)
			if (scalar[index] != optimized[index]) {
				fprintf(stderr, "mismatch offset=%u scalar=%u optimized=%u\n",
					index, scalar[index], optimized[index]);
				break;
			}
		return 1;
	}
	for (index = y_length + uv_length; index < sizeof(scalar); ++index)
		if (scalar[index] != 0xa5 || optimized[index] != 0xa5) {
			fprintf(stderr, "guard overwritten offset=%u\n", index);
			return 1;
		}
	puts("h264_rgb_to_nv12: scalar and optimized outputs match; guards intact");
	return 0;
}
