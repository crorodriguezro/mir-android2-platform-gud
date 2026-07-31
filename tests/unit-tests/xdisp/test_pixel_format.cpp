#include "gud_screencast_format.h"
#include "gud_screencast_presenter.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <drm/drm_fourcc.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
TEST(MirgudPixelFormat, parses_supported_names)
{
    EXPECT_EQ(mirgud::PixelFormat::rgb565, mirgud::parse_pixel_format("rgb565"));
    EXPECT_EQ(mirgud::PixelFormat::xrgb8888, mirgud::parse_pixel_format("xrgb8888"));
    EXPECT_THROW(mirgud::parse_pixel_format("rgb888"), std::runtime_error);
}

TEST(MirgudSourcePixelFormat, parses_names_and_selects_advertised_format)
{
    EXPECT_EQ(mirgud::SourcePixelFormat::xrgb8888, mirgud::parse_source_pixel_format("xrgb8888"));
    EXPECT_EQ(mirgud::SourcePixelFormat::argb8888, mirgud::parse_source_pixel_format("argb8888"));
    EXPECT_EQ("abgr8888", mirgud::mir_pixel_format_name(mir_pixel_format_abgr_8888));
    EXPECT_EQ("xrgb8888", mirgud::mir_pixel_format_name(mir_pixel_format_xrgb_8888));
    EXPECT_THROW(mirgud::parse_source_pixel_format("invalid"), std::runtime_error);

    std::vector<MirPixelFormat> const formats{mir_pixel_format_abgr_8888, mir_pixel_format_argb_8888,
        mir_pixel_format_xrgb_8888};
    EXPECT_EQ(mir_pixel_format_abgr_8888,
        mirgud::select_source_pixel_format(mirgud::SourcePixelFormat::auto_select, formats));
    EXPECT_EQ(mir_pixel_format_xrgb_8888,
        mirgud::select_source_pixel_format(mirgud::SourcePixelFormat::xrgb8888, formats));
    EXPECT_THROW(mirgud::select_source_pixel_format(mirgud::SourcePixelFormat::rgb565, formats), std::runtime_error);
}

TEST(MirgudPixelFormat, reports_transport_bytes_per_pixel)
{
    EXPECT_EQ(2u, mirgud::bytes_per_pixel(mirgud::PixelFormat::rgb565));
    EXPECT_EQ(4u, mirgud::bytes_per_pixel(mirgud::PixelFormat::xrgb8888));
}

mirgud::Frame frame()
{
    return {1, 1, mirgud::PixelFormat::rgb565, {0x00, 0x00}};
}

TEST(MirgudPixelFormat, protects_drm_fourcc_mappings)
{
    EXPECT_EQ(DRM_FORMAT_RGB565, mirgud::drm_format(mirgud::PixelFormat::rgb565));
    EXPECT_EQ(DRM_FORMAT_XRGB8888, mirgud::drm_format(mirgud::PixelFormat::xrgb8888));
}

TEST(MirgudPixelFormat, identifies_xrgb8888_scanout_copy_eligibility)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    EXPECT_TRUE(mirgud::is_xrgb8888_scanout_copy_compatible(mir_pixel_format_xrgb_8888));
    EXPECT_TRUE(mirgud::is_xrgb8888_scanout_copy_compatible(mir_pixel_format_argb_8888));
#else
    EXPECT_FALSE(mirgud::is_xrgb8888_scanout_copy_compatible(mir_pixel_format_xrgb_8888));
    EXPECT_FALSE(mirgud::is_xrgb8888_scanout_copy_compatible(mir_pixel_format_argb_8888));
#endif
    EXPECT_FALSE(mirgud::is_xrgb8888_scanout_copy_compatible(mir_pixel_format_xbgr_8888));
    EXPECT_FALSE(mirgud::is_xrgb8888_scanout_copy_compatible(mir_pixel_format_abgr_8888));

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    EXPECT_EQ(mirgud::ConversionPath::direct_copy,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_xrgb_8888));
    EXPECT_EQ(mirgud::ConversionPath::direct_copy,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_argb_8888));
#else
    EXPECT_EQ(mirgud::ConversionPath::channel_reorder,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_xrgb_8888));
