/*
 * Copyright © 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by:
 *   Kevin DuBois <kevin.dubois@canonical.com>
 */

#include "swapping_gl_context.h"
#include "hwc_device.h"
#include "hwc_layerlist.h"
#include "hwc_wrapper.h"
#include "framebuffer_bundle.h"
#include "buffer.h"
#include "hwc_fallback_gl_renderer.h"
#include "gud_output.h"
#include "gud_hwc_boundary.h"
#include "gud_render_only_control.h"
#include "gud_synthetic_output_control.h"
#include "mir/raii.h"
#define MIR_LOG_COMPONENT "android-hwc-device"
#include <mir/log.h>
#include <limits>
#include <algorithm>
#include <chrono>
#include <thread>
#include <fstream>
#include <unistd.h>

namespace mg = mir::graphics;
namespace mga=mir::graphics::android;
namespace geom = mir::geometry;

namespace
{
bool plane_alpha_is_translucent(mg::Renderable const& renderable)
{
    float static const tolerance
    {
        1.0f/(2.0 * static_cast<float>(std::numeric_limits<decltype(hwc_layer_1_t::planeAlpha)>::max()))
    };
    return (renderable.alpha() < 1.0f - tolerance);
}
}

bool mga::HwcDevice20::compatible_renderlist(RenderableList const& list)
{
    if (list.empty())
        return false;

    for (auto const& renderable : list)
    {
        // TODO: enable planeAlpha for (hwc version >= 1.2), 90 deg rotation
        static glm::mat4 const identity(1, 0, 0, 0,  //
                                        0, 1, 0, 0,  //
                                        0, 0, 1, 0,  //
                                        0, 0, 0, 1);
        if (plane_alpha_is_translucent(*renderable) ||
            renderable->transformation() != identity)
        {
            return false;
        }
    }
    return true;
}

bool mga::HwcDevice::compatible_renderlist(RenderableList const& list)
{
    if (list.empty())
        return false;

    for (auto const& renderable : list)
    {
        // TODO: enable planeAlpha for (hwc version >= 1.2), 90 deg rotation
        static glm::mat4 const identity(1, 0, 0, 0,  //
                                        0, 1, 0, 0,  //
                                        0, 0, 1, 0,  //
                                        0, 0, 0, 1);
        if (plane_alpha_is_translucent(*renderable) ||
            renderable->transformation() != identity)
        {
            return false;
        }
    }
    return true;
}

mga::HwcDevice::HwcDevice(
    std::shared_ptr<HwcWrapper> const& hwc_wrapper,
    bool synthetic_gud_external) :
    hwc_wrapper(hwc_wrapper),
    synthetic_gud_external(synthetic_gud_external)
{
}

bool mga::HwcDevice::buffer_is_onscreen(mg::Buffer const& buffer) const
{
    /* check the handles, as the buffer ptrs might change between sets */
    auto const handle = buffer.native_buffer_handle().get();
    auto it = std::find_if(
        onscreen_overlay_buffers.begin(), onscreen_overlay_buffers.end(),
        [&handle](std::shared_ptr<mg::Buffer> const& b)
        {
            return (handle == b->native_buffer_handle().get());
        });
    return it != onscreen_overlay_buffers.end();
}

