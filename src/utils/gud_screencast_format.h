/* Format-aware frame representation and conversion helpers for XDISP V0.
 *
 * This header provides a pixel-format-agnostic Frame structure and
 * conversion functions for both RGB565 and XRGB8888 transport formats.
 */
#ifndef MIR_UTILS_GUD_SCREENCAST_FORMAT_H_
#define MIR_UTILS_GUD_SCREENCAST_FORMAT_H_

#include "mir_toolkit/common.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
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

/*
 * DRM_FORMAT_RGB565 = fourcc_code('R','G','1','6')
 *   = 0x52 | (0x47<<8) | (0x31<<16) | (0x36<<24) = 0x36314752
 * DRM_FORMAT_XRGB8888 = fourcc_code('X','R','2','4')
 *   = 0x58 | (0x52<<8) | (0x32<<16) | (0x34<<24) = 0x34325258
 * Verified against <drm/drm_fourcc.h>.
 */
inline uint32_t drm_format(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::rgb565:
        return 0x36314752; /* DRM_FORMAT_RGB565 */
    case PixelFormat::xrgb8888:
        return 0x34325258; /* DRM_FORMAT_XRGB8888 */
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

inline PixelFormat parse_pixel_format(std::string const& name)
{
    if (name == "rgb565")
        return PixelFormat::rgb565;
    if (name == "xrgb8888")
        return PixelFormat::xrgb8888;
    throw std::runtime_error{"pixel-format must be rgb565 or xrgb8888"};
}

/*
 * Conversion path classification.
 *
 * On this little-endian target, DRM_FORMAT_XRGB8888 has memory layout
 * [B, G, R, X] per pixel (integer value 0x00RRGGBB).
 *
 * The only Mir source format with an identical byte-for-byte memory
 * representation is mir_pixel_format_xrgb_8888 (also 0x00RRGGBB, LE
 * memory [B, G, R, X]).  All other 4-byte Mir formats require channel
 * reordering.  3-byte and RGB565 sources require full conversion.
 */
enum class ConversionPath
{
    direct_copy,       /* memcpy row: source matches DRM_FORMAT_XRGB8888 */
    channel_reorder,   /* byte shuffle for incompatible 4-byte source */
    rgb565_pack,       /* convert source to RGB565 transport */
    rgb565_expand,     /* expand RGB565 source to XRGB8888 transport */
    rgb888_expand      /* expand RGB888/BGR888 source to XRGB8888 transport */
};

inline const char* conversion_path_name(ConversionPath path)
{
    switch (path)
    {
    case ConversionPath::direct_copy:     return "direct-copy";
    case ConversionPath::channel_reorder: return "channel-reorder";
    case ConversionPath::rgb565_pack:     return "rgb565-pack";
    case ConversionPath::rgb565_expand:   return "rgb565-expand";
    case ConversionPath::rgb888_expand:   return "rgb888-expand";
    }
    return "unknown";
}

/*
 * Returns true when the Mir source format is byte-for-byte compatible with
 * DRM_FORMAT_XRGB8888 on this little-endian target.
 *
 * DRM_FORMAT_XRGB8888 memory layout (LE): [B, G, R, X]
 * mir_pixel_format_xrgb_8888 integer: 0x00RRGGBB
 *   -> LE memory: [B, G, R, X]  identical
 *
 * Other 4-byte Mir formats:
 *   mir_pixel_format_abgr_8888  -> LE: [R, G, B, A]  not compatible
 *   mir_pixel_format_xbgr_8888  -> LE: [R, G, B, X]  not compatible
 *   mir_pixel_format_argb_8888  -> LE: [B, G, R, A]  not compatible (alpha differs)
 */
inline bool xrgb8888_compatible(MirPixelFormat format)
{
    return format == mir_pixel_format_xrgb_8888;
}

inline ConversionPath conversion_path_for(PixelFormat transport_format, MirPixelFormat mir_format)
{
    if (transport_format == PixelFormat::xrgb8888)
    {
        if (xrgb8888_compatible(mir_format))
            return ConversionPath::direct_copy;
        if (mir_format == mir_pixel_format_rgb_565)
            return ConversionPath::rgb565_expand;
        if (mir_format == mir_pixel_format_rgb_888 || mir_format == mir_pixel_format_bgr_888)
            return ConversionPath::rgb888_expand;
        return ConversionPath::channel_reorder;
    }
    else /* PixelFormat::rgb565 */
    {
        if (mir_format == mir_pixel_format_rgb_565)
            return ConversionPath::direct_copy;
        return ConversionPath::rgb565_pack;
    }
}

/*
 * Bounded timing summary with min, max, average, and approximate histogram
 * for p50/p95 estimation.  No per-frame vectors are allocated.
 */
struct TimingSummary
{
    uint64_t count{};
    uint64_t total{};
    uint64_t min{};
    uint64_t max{};

    static constexpr unsigned num_buckets = 16;
    uint64_t buckets[num_buckets]{};

    static uint64_t bucket_boundary(unsigned index)
    {
        static uint64_t const boundaries[num_buckets] = {
            10, 50, 100, 500, 1000, 2000, 5000, 10000,
            20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000};
        return boundaries[index];
    }

    void add(uint64_t us)
    {
        ++count;
        total += us;
        if (count == 1 || us < min)
            min = us;
        if (us > max)
            max = us;
        for (unsigned i = 0; i < num_buckets; ++i)
        {
            if (us <= bucket_boundary(i))
            {
                ++buckets[i];
                return;
            }
        }
        ++buckets[num_buckets - 1];
    }

    uint64_t average() const
    {
        return count > 0 ? total / count : 0;
    }

    /* Approximate percentile from histogram (0.0-100.0). */
    uint64_t percentile(double pct) const
    {
        if (count == 0)
            return 0;
        uint64_t const target = static_cast<uint64_t>(count * pct / 100.0);
        uint64_t cumulative{};
        for (unsigned i = 0; i < num_buckets; ++i)
        {
            cumulative += buckets[i];
            if (cumulative > target)
                return bucket_boundary(i);
        }
        return bucket_boundary(num_buckets - 1);
    }
};

/*
 * Testable denominator helper: returns total/count or 0 when count is zero.
 */
inline uint64_t average(uint64_t total, uint64_t count)
{
    return count > 0 ? total / count : 0;
}

/*
 * Runtime source format evidence recorded from the Mir graphics region.
 */
struct SourceFormat
{
    MirPixelFormat pixel_format{mir_pixel_format_invalid};
    uint32_t width{};
    uint32_t height{};
    std::ptrdiff_t stride{};
    RowOrder row_order{RowOrder::bottom_up};
    PixelFormat transport_format{PixelFormat::rgb565};
    ConversionPath conversion_path{ConversionPath::rgb565_pack};

    const char* conversion_path_name() const
    {
        return mirgud::conversion_path_name(conversion_path);
    }
};

inline SourceFormat make_source_format(
    MirPixelFormat pixel_format, uint32_t width, uint32_t height,
    std::ptrdiff_t stride, RowOrder row_order, PixelFormat transport_format)
{
    return SourceFormat{
        pixel_format, width, height, stride, row_order, transport_format,
        conversion_path_for(transport_format, pixel_format)};
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
 * Convert a single row from a Mir pixel format to RGB565.
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
 * When the Mir source is mir_pixel_format_xrgb_8888, copy_rows() uses a
 * direct memcpy fast path instead of calling this function.
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
 *
 * When the Mir source format is mir_pixel_format_xrgb_8888 and the
 * transport format is PixelFormat::xrgb8888, a direct row memcpy is used
 * because the source memory is byte-for-byte compatible with
 * DRM_FORMAT_XRGB8888 on this little-endian target.
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
        if (format == PixelFormat::xrgb8888 && mir_format == mir_pixel_format_xrgb_8888)
        {
            std::memcpy(dest_row, row, width * bpp);
        }
        else if (format == PixelFormat::rgb565)
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

/*
 * Convert an RGB888 byte buffer (3 bytes/pixel, R then G then B) to
 * RGB565 transport format (2 bytes/pixel).
 */
inline std::vector<uint8_t> rgb888_to_rgb565(
    uint8_t const* rgb888, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 2);
    for (uint32_t y = 0; y != height; ++y)
    {
        for (uint32_t x = 0; x != width; ++x)
        {
            auto const offset = (static_cast<std::size_t>(y) * width + x) * 3;
            auto const dest = (static_cast<std::size_t>(y) * width + x) * 2;
            uint8_t r = rgb888[offset];
            uint8_t g = rgb888[offset + 1];
            uint8_t b = rgb888[offset + 2];
            *reinterpret_cast<uint16_t*>(&pixels[dest]) = rgb565(r, g, b);
        }
    }
    return pixels;
}

/*
 * Convert an RGB888 byte buffer (3 bytes/pixel, R then G then B) to
 * XRGB8888 transport format (4 bytes/pixel, LE memory: B, G, R, X).
 */
inline std::vector<uint8_t> rgb888_to_xrgb8888(
    uint8_t const* rgb888, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (uint32_t y = 0; y != height; ++y)
    {
        for (uint32_t x = 0; x != width; ++x)
        {
            auto const offset = (static_cast<std::size_t>(y) * width + x) * 3;
            auto const dest = (static_cast<std::size_t>(y) * width + x) * 4;
            uint8_t r = rgb888[offset];
            uint8_t g = rgb888[offset + 1];
            uint8_t b = rgb888[offset + 2];
            *reinterpret_cast<uint32_t*>(&pixels[dest]) = xrgb8888(r, g, b);
        }
    }
    return pixels;
}

/*
 * Convert a Frame (in any transport format) back to RGB888 for comparison.
 */
inline std::vector<uint8_t> frame_to_rgb888(Frame const& frame)
{
    std::vector<uint8_t> rgb(static_cast<std::size_t>(frame.width) * frame.height * 3);
    auto const bpp = frame.bpp();
    for (uint32_t y = 0; y != frame.height; ++y)
    {
        for (uint32_t x = 0; x != frame.width; ++x)
        {
            auto const offset = (static_cast<std::size_t>(y) * frame.width + x) * bpp;
            auto const dest = (static_cast<std::size_t>(y) * frame.width + x) * 3;
            if (frame.format == PixelFormat::rgb565)
            {
                auto const pixel = *reinterpret_cast<uint16_t const*>(&frame.pixels[offset]);
                rgb[dest] = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
                rgb[dest + 1] = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
                rgb[dest + 2] = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
            }
            else
            {
                auto const pixel = *reinterpret_cast<uint32_t const*>(&frame.pixels[offset]);
                rgb[dest] = static_cast<uint8_t>((pixel >> 16) & 0xff);
                rgb[dest + 1] = static_cast<uint8_t>((pixel >> 8) & 0xff);
                rgb[dest + 2] = static_cast<uint8_t>(pixel & 0xff);
            }
        }
    }
    return rgb;
}

/*
 * Deterministic test pattern generators.
 *
 * Each function produces a full-color RGB888 reference buffer (3 bytes/pixel,
 * R, G, B) that is identical regardless of runtime conditions.  The same
 * reference can be converted to both RGB565 and XRGB8888 to isolate
 * quantization effects from scene changes.
 */

/*
 * Smooth horizontal gradient: R ramps 0-255 across the width,
 * G and B are constant at 128.
 */
inline std::vector<uint8_t> gradient_pattern(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (uint32_t y = 0; y != height; ++y)
        for (uint32_t x = 0; x != width; ++x)
        {
            auto const offset = (static_cast<std::size_t>(y) * width + x) * 3;
            rgb[offset] = width > 1 ? static_cast<uint8_t>(x * 255 / (width - 1)) : 0;
            rgb[offset + 1] = 128;
            rgb[offset + 2] = 128;
        }
    return rgb;
}

/*
 * Color ramps: R, G, B channels each ramp independently across the width.
 */
inline std::vector<uint8_t> color_ramps_pattern(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (uint32_t y = 0; y != height; ++y)
        for (uint32_t x = 0; x != width; ++x)
        {
            auto const offset = (static_cast<std::size_t>(y) * width + x) * 3;
            rgb[offset] = width > 1 ? static_cast<uint8_t>(x * 255 / (width - 1)) : 0;
            rgb[offset + 1] = height > 1 ? static_cast<uint8_t>(y * 255 / (height - 1)) : 0;
            auto const range = (width > 1 ? width - 1 : 0) + (height > 1 ? height - 1 : 0);
            rgb[offset + 2] = range ? static_cast<uint8_t>((x + y) * 255 / range) : 0;
        }
    return rgb;
}

/*
 * High-frequency edges: narrow vertical stripes and a synthetic text-like
 * grid pattern.
 */
inline std::vector<uint8_t> high_frequency_pattern(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (uint32_t y = 0; y != height; ++y)
        for (uint32_t x = 0; x != width; ++x)
        {
            auto const offset = (static_cast<std::size_t>(y) * width + x) * 3;
            bool const stripe = (x / 4) % 2 == 0;
            bool const grid = (x % 8 == 0) || (y % 8 == 0);
            uint8_t const intensity = stripe ? 200 : 50;
            rgb[offset] = grid ? 255 : intensity;
            rgb[offset + 1] = grid ? 255 : intensity;
            rgb[offset + 2] = grid ? 255 : intensity;
        }
    return rgb;
}

/*
 * Photo-like synthetic pattern: overlapping sine waves in each channel.
 */
inline std::vector<uint8_t> photo_like_pattern(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    double const pi = 3.14159265358979323846;
    for (uint32_t y = 0; y != height; ++y)
        for (uint32_t x = 0; x != width; ++x)
        {
            auto const offset = (static_cast<std::size_t>(y) * width + x) * 3;
            double const fx = static_cast<double>(x) / width;
            double const fy = static_cast<double>(y) / height;
            rgb[offset] = static_cast<uint8_t>(
                128 + 127 * std::sin(2.0 * pi * (fx * 3.0 + fy * 1.0)));
            rgb[offset + 1] = static_cast<uint8_t>(
                128 + 127 * std::sin(2.0 * pi * (fx * 1.0 + fy * 5.0)));
            rgb[offset + 2] = static_cast<uint8_t>(
                128 + 127 * std::sin(2.0 * pi * (fx * 4.0 + fy * 2.0)));
        }
    return rgb;
}

/*
 * Write an RGB888 buffer as a P6 PPM file.
 */
inline void write_ppm_rgb888(uint8_t const* rgb, uint32_t width, uint32_t height,
    std::string const& path)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        throw std::runtime_error{"cannot open PPM dump " + path};
    output << "P6\n" << width << " " << height << "\n255\n";
    output.write(reinterpret_cast<char const*>(rgb),
        static_cast<std::streamsize>(static_cast<std::size_t>(width) * height * 3));
    if (!output)
        throw std::runtime_error{"cannot write PPM dump " + path};
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
