#include "h264_rgb_to_nv12.h"

#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

static uint8_t clamp8(int value)
{
	return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static void convert_scalar(uint8_t *y_plane, uint8_t *uv_plane,
	unsigned int y_stride, uint8_t const *source, unsigned int source_stride,
	unsigned int width, unsigned int height)
{
	unsigned int x, y;
	for (y = 0; y < height; ++y) {
		uint8_t const *row = source + (size_t)(height - 1 - y) * source_stride;
		for (x = 0; x < width; ++x) {
			uint8_t const *pixel = row + 4 * x;
			y_plane[(size_t)y * y_stride + x] =
				clamp8(((47 * pixel[0] + 157 * pixel[1] +
					16 * pixel[2] + 128) >> 8) + 16);
		}
	}
	for (y = 0; y < height; y += 2) {
		uint8_t const *row0 = source + (size_t)(height - 1 - y) * source_stride;
		uint8_t const *row1 = source + (size_t)(height - 2 - y) * source_stride;
		uint8_t *destination = uv_plane + (size_t)(y / 2) * y_stride;
		for (x = 0; x < width; x += 2) {
			unsigned int red = row0[4*x] + row0[4*(x+1)] +
				row1[4*x] + row1[4*(x+1)];
			unsigned int green = row0[4*x+1] + row0[4*(x+1)+1] +
				row1[4*x+1] + row1[4*(x+1)+1];
			unsigned int blue = row0[4*x+2] + row0[4*(x+1)+2] +
				row1[4*x+2] + row1[4*(x+1)+2];
			red = (red + 2) / 4;
			green = (green + 2) / 4;
			blue = (blue + 2) / 4;
			destination[x] = clamp8(((-26 * (int)red - 87 * (int)green +
				112 * (int)blue + 128) >> 8) + 128);
			destination[x + 1] = clamp8(((112 * (int)red - 102 * (int)green -
				10 * (int)blue + 128) >> 8) + 128);
		}
	}
}

#if defined(__aarch64__)
static uint8x8_t chroma8(uint16x8_t red, uint16x8_t green,
	uint16x8_t blue, int red_coefficient, int green_coefficient,
	int blue_coefficient)
{
	int16x8_t r = vreinterpretq_s16_u16(red);
	int16x8_t g = vreinterpretq_s16_u16(green);
	int16x8_t b = vreinterpretq_s16_u16(blue);
	int32x4_t low = vmull_n_s16(vget_low_s16(r), (int16_t)red_coefficient);
	int32x4_t high = vmull_high_n_s16(r, (int16_t)red_coefficient);
	low = vmlal_n_s16(low, vget_low_s16(g), (int16_t)green_coefficient);
	high = vmlal_high_n_s16(high, g, (int16_t)green_coefficient);
	low = vmlal_n_s16(low, vget_low_s16(b), (int16_t)blue_coefficient);
	high = vmlal_high_n_s16(high, b, (int16_t)blue_coefficient);
	low = vaddq_s32(low, vdupq_n_s32(128));
	high = vaddq_s32(high, vdupq_n_s32(128));
	return vqmovun_s16(vaddq_s16(vcombine_s16(vshrn_n_s32(low, 8),
		vshrn_n_s32(high, 8)), vdupq_n_s16(128)));
}

static void convert_neon(uint8_t *y_plane, uint8_t *uv_plane,
	unsigned int y_stride, uint8_t const *source, unsigned int source_stride,
	unsigned int width, unsigned int height)
{
	unsigned int x, y;
	for (y = 0; y < height; ++y) {
		uint8_t const *row = source + (size_t)(height - 1 - y) * source_stride;
		uint8_t *destination = y_plane + (size_t)y * y_stride;
		for (x = 0; x + 16 <= width; x += 16) {
			uint8x16x4_t rgba = vld4q_u8(row + 4 * x);
			uint16x8_t low = vmull_u8(vget_low_u8(rgba.val[0]), vdup_n_u8(47));
			uint16x8_t high = vmull_high_u8(rgba.val[0], vdupq_n_u8(47));
			low = vmlal_u8(low, vget_low_u8(rgba.val[1]), vdup_n_u8(157));
			high = vmlal_high_u8(high, rgba.val[1], vdupq_n_u8(157));
			low = vmlal_u8(low, vget_low_u8(rgba.val[2]), vdup_n_u8(16));
			high = vmlal_high_u8(high, rgba.val[2], vdupq_n_u8(16));
			low = vaddq_u16(low, vdupq_n_u16(128));
			high = vaddq_u16(high, vdupq_n_u16(128));
			vst1q_u8(destination + x, vaddq_u8(vcombine_u8(
				vshrn_n_u16(low, 8), vshrn_n_u16(high, 8)), vdupq_n_u8(16)));
		}
		for (; x < width; ++x) {
			uint8_t const *pixel = row + 4 * x;
			destination[x] = clamp8(((47 * pixel[0] + 157 * pixel[1] +
				16 * pixel[2] + 128) >> 8) + 16);
		}
	}
	for (y = 0; y < height; y += 2) {
		uint8_t const *row0 = source + (size_t)(height - 1 - y) * source_stride;
		uint8_t const *row1 = source + (size_t)(height - 2 - y) * source_stride;
		uint8_t *destination = uv_plane + (size_t)(y / 2) * y_stride;
		for (x = 0; x + 16 <= width; x += 16) {
			uint8x16x4_t upper = vld4q_u8(row0 + 4 * x);
			uint8x16x4_t lower = vld4q_u8(row1 + 4 * x);
			uint16x8_t red = vrshrq_n_u16(vaddq_u16(vpaddlq_u8(upper.val[0]),
				vpaddlq_u8(lower.val[0])), 2);
			uint16x8_t green = vrshrq_n_u16(vaddq_u16(vpaddlq_u8(upper.val[1]),
				vpaddlq_u8(lower.val[1])), 2);
			uint16x8_t blue = vrshrq_n_u16(vaddq_u16(vpaddlq_u8(upper.val[2]),
				vpaddlq_u8(lower.val[2])), 2);
			uint8x8x2_t uv = {{chroma8(red, green, blue, -26, -87, 112),
				chroma8(red, green, blue, 112, -102, -10)}};
			vst2_u8(destination + x, uv);
		}
		for (; x < width; x += 2) {
			unsigned int red = row0[4*x] + row0[4*(x+1)] +
				row1[4*x] + row1[4*(x+1)];
			unsigned int green = row0[4*x+1] + row0[4*(x+1)+1] +
				row1[4*x+1] + row1[4*(x+1)+1];
			unsigned int blue = row0[4*x+2] + row0[4*(x+1)+2] +
				row1[4*x+2] + row1[4*(x+1)+2];
			red = (red + 2) / 4; green = (green + 2) / 4; blue = (blue + 2) / 4;
			destination[x] = clamp8(((-26 * (int)red - 87 * (int)green +
				112 * (int)blue + 128) >> 8) + 128);
			destination[x + 1] = clamp8(((112 * (int)red - 102 * (int)green -
				10 * (int)blue + 128) >> 8) + 128);
		}
	}
}
#endif

void h264_abgr_to_nv12(uint8_t *y_plane, size_t y_length,
	uint8_t *uv_plane, size_t uv_length, unsigned int y_stride,
	unsigned int y_scanlines, uint8_t const *source,
	unsigned int source_stride, unsigned int width, unsigned int height,
	enum h264_converter converter)
{
	memset(y_plane, 16, y_length);
	if (!uv_plane) {
		uv_plane = y_plane + (size_t)y_stride * y_scanlines;
		uv_length = y_length - (size_t)y_stride * y_scanlines;
	}
	memset(uv_plane, 128, uv_length);
#if defined(__aarch64__)
	if (converter == H264_CONVERTER_OPTIMIZED) {
		convert_neon(y_plane, uv_plane, y_stride, source, source_stride, width, height);
		return;
	}
#else
	(void)converter;
#endif
	convert_scalar(y_plane, uv_plane, y_stride, source, source_stride, width, height);
}

char const *h264_converter_name(enum h264_converter converter)
{
#if defined(__aarch64__)
	return converter == H264_CONVERTER_OPTIMIZED ? "neon" : "scalar";
#else
	(void)converter;
	return "scalar";
#endif
}
