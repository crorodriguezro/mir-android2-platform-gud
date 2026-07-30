/* Format-aware frame representation and conversion helpers for XDISP V0.
 *
 * This header provides a pixel-format-agnostic Frame structure and
 * conversion functions for both RGB565 and XRGB8888 transport formats.
 */
#ifndef MIR_UTILS_GUD_SCREENCAST_FORMAT_H_
#define MIR_UTILS_GUD_SCREENCAST_FORMAT_H_

#include "mir_toolkit/common.h"

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

enum class PixelFormat
{
    rgb565,
    xrgb8888
};

inline unsigned bytes_per_pixel(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::rgb565:
        return 2;
    case PixelFormat::xrgb8888:
        return 4;
    }
    throw std::runtime_error{"unknown pixel format"};
}

inline uint32_t drm_format(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::rgb565:
        return 0x36315247; /* DRM_FORMAT_RGB565 */
    case PixelFormat::xrgb8888:
        return 0x34424758; /* DRM_FORMAT_XRGB8888 (XRGB) */
    }
    throw std::runtime_error{"unknown pixel format"};
}

inline uint8_t gud_pixel_format(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::rgb565:
        return 0x40; /* GUD_PIXEL_FORMAT_RGB565 */
    case PixelFormat::xrgb8888:
        return 0x80; /* GUD_PIXEL_FORMAT_XRGB8888 */
    }
    throw std::runtime_error{"unknown pixel format"};
}

inline const char* format_name(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::rgb565:
        return "rgb565";
    case PixelFormat::xrgb8888:
        return "xrgb8888";
    }
    return "unknown";
}

struct Frame
{
    uint32_t width{};
    uint32_t height{};
    PixelFormat format{PixelFormat::rgb565};
    std::vector<uint8_t> pixels;

    unsigned bpp() const { return bytes_per_pixel(format); }
    std::size_t size() const { return static_cast<std::size_t>(width) * height * bpp(); }
};

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

