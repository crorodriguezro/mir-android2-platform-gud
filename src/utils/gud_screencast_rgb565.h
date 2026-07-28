/* Small, CPU-only conversion helper for the virtual-to-GUD V0 client POC. */
#ifndef MIR_UTILS_GUD_SCREENCAST_RGB565_H_
#define MIR_UTILS_GUD_SCREENCAST_RGB565_H_

#include <mir_toolkit/common.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mirgud
{
enum class RowOrder
{
    top_down,
    bottom_up
};

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

inline void convert_row_to_rgb565(
    MirPixelFormat format, uint8_t const* source, uint16_t* destination, std::size_t width)
{
    for (std::size_t x = 0; x != width; ++x)
    {
        uint8_t r{};
        uint8_t g{};
        uint8_t b{};
        switch (format)
        {
        case mir_pixel_format_abgr_8888: // RGBA in little-endian memory
        case mir_pixel_format_xbgr_8888: // RGBX in little-endian memory
            r = source[4*x]; g = source[4*x + 1]; b = source[4*x + 2];
            break;
        case mir_pixel_format_argb_8888: // BGRA in little-endian memory
        case mir_pixel_format_xrgb_8888: // BGRX in little-endian memory
            b = source[4*x]; g = source[4*x + 1]; r = source[4*x + 2];
            break;
        case mir_pixel_format_rgb_888:
            r = source[3*x]; g = source[3*x + 1]; b = source[3*x + 2];
            break;
        case mir_pixel_format_bgr_888:
            b = source[3*x]; g = source[3*x + 1]; r = source[3*x + 2];
            break;
        case mir_pixel_format_rgb_565:
            destination[x] = static_cast<uint16_t>(source[2*x] | (source[2*x + 1] << 8));
            continue;
        default:
            throw std::runtime_error{"unsupported Mir screencast pixel format for RGB565 conversion"};
        }
        destination[x] = rgb565(r, g, b);
    }
}

inline void copy_rows_to_rgb565(
    MirPixelFormat format, uint8_t const* source, std::ptrdiff_t stride,
    std::size_t width, std::size_t height, RowOrder row_order, uint16_t* destination)
{
    if (height == 0)
        return;
    auto const* row = source;
    if (row_order == RowOrder::bottom_up)
        row += static_cast<std::size_t>(height - 1) * stride;

    for (std::size_t y = 0; y != height; ++y)
    {
        convert_row_to_rgb565(format, row, destination + y * width, width);
        row += row_order == RowOrder::top_down ? stride : -stride;
    }
}

inline std::vector<uint16_t> checkerboard_rgb565(uint32_t width, uint32_t height)
{
    std::vector<uint16_t> pixels(static_cast<std::size_t>(width) * height);
    for (uint32_t y = 0; y != height; ++y)
        for (uint32_t x = 0; x != width; ++x)
        {
            auto const band = (x / 80) % 3;
            auto const light = ((x / 40) + (y / 40)) % 2;
            pixels[static_cast<std::size_t>(y) * width + x] = light ?
                (band == 0 ? rgb565(255, 255, 255) : band == 1 ? rgb565(255, 0, 255) : rgb565(0, 255, 255)) :
                (band == 0 ? rgb565(255, 0, 0) : band == 1 ? rgb565(0, 255, 0) : rgb565(0, 0, 255));
        }
    return pixels;
}
}

#endif