void mga::HwcDevice::commit(std::list<DisplayContents> const& contents)
{
    std::vector<std::shared_ptr<mg::Buffer>> next_onscreen_overlay_buffers;
    std::list<DisplayContents> hwc_contents;
    bool primary_needs_swap{false};
    bool synthetic_external_present{false};
    bool synthetic_external_needs_swap{false};
    bool purely_overlays_before_synthetic_external{true};

    for (auto const& content : contents)
    {
        /*
         * A GUD output is a Mir-rendered sink, not an Android-HWC display.
         * Passing it to Android HWC can make a device try to present a
         * nonexistent physical external display. Keep that path out of both
         * prepare() and set(); its framebuffer is submitted below instead.
         */
        if (mga::should_submit_to_android_hwc(synthetic_gud_external, content.name))
            hwc_contents.push_back(content);
    }

    hwc_wrapper->prepare(hwc_contents);

    bool purely_overlays = true;

    for (auto& content : contents)
    {
        if (synthetic_gud_external && content.name == mga::DisplayName::external &&
            mga::should_bypass_synthetic_hwc_bookkeeping())
            continue;
        auto const synthetic_external = synthetic_gud_external &&
            content.name == mga::DisplayName::external;
        if (synthetic_external)
        {
            synthetic_external_present = true;
            purely_overlays_before_synthetic_external = purely_overlays;
        }
        auto const needs_swap = content.list.needs_swapbuffers();
        if (content.name == mga::DisplayName::primary)
            primary_needs_swap = needs_swap;
        if (synthetic_external)
            synthetic_external_needs_swap = needs_swap;
        if (needs_swap)
        {
            auto rejected_renderables = content.list.rejected_renderables();
            if (!rejected_renderables.empty())
            {
                auto current_context = mir::raii::paired_calls(
                    [&]{ content.context.make_current(); },
                    [&]{ content.context.release_current(); });
                content.compositor.render(std::move(rejected_renderables), content.list_offset, content.context);
            }
            auto const framebuffer = content.context.last_rendered_buffer();
            if (framebuffer)
                content.list.setup_fb(framebuffer);
            /* The synthetic sink is not submitted to Android HWC to consume this fence. */
            if (mga::should_arm_android_hwc_acquire_fence(synthetic_gud_external, content.name))
                content.list.swap_occurred();
            if (mga::affects_hwc_pacing(synthetic_gud_external, content.name))
                purely_overlays = false;
        }
    
        //setup overlays
        for (auto& layer : content.list)
        {
            auto buffer = layer.layer.buffer();
            if (layer.layer.is_overlay() && buffer)
            {
                if (!buffer_is_onscreen(*buffer))
                    layer.layer.set_acquirefence();
                next_onscreen_overlay_buffers.push_back(buffer);
            }
        }
    }

    if (synthetic_gud_external && !mga::should_bypass_synthetic_hwc_bookkeeping())
        mga::GudOutput::present_external(contents);

    hwc_wrapper->set(hwc_contents);
    onscreen_overlay_buffers = std::move(next_onscreen_overlay_buffers);

    for (auto& content : contents)
    {
        for (auto& it : content.list)
            it.layer.release_buffer();

        mir::Fd retire_fd(content.list.retirement_fence());
    }

    /*
     * Test results (how long can we sleep for without missing a frame?):
     *   arale:   10ms  (TODO: Find out why arale is so slow)
     *   mako:    15ms
     *   krillin: 11ms  (to be fair, the display is 67Hz)
     */
    using namespace std;
    recommend_sleep = purely_overlays ? 10ms : 0ms;

    ++pacing_commit_count;
    if (primary_needs_swap)
        ++pacing_primary_swap_count;
    if (synthetic_external_needs_swap)
        ++pacing_external_swap_count;
    auto const now = std::chrono::steady_clock::now();
    if (pacing_last_report.time_since_epoch().count() == 0 ||
        now - pacing_last_report >= std::chrono::seconds{1})
    {
        auto const commits = pacing_commit_count;
        auto const primary_swaps = pacing_primary_swap_count;
        auto const external_swaps = pacing_external_swap_count;
        mir::log_info(
            "XDISP pacing commits=%llu primary_swap=%llu external_swap=%llu synthetic=%d "
            "purely_before_external=%d purely_overlays=%d sleep_ms=%lld render_only=%d worker=%d",
            static_cast<unsigned long long>(commits),
            static_cast<unsigned long long>(primary_swaps),
            static_cast<unsigned long long>(external_swaps),
            synthetic_external_present, purely_overlays_before_synthetic_external, purely_overlays,
            static_cast<long long>(recommend_sleep.count()),
            !mga::should_start_gud_presentation_worker(),
            mga::should_start_gud_presentation_worker());
        std::ofstream pacing_trace{"/tmp/xdisp-p0.2-pacing.log", std::ios::app};
        pacing_trace << "pid=" << getpid() << " commits=" << commits
                     << " primary_swap=" << primary_swaps
                     << " external_swap=" << external_swaps
                     << " synthetic=" << synthetic_external_present
                     << " purely_before_external=" << purely_overlays_before_synthetic_external
                     << " purely_overlays=" << purely_overlays
                     << " sleep_ms=" << recommend_sleep.count()
                     << " render_only=" << !mga::should_start_gud_presentation_worker()
                     << " worker=" << mga::should_start_gud_presentation_worker() << '\n';
        pacing_commit_count = 0;
        pacing_primary_swap_count = 0;
        pacing_external_swap_count = 0;
        pacing_last_report = now;
    }
}

mga::HwcDevice::~HwcDevice()
{
    GudOutput::shutdown();
}

std::chrono::milliseconds mga::HwcDevice::recommended_sleep() const
{
    return recommend_sleep;
}

void mga::HwcDevice::content_cleared()
{
    onscreen_overlay_buffers.clear();
}

bool mga::HwcDevice::can_swap_buffers() const
{
    return true;
}
