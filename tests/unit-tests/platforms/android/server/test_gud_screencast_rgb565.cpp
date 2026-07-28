#include "src/utils/gud_screencast_rgb565.h"

#include <gtest/gtest.h>

#include <array>

TEST(GudScreencastRgb565, converts_little_endian_rgba_and_bgra_rows)
{
    std::array<uint8_t, 8> rgba{{255, 0, 0, 0, 0, 255, 0, 0}};
    std::array<uint16_t, 2> converted{};
    mirgud::convert_row_to_rgb565(mir_pixel_format_abgr_8888, rgba.data(), converted.data(), converted.size());
    EXPECT_EQ(converted[0], 0xf800);
    EXPECT_EQ(converted[1], 0x07e0);

    std::array<uint8_t, 8> bgra{{0, 0, 255, 0, 255, 0, 0, 0}};
    mirgud::convert_row_to_rgb565(mir_pixel_format_argb_8888, bgra.data(), converted.data(), converted.size());
    EXPECT_EQ(converted[0], 0xf800);
    EXPECT_EQ(converted[1], 0x001f);
}

TEST(GudScreencastRgb565, preserves_rgb565_little_endian_input)
{
    std::array<uint8_t, 4> source{{0x00, 0xf8, 0x1f, 0x00}};
    std::array<uint16_t, 2> converted{};
    mirgud::convert_row_to_rgb565(mir_pixel_format_rgb_565, source.data(), converted.data(), converted.size());
    EXPECT_EQ(converted[0], 0xf800);
    EXPECT_EQ(converted[1], 0x001f);
}

TEST(GudScreencastRgb565, copies_top_down_rows_with_stride)
{
    std::array<uint8_t, 18> source{{
        0x00, 0xf8, 0xe0, 0x07, 0xaa, 0xaa,
        0x1f, 0x00, 0xff, 0xff, 0xbb, 0xbb,
        0xe0, 0x07, 0x00, 0xf8, 0xcc, 0xcc}};
    std::array<uint16_t, 6> converted{};

    mirgud::copy_rows_to_rgb565(mir_pixel_format_rgb_565, source.data(), 6, 2, 3,
        mirgud::RowOrder::top_down, converted.data());

    EXPECT_EQ(converted, (std::array<uint16_t, 6>{{0xf800, 0x07e0, 0x001f, 0xffff, 0x07e0, 0xf800}}));
}

TEST(GudScreencastRgb565, copies_bottom_up_rows_with_stride)
{
    std::array<uint8_t, 18> source{{
        0x00, 0xf8, 0xe0, 0x07, 0xaa, 0xaa,
        0x1f, 0x00, 0xff, 0xff, 0xbb, 0xbb,
        0xe0, 0x07, 0x00, 0xf8, 0xcc, 0xcc}};
    std::array<uint16_t, 6> converted{};

    mirgud::copy_rows_to_rgb565(mir_pixel_format_rgb_565, source.data(), 6, 2, 3,
        mirgud::RowOrder::bottom_up, converted.data());

    EXPECT_EQ(converted, (std::array<uint16_t, 6>{{0x07e0, 0xf800, 0x001f, 0xffff, 0xf800, 0x07e0}}));
}