inline uint32_t xrgb8888(uint8_t r, uint8_t g, uint8_t b)
{
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

/*
 * Convert a single row from a Mir pixel format to the destination format.
 *
 * For XRGB8888, when the Mir source is already an XRGB8888-compatible 4-byte
 * format (abgr_8888, xbgr_8888, argb_8888, xrgb_8888), this performs a
 * direct row copy with channel reordering only.
 */
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

/*
 * Convert a single row from a Mir pixel format to XRGB8888.
 *
 * When the Mir source is already an XRGB8888-compatible 4-byte format,
 * this performs a direct row copy with only channel reordering.
 */
inline void convert_row_to_xrgb8888(
    MirPixelFormat format, uint8_t const* source, uint32_t* destination, std::size_t width)
{
    for (std::size_t x = 0; x != width; ++x)
    {
        uint8_t r{};
        uint8_t g{};
        uint8_t b{};
        switch (format)
        {
        case mir_pixel_format_abgr_8888: // RGBA in LE memory -> XRGB
            r = source[4*x]; g = source[4*x + 1]; b = source[4*x + 2];
            break;
        case mir_pixel_format_xbgr_8888: // RGBX in LE memory -> XRGB
            r = source[4*x]; g = source[4*x + 1]; b = source[4*x + 2];
            break;
        case mir_pixel_format_argb_8888: // BGRA in LE memory -> XRGB
            b = source[4*x]; g = source[4*x + 1]; r = source[4*x + 2];
            break;
        case mir_pixel_format_xrgb_8888: // BGRX in LE memory -> XRGB
            b = source[4*x]; g = source[4*x + 1]; r = source[4*x + 2];
            break;
        case mir_pixel_format_rgb_888:
            r = source[3*x]; g = source[3*x + 1]; b = source[3*x + 2];
            break;
        case mir_pixel_format_bgr_888:
            b = source[3*x]; g = source[3*x + 1]; r = source[3*x + 2];
            break;
        case mir_pixel_format_rgb_565:
        {
            uint16_t pixel = static_cast<uint16_t>(source[2*x] | (source[2*x + 1] << 8));
            r = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
            g = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
            b = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
            break;
        }
        default:
            throw std::runtime_error{"unsupported Mir screencast pixel format for XRGB8888 conversion"};
        }
        destination[x] = xrgb8888(r, g, b);
    }
}

/*
 * Copy rows from a Mir graphics region to a destination buffer in the
 * specified format, handling row order (top-down or bottom-up).
 */
inline void copy_rows(
    PixelFormat format, MirPixelFormat mir_format,
    uint8_t const* source, std::ptrdiff_t stride,
    std::size_t width, std::size_t height, RowOrder row_order,
    uint8_t* destination)
{
    if (height == 0)
        return;
    auto const* row = source;
    if (row_order == RowOrder::bottom_up)
        row += static_cast<std::size_t>(height - 1) * stride;

    auto const bpp = bytes_per_pixel(format);
    for (std::size_t y = 0; y != height; ++y)
    {
        auto* dest_row = destination + y * width * bpp;
        if (format == PixelFormat::rgb565)
        {
            convert_row_to_rgb565(mir_format, row,
                reinterpret_cast<uint16_t*>(dest_row), width);
        }
        else
        {
            convert_row_to_xrgb8888(mir_format, row,
                reinterpret_cast<uint32_t*>(dest_row), width);
        }
        row += row_order == RowOrder::top_down ? stride : -stride;
    }
}

/*
 * Copy rows from an already-decoded RGBA byte buffer (from EGL readback)
 * to a destination buffer in the specified format.
 */
inline void copy_rows_from_rgba(
    PixelFormat format, uint8_t const* source,
    std::size_t width, std::size_t height, RowOrder row_order,
    uint8_t* destination)
{
    if (height == 0)
        return;
    auto const* row = source;
    if (row_order == RowOrder::bottom_up)
        row += static_cast<std::size_t>(height - 1) * width * 4;

    auto const bpp = bytes_per_pixel(format);
    for (std::size_t y = 0; y != height; ++y)
    {
        auto* dest_row = destination + y * width * bpp;
        if (format == PixelFormat::rgb565)
        {
            convert_row_to_rgb565(mir_pixel_format_abgr_8888, row,
                reinterpret_cast<uint16_t*>(dest_row), width);
        }
        else
        {
            convert_row_to_xrgb8888(mir_pixel_format_abgr_8888, row,
                reinterpret_cast<uint32_t*>(dest_row), width);
        }
        row += row_order == RowOrder::top_down ? width * 4 : -width * 4;
    }
}

inline std::vector<uint8_t> checkerboard(PixelFormat format, uint32_t width, uint32_t height)
{
    auto const bpp = bytes_per_pixel(format);
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * bpp);
    for (uint32_t y = 0; y != height; ++y)
        for (uint32_t x = 0; x != width; ++x)
        {
            auto const band = (x / 80) % 3;
            auto const light = ((x / 40) + (y / 40)) % 2;
            uint8_t r, g, b;
            if (light)
            {
                if (band == 0) { r = 255; g = 255; b = 255; }
                else if (band == 1) { r = 255; g = 0; b = 255; }
                else { r = 0; g = 255; b = 255; }
            }
            else
            {
                if (band == 0) { r = 255; g = 0; b = 0; }
                else if (band == 1) { r = 0; g = 255; b = 0; }
                else { r = 0; g = 0; b = 255; }
            }
            auto const offset = (static_cast<std::size_t>(y) * width + x) * bpp;
            if (format == PixelFormat::rgb565)
            {
                *reinterpret_cast<uint16_t*>(&pixels[offset]) = rgb565(r, g, b);
            }
            else
            {
                *reinterpret_cast<uint32_t*>(&pixels[offset]) = xrgb8888(r, g, b);
            }
        }
    return pixels;
}
}

#endif