#endif
    EXPECT_EQ(mirgud::ConversionPath::channel_reorder,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_abgr_8888));
    EXPECT_EQ(mirgud::ConversionPath::channel_reorder,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_xbgr_8888));
    EXPECT_EQ(mirgud::ConversionPath::rgb565_expand,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_rgb_565));
    EXPECT_EQ(mirgud::ConversionPath::rgb888_expand,
        mirgud::conversion_path_for(mirgud::PixelFormat::xrgb8888, mir_pixel_format_rgb_888));
    EXPECT_EQ(mirgud::ConversionPath::direct_copy,
        mirgud::conversion_path_for(mirgud::PixelFormat::rgb565, mir_pixel_format_rgb_565));
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

TEST(MirgudPixelFormat, direct_copy_preserves_argb8888_bytes_for_visible_xrgb_channels)
{
    std::vector<uint8_t> const source{
        0x19, 0x72, 0xe1, 0xa5,
        0x03, 0x42, 0xc0, 0x5a};
    std::vector<uint8_t> destination(source.size());

    mirgud::copy_rows(mirgud::PixelFormat::xrgb8888, mir_pixel_format_argb_8888,
        source.data(), 8, 2, 1, mirgud::RowOrder::top_down, destination.data());

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    EXPECT_EQ(source, destination);
#else
    EXPECT_EQ((std::vector<uint8_t>{0x19, 0x72, 0xe1, 0x00, 0x03, 0x42, 0xc0, 0x00}), destination);
#endif
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

TEST(MirgudPixelFormat, xbgr8888_uses_channel_reorder)
{
    std::vector<uint8_t> const source{0xe1, 0x72, 0x19, 0x5a};
    std::vector<uint8_t> destination(4);

    mirgud::copy_rows(mirgud::PixelFormat::xrgb8888, mir_pixel_format_xbgr_8888,
        source.data(), 4, 1, 1, mirgud::RowOrder::top_down, destination.data());

    EXPECT_EQ((std::vector<uint8_t>{0x19, 0x72, 0xe1, 0x00}), destination);
}

TEST(MirgudPixelFormat, egl_rgba_source_converts_correctly_with_bottom_up_rows)
{
    // GL_RGBA bytes map to Mir ABGR8888 memory.
    std::vector<uint8_t> const source{
        0x10, 0x20, 0x30, 0x40,
        0xe1, 0x72, 0x19, 0xa5,
        0x01, 0x02, 0x03, 0x04,
        0x21, 0x43, 0x65, 0x87};
    std::vector<uint8_t> rgb565(4);
    std::vector<uint8_t> xrgb8888(8);

    mirgud::copy_rows(mirgud::PixelFormat::rgb565, mir_pixel_format_abgr_8888,
        source.data(), 8, 2, 2, mirgud::RowOrder::bottom_up, rgb565.data());
    mirgud::copy_rows(mirgud::PixelFormat::xrgb8888, mir_pixel_format_abgr_8888,
        source.data(), 8, 2, 2, mirgud::RowOrder::bottom_up, xrgb8888.data());

    EXPECT_EQ((std::vector<uint8_t>{0x00, 0x00, 0x0c, 0x22}), rgb565);
    EXPECT_EQ((std::vector<uint8_t>{0x03, 0x02, 0x01, 0x00, 0x65, 0x43, 0x21, 0x00}), xrgb8888);
}

TEST(MirgudPixelFormat, egl_source_layouts_convert_one_row_without_channel_swapping)
{
    std::vector<uint8_t> const rgba{0xe1, 0x72, 0x19, 0xa5};
    std::vector<uint8_t> const bgra{0x19, 0x72, 0xe1, 0xa5};
    std::vector<uint8_t> rgba_rgb565(2);
    std::vector<uint8_t> bgra_rgb565(2);
    std::vector<uint8_t> rgba_xrgb8888(4);
    std::vector<uint8_t> bgra_xrgb8888(4);

    mirgud::copy_rows(mirgud::PixelFormat::rgb565, mir_pixel_format_abgr_8888,
        rgba.data(), 4, 1, 1, mirgud::RowOrder::bottom_up, rgba_rgb565.data());
    mirgud::copy_rows(mirgud::PixelFormat::rgb565, mir_pixel_format_argb_8888,
        bgra.data(), 4, 1, 1, mirgud::RowOrder::bottom_up, bgra_rgb565.data());
    mirgud::copy_rows(mirgud::PixelFormat::xrgb8888, mir_pixel_format_abgr_8888,
        rgba.data(), 4, 1, 1, mirgud::RowOrder::bottom_up, rgba_xrgb8888.data());
    mirgud::copy_rows(mirgud::PixelFormat::xrgb8888, mir_pixel_format_argb_8888,
        bgra.data(), 4, 1, 1, mirgud::RowOrder::bottom_up, bgra_xrgb8888.data());

    EXPECT_EQ((std::vector<uint8_t>{0x83, 0xe3}), rgba_rgb565);
    EXPECT_EQ(rgba_rgb565, bgra_rgb565);
    EXPECT_EQ((std::vector<uint8_t>{0x19, 0x72, 0xe1, 0x00}), rgba_xrgb8888);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    EXPECT_EQ((std::vector<uint8_t>{0x19, 0x72, 0xe1, 0xa5}), bgra_xrgb8888);
#else
    EXPECT_EQ(rgba_xrgb8888, bgra_xrgb8888);
#endif
}

TEST(MirgudPixelFormat, egl_bgra_source_converts_correctly_with_bottom_up_rows)
{
    // GL_BGRA_EXT bytes map to Mir ARGB8888 memory.
    std::vector<uint8_t> const source{
        0x30, 0x20, 0x10, 0x40,
        0x19, 0x72, 0xe1, 0xa5,
        0x03, 0x02, 0x01, 0x04,
        0x65, 0x43, 0x21, 0x87};
    std::vector<uint8_t> rgb565(4);
    std::vector<uint8_t> xrgb8888(8);

    mirgud::copy_rows(mirgud::PixelFormat::rgb565, mir_pixel_format_argb_8888,
        source.data(), 8, 2, 2, mirgud::RowOrder::bottom_up, rgb565.data());
    mirgud::copy_rows(mirgud::PixelFormat::xrgb8888, mir_pixel_format_argb_8888,
        source.data(), 8, 2, 2, mirgud::RowOrder::bottom_up, xrgb8888.data());

    EXPECT_EQ((std::vector<uint8_t>{0x00, 0x00, 0x0c, 0x22}), rgb565);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    EXPECT_EQ((std::vector<uint8_t>{0x03, 0x02, 0x01, 0x04, 0x65, 0x43, 0x21, 0x87}), xrgb8888);
#else
    EXPECT_EQ((std::vector<uint8_t>{0x03, 0x02, 0x01, 0x00, 0x65, 0x43, 0x21, 0x00}), xrgb8888);
#endif
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

TEST(MirgudPixelFormat, rgb565_direct_copy_respects_stride_and_row_order)
{
    std::vector<uint8_t> const source{
        0x11, 0x22, 0x33, 0x44, 0xaa, 0xbb,
        0x55, 0x66, 0x77, 0x88, 0xcc, 0xdd};
    std::vector<uint8_t> top_down(8);
    std::vector<uint8_t> bottom_up(8);

    mirgud::copy_rows(mirgud::PixelFormat::rgb565, mir_pixel_format_rgb_565,
        source.data(), 6, 2, 2, mirgud::RowOrder::top_down, top_down.data());
    mirgud::copy_rows(mirgud::PixelFormat::rgb565, mir_pixel_format_rgb_565,
        source.data(), 6, 2, 2, mirgud::RowOrder::bottom_up, bottom_up.data());

    EXPECT_EQ((std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}), top_down);
    EXPECT_EQ((std::vector<uint8_t>{0x55, 0x66, 0x77, 0x88, 0x11, 0x22, 0x33, 0x44}), bottom_up);
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
    EXPECT_EQ(10u, summary.percentile(0.0));
    EXPECT_EQ(50u, summary.percentile(50.0)); // Histogram reports bucket upper bounds.
    EXPECT_EQ(50u, summary.percentile(95.0));
    EXPECT_EQ(50u, summary.percentile(100.0));
}

TEST(MirgudTiming, uses_nearest_rank_percentiles)
{
    mirgud::TimingSummary summary;
    for (unsigned i = 0; i != 20; ++i)
        summary.add(i < 10 ? 10 : 100);
    EXPECT_EQ(10u, summary.percentile(50.0));
    EXPECT_EQ(100u, summary.percentile(95.0)); // ceil(.95 * 20) == 19
    EXPECT_EQ(100u, summary.percentile(100.0));
}

TEST(MirgudTiming, calculates_zero_and_elapsed_rates)
{
    EXPECT_EQ(0u, mirgud::fps(100, 0));
    EXPECT_EQ(60u, mirgud::fps(60, 1000000));
    EXPECT_EQ(40u, mirgud::fps(100, 2500000));
    EXPECT_EQ(7u, mirgud::counter_delta(12, 5));
    EXPECT_EQ(0u, mirgud::counter_delta(5, 12));
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

TEST(MirgudPattern, deterministic_seed_and_format_conversion_share_rgb_source)
{
    auto const first = mirgud::pattern_rgb888(mirgud::PatternWorkload::noise, 8, 4, 7, 99);
    EXPECT_EQ(first, mirgud::pattern_rgb888(mirgud::PatternWorkload::noise, 8, 4, 7, 99));
    EXPECT_NE(first, mirgud::pattern_rgb888(mirgud::PatternWorkload::noise, 8, 4, 7, 100));
    EXPECT_EQ(mirgud::pattern_rgb888(mirgud::PatternWorkload::motion, 8, 4, 3, 9),
        mirgud::pattern_rgb888(mirgud::PatternWorkload::motion, 8, 4, 3, 9));

    auto const rgb565 = mirgud::pattern_frame(mirgud::PatternWorkload::checkerboard,
        mirgud::PixelFormat::rgb565, 8, 4, 0, 1);
    auto const xrgb = mirgud::pattern_frame(mirgud::PatternWorkload::checkerboard,
        mirgud::PixelFormat::xrgb8888, 8, 4, 0, 1);
    EXPECT_EQ(mirgud::frame_to_rgb888(xrgb), mirgud::pattern_rgb888(
        mirgud::PatternWorkload::checkerboard, 8, 4, 0, 1));
    EXPECT_EQ(96u, mirgud::frame_to_rgb888(rgb565).size());
    EXPECT_THROW(mirgud::parse_pattern_workload("invalid"), std::runtime_error);
}

TEST(MirgudPattern, pregenerated_frames_repeat_without_changing_logical_rgb_source)
{
    auto const rgb = mirgud::pattern_rgb888(mirgud::PatternWorkload::gradient, 8, 4, 0, 7);
    auto const stored = mirgud::pattern_frame_from_rgb888(rgb, mirgud::PixelFormat::xrgb8888, 8, 4);
    auto const submitted = mirgud::Frame{stored.width, stored.height, stored.format, stored.pixels};
    EXPECT_EQ(stored.pixels, submitted.pixels);
    EXPECT_EQ(rgb, mirgud::frame_to_rgb888(submitted));
}

TEST(MirgudSourceFormat, rejects_each_runtime_contract_change)
{
    auto const expected = mirgud::make_source_format(mir_pixel_format_argb_8888, 1280, 720, 5120,
        mirgud::RowOrder::top_down, mirgud::PixelFormat::xrgb8888);
    auto expect_change = [&](mirgud::SourceFormat actual, char const* field)
    {
        try { mirgud::validate_source_format(expected, actual); FAIL() << "expected source change"; }
        catch (std::runtime_error const& error) { EXPECT_NE(std::string{error.what()}.find(field), std::string::npos); }
    };
    auto actual = expected;
    actual.pixel_format = mir_pixel_format_abgr_8888;
    expect_change(actual, "pixel_format");
    actual = expected;
    ++actual.width;
    expect_change(actual, "width");
    actual = expected;
    ++actual.height;
    expect_change(actual, "height");
    actual = expected;
    ++actual.stride;
    expect_change(actual, "stride");
}

TEST(MirgudXByteSamples, tracks_constant_and_varying_bounded_samples)
{
    std::vector<uint8_t> zero(4 * 8, 0);
    mirgud::XByteSamples samples;
    mirgud::sample_x_bytes(zero.data(), 32, 8, 1, mirgud::RowOrder::top_down, &samples, 4);
    EXPECT_EQ(4u, samples.count);
    EXPECT_TRUE(samples.constant);
    EXPECT_EQ(0u, samples.min);
    EXPECT_EQ(4u, samples.zero);

    std::vector<uint8_t> varying(4 * 8, 0);
    for (unsigned i = 0; i != 8; ++i)
        varying[4 * i + 3] = i == 7 ? 0xff : static_cast<uint8_t>(i);
    mirgud::XByteSamples varied;
    mirgud::sample_x_bytes(varying.data(), 32, 8, 1, mirgud::RowOrder::top_down, &varied, 8);
    EXPECT_FALSE(varied.constant);
    EXPECT_EQ(0xffu, varied.max);
    EXPECT_EQ(1u, varied.ff);
}

TEST(MirgudXByteSamples, distributes_samples_over_full_frame)
{
    constexpr uint32_t width = 1280;
    constexpr uint32_t height = 720;
    std::vector<uint8_t> source(static_cast<std::size_t>(width) * height * 4);
    for (uint32_t y = 0; y != height; ++y)
        for (uint32_t x = 0; x != width; ++x)
            source[(static_cast<std::size_t>(y) * width + x) * 4 + 3] =
                y == 0 ? 1 : (y == height - 1 ? 3 : 2);
    mirgud::XByteSamples samples;
    mirgud::sample_x_bytes(source.data(), width * 4, width, height, mirgud::RowOrder::top_down, &samples, 64);
    EXPECT_EQ(64u, samples.count);
    EXPECT_EQ(1u, samples.min);
    EXPECT_EQ(3u, samples.max);
    EXPECT_FALSE(samples.constant);
}

TEST(MirgudPresenter, accounts_for_one_presented_frame)
{
    std::mutex mutex;
    std::condition_variable wakeup;
    bool presented{};
    mirgud::LatestFramePresenter presenter{[](mirgud::Frame const&) {}, [&]
    {
        std::lock_guard<std::mutex> lock{mutex};
        presented = true;
        wakeup.notify_one();
    }};
    presenter.submit(frame());
    {
        std::unique_lock<std::mutex> lock{mutex};
        wakeup.wait(lock, [&] { return presented; });
    }
    presenter.stop();

    auto const stats = presenter.stats();
    EXPECT_EQ(1u, stats.submitted);
    EXPECT_EQ(1u, stats.presented);
    EXPECT_EQ(0u, stats.dropped);
    EXPECT_EQ(0u, stats.cancelled);
    EXPECT_EQ(0u, stats.submit_failures);
    EXPECT_TRUE(mirgud::accounting_ok(stats));
}

TEST(MirgudPresenter, accounts_for_pending_replacement)
{
    std::mutex mutex;
    std::condition_variable wakeup;
    bool entered{};
    bool release{};
    unsigned calls{};
    mirgud::LatestFramePresenter presenter{[&](mirgud::Frame const&)
    {
        std::unique_lock<std::mutex> lock{mutex};
        ++calls;
        entered = calls == 1;
        wakeup.notify_one();
        if (entered)
            wakeup.wait(lock, [&] { return release; });
    }};
    presenter.submit(frame());
    {
        std::unique_lock<std::mutex> lock{mutex};
        wakeup.wait(lock, [&] { return entered; });
    }
    presenter.submit(frame());
    presenter.submit(frame());
    {
        std::lock_guard<std::mutex> lock{mutex};
        release = true;
    }
    wakeup.notify_one();
    {
        std::unique_lock<std::mutex> lock{mutex};
        wakeup.wait(lock, [&] { return calls == 2; });
    }
    presenter.stop();

    auto const stats = presenter.stats();
    EXPECT_EQ(3u, stats.submitted);
    EXPECT_EQ(2u, stats.presented);
    EXPECT_EQ(1u, stats.dropped);
    EXPECT_TRUE(mirgud::accounting_ok(stats));
}

TEST(MirgudPresenter, snapshot_is_accounting_valid_while_presenting)
{
    std::mutex mutex;
    std::condition_variable wakeup;
    bool entered{};
    bool release{};
    mirgud::LatestFramePresenter presenter{[&](mirgud::Frame const&)
    {
        std::unique_lock<std::mutex> lock{mutex};
        entered = true;
        wakeup.notify_one();
        wakeup.wait(lock, [&] { return release; });
    }};
    presenter.submit(frame());
    {
        std::unique_lock<std::mutex> lock{mutex};
        wakeup.wait(lock, [&] { return entered; });
    }
    auto const active = presenter.stats();
    EXPECT_EQ(1u, active.in_flight);
    EXPECT_EQ(0u, active.presented);
    EXPECT_TRUE(mirgud::accounting_ok(active));
    {
        std::lock_guard<std::mutex> lock{mutex};
        release = true;
    }
    wakeup.notify_one();
    presenter.stop();
    EXPECT_EQ(0u, presenter.stats().in_flight);
    EXPECT_TRUE(mirgud::accounting_ok(presenter.stats()));
}

TEST(MirgudPresenter, cancels_pending_frame_once_during_stop)
{
    std::mutex mutex;
    std::condition_variable wakeup;
    bool entered{};
    bool release{};
    mirgud::LatestFramePresenter presenter{[&](mirgud::Frame const&)
    {
        std::unique_lock<std::mutex> lock{mutex};
        entered = true;
        wakeup.notify_one();
        wakeup.wait(lock, [&] { return release; });
    }};
    presenter.submit(frame());
    {
        std::unique_lock<std::mutex> lock{mutex};
        wakeup.wait(lock, [&] { return entered; });
    }
    presenter.submit(frame());
    std::thread stopper{[&] { presenter.stop(); }};
    while (presenter.stats().cancelled == 0)
        std::this_thread::yield();
    {
        std::lock_guard<std::mutex> lock{mutex};
        release = true;
    }
    wakeup.notify_one();
    stopper.join();
    presenter.stop();

    auto const stats = presenter.stats();
    EXPECT_EQ(2u, stats.submitted);
    EXPECT_EQ(1u, stats.presented);
    EXPECT_EQ(1u, stats.cancelled);
    EXPECT_TRUE(mirgud::accounting_ok(stats));
}

TEST(MirgudPresenter, records_failed_presentation_attempts)
{
    std::mutex mutex;
    std::condition_variable wakeup;
    bool failed{};
    mirgud::LatestFramePresenter presenter{[](mirgud::Frame const&)
    {
        throw std::runtime_error{"presentation failed"};
    }, {}, [&]
    {
        std::lock_guard<std::mutex> lock{mutex};
        failed = true;
        wakeup.notify_one();
    }};
    presenter.submit(frame());
    {
        std::unique_lock<std::mutex> lock{mutex};
        wakeup.wait(lock, [&] { return failed; });
    }
    presenter.stop();

    auto const stats = presenter.stats();
    EXPECT_EQ(1u, stats.submitted);
    EXPECT_EQ(0u, stats.presented);
    EXPECT_EQ(1u, stats.submit_failures);
    EXPECT_EQ(1u, stats.submit_us.count);
    EXPECT_TRUE(mirgud::accounting_ok(stats));
    EXPECT_THROW(presenter.rethrow_failure(), std::runtime_error);
}

TEST(MirgudPresenter, separates_first_presented_callback_failure)
{
    mirgud::LatestFramePresenter presenter{[](mirgud::Frame const&) {}, []
    {
        throw std::runtime_error{"lifecycle callback failed"};
    }};
    presenter.submit(frame());
    while (!presenter.stats().lifecycle_callback_failures)
        std::this_thread::yield();
    presenter.stop();

    auto const stats = presenter.stats();
    EXPECT_EQ(1u, stats.submitted);
    EXPECT_EQ(1u, stats.presented);
    EXPECT_EQ(0u, stats.submit_failures);
    EXPECT_EQ(1u, stats.lifecycle_callback_failures);
    EXPECT_EQ(0u, stats.in_flight);
    EXPECT_TRUE(mirgud::accounting_ok(stats));
    EXPECT_THROW(presenter.rethrow_failure(), std::runtime_error);
}

TEST(MirgudPresenter, reports_zero_denominator_percentages_and_final_accounting)
{
    mirgud::Stats stats;
    EXPECT_DOUBLE_EQ(0.0, mirgud::percent(stats.dropped, stats.submitted));
    EXPECT_DOUBLE_EQ(0.0, mirgud::percent(stats.cancelled, stats.submitted));
    auto const report = mirgud::format_accounting_fields(stats, true);
    EXPECT_NE(std::string::npos, report.find("report_kind=final"));
    EXPECT_NE(std::string::npos, report.find("final=true"));
    EXPECT_NE(std::string::npos, report.find("frames_cancelled=0"));
    EXPECT_NE(std::string::npos, report.find("accounting_ok=true"));
}
}
