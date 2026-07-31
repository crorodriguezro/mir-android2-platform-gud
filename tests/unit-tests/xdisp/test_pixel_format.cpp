#include "gud_screencast_format.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace
{
TEST(MirgudPixelFormat, parses_supported_names)
{
    EXPECT_EQ(mirgud::PixelFormat::rgb565, mirgud::parse_pixel_format("rgb565"));
    EXPECT_EQ(mirgud::PixelFormat::xrgb8888, mirgud::parse_pixel_format("xrgb8888"));
    EXPECT_THROW(mirgud::parse_pixel_format("rgb888"), std::runtime_error);
}

TEST(MirgudPixelFormat, reports_transport_bytes_per_pixel)
{
    EXPECT_EQ(2u, mirgud::bytes_per_pixel(mirgud::PixelFormat::rgb565));
    EXPECT_EQ(4u, mirgud::bytes_per_pixel(mirgud::PixelFormat::xrgb8888));
}

TEST(MirgudPixelFormat, identifies_xrgb8888_direct_copy_eligibility)
{
    EXPECT_TRUE(mirgud::xrgb8888_compatible(mir_pixel_format_xrgb_8888));
    EXPECT_FALSE(mirgud::xrgb8888_compatible(mir_pixel_format_xbgr_8888));
    EXPECT_FALSE(mirgud::xrgb8888_compatible(mir_pixel_format_argb_8888));
    EXPECT_FALSE(mirgud::xrgb8888_compatible(mir_pixel_format_abgr_8888));

    EXPECT_EQ(mirgud::ConversionPath::direct_copy,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_xrgb_8888));
    EXPECT_EQ(mirgud::ConversionPath::channel_reorder,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_abgr_8888));
    EXPECT_EQ(mirgud::ConversionPath::rgb565_expand,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_rgb_565));
    EXPECT_EQ(mirgud::ConversionPath::rgb888_expand,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_rgb_888));
}

TEST(MirgudPixelFormat, direct_copy_preserves_xrgb8888_bytes)
{
    std::vector<uint8_t> const source{
        0x11, 0x22, 0x33, 0x00,
        0x44, 0x55, 0x66, 0x00};
    std::vector<uint8_t> destination(source.size());

    mirgud::copy_rows(mirgud::PixelFormat::xrgb8888, mir_pixel_format_xrgb_8888,
        source.data(), 8, 2, 1, mirgud::RowOrder::top_down, destination.data());

    EXPECT_EQ(source, destination);
}

TEST(MirgudPixelFormat, channel_reorder_produces_xrgb8888)
{
    std::vector<uint8_t> const source{
        0x33, 0x22, 0x11, 0xff,
        0x66, 0x55, 0x44, 0xff};
    std::vector<uint8_t> destination(8);

    mirgud::copy_rows(mirgud::PixelFormat::xrgb8888, mir_pixel_format_abgr_8888,
        source.data(), 8, 2, 1, mirgud::RowOrder::top_down, destination.data());

    EXPECT_EQ((std::vector<uint8_t>{
        0x11, 0x22, 0x33, 0x00,
        0x44, 0x55, 0x66, 0x00}), destination);
}

TEST(MirgudPixelFormat, packs_rgb565)
{
    std::vector<uint8_t> const source{
        0xff, 0x00, 0x00, 0xff,
        0x00, 0xff, 0x00, 0xff,
        0x00, 0x00, 0xff, 0xff};
    std::vector<uint8_t> destination(6);

    mirgud::copy_rows(mirgud::PixelFormat::rgb565, mir_pixel_format_abgr_8888,
        source.data(), 12, 3, 1, mirgud::RowOrder::top_down, destination.data());

    EXPECT_EQ(0xf800u, static_cast<uint16_t>(destination[0] | (destination[1] << 8)));
    EXPECT_EQ(0x07e0u, static_cast<uint16_t>(destination[2] | (destination[3] << 8)));
    EXPECT_EQ(0x001fu, static_cast<uint16_t>(destination[4] | (destination[5] << 8)));
}

TEST(MirgudTiming, uses_explicit_denominators_and_bounded_summary)
{
    EXPECT_EQ(0u, mirgud::average(100, 0));
    EXPECT_EQ(20u, mirgud::average(100, 5));
    EXPECT_EQ(50u, mirgud::average(100, 2));

    mirgud::TimingSummary summary;
    summary.add(10);
    summary.add(30);
    summary.add(20);
    EXPECT_EQ(3u, summary.count);
    EXPECT_EQ(60u, summary.total);
    EXPECT_EQ(10u, summary.min);
    EXPECT_EQ(30u, summary.max);
    EXPECT_EQ(20u, summary.average());
    EXPECT_GT(summary.percentile(50.0), 0u);
    EXPECT_GT(summary.percentile(95.0), 0u);
}

TEST(MirgudQuality, deterministic_pattern_converts_from_one_reference)
{
    auto const reference = mirgud::gradient_pattern(8, 2);
    EXPECT_EQ(reference, mirgud::gradient_pattern(8, 2));

    auto const rgb565 = mirgud::rgb888_to_rgb565(reference.data(), 8, 2);
    auto const xrgb8888 = mirgud::rgb888_to_xrgb8888(reference.data(), 8, 2);
    mirgud::Frame const rgb565_frame{8, 2, mirgud::PixelFormat::rgb565, rgb565};
    mirgud::Frame const xrgb8888_frame{8, 2, mirgud::PixelFormat::xrgb8888, xrgb8888};

    EXPECT_EQ(reference, mirgud::frame_to_rgb888(xrgb8888_frame));
    EXPECT_NE(reference, mirgud::frame_to_rgb888(rgb565_frame));
    EXPECT_EQ(48u, mirgud::frame_to_rgb888(rgb565_frame).size());
}
}
