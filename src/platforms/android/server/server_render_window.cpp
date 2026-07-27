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

#include "mir/graphics/buffer.h"
#include "sync_fence.h"
#include "android_format_conversion-inl.h"
#include "server_render_window.h"
#include "framebuffer_bundle.h"
#include "buffer.h"
#include "interpreter_resource_cache.h"

#define MIR_LOG_COMPONENT "android-server-render-window"
#include <mir/log.h>

#include <system/window.h>
#include <boost/throw_exception.hpp>
#include <stdexcept>
#include <sstream>
#include <unistd.h>

namespace mg=mir::graphics;
namespace mga=mir::graphics::android;
namespace geom=mir::geometry;

mga::ServerRenderWindow::ServerRenderWindow(
    std::shared_ptr<mga::FramebufferBundle> const& fb_bundle,
    MirPixelFormat format,
    std::shared_ptr<InterpreterResourceCache> const& cache,
    DeviceQuirks& quirks,
    bool synthetic_gud_external,
    DisplayName display_name)
    : fb_bundle(fb_bundle),
      resource_cache(cache),
      format(mga::to_android_format(format)),
      clear_fence(quirks.clear_fb_context_fence()),
      synthetic_gud_external(synthetic_gud_external),
      display_name(display_name)
{
}

std::shared_ptr<mga::NativeBuffer> mga::ServerRenderWindow::driver_requests_buffer(int fence)
{
    auto buffer = fb_bundle->buffer_for_render();
    auto handle = mga::to_native_buffer_checked(buffer->native_buffer_handle());
    if (fence >= 0)
    {
        handle->reset_fence();
        handle->update_usage(fence, mga::BufferAccess::write);
    }
    resource_cache->store_buffer(buffer, handle);
    ++requests;
    if (display_name == DisplayName::external)
    {
        if (fence >= 0)
            ++dequeued_fences;
        auto const copied_fence = handle->copy_fence();
        if (copied_fence >= 0)
        {
            ++copied_fences;
            close(copied_fence);
        }
    }
    return handle;
}

void mga::ServerRenderWindow::driver_returns_buffer(ANativeWindowBuffer* buffer, int fence_fd)
{
    if (display_name == DisplayName::external)
    {
        returned_buffers.insert(buffer);
        if (fence_fd >= 0)
            ++returned_fences;
        if (requests <= 10 || requests % 120 == 0)
            mir::log_info(
                "GUD POC external render window requests=%u returned_fences=%u unique_buffers=%zu "
                "dequeued_fences=%u copied_fences=%u returned_fence=%d",
                requests, returned_fences, returned_buffers.size(), dequeued_fences, copied_fences, fence_fd);
    }

    //depending on the quirk, some mali drivers won't synchronize the fb context fence before posting.
    //if this bug is present, we synchronize here to avoid tearing or other artifacts.
    if (clear_fence || (synthetic_gud_external && display_name == DisplayName::external))
        mga::SyncFence(std::make_shared<RealSyncFileOps>(), mir::Fd(fence_fd)).wait();
    else
        resource_cache->update_native_fence(buffer, fence_fd);

    resource_cache->retrieve_buffer(buffer);
}

void mga::ServerRenderWindow::driver_cancels_buffer(ANativeWindowBuffer*, int)
{
}

void mga::ServerRenderWindow::lock_buffer(ANativeWindowBuffer*)
{
}

void mga::ServerRenderWindow::dispatch_driver_request_format(int request_format)
{
    format = request_format;
}

void mga::ServerRenderWindow::dispatch_driver_request_buffer_size(geometry::Size)
{
}

void mga::ServerRenderWindow::dispatch_driver_request_damage(geometry::Rectangles)
{
}

void mga::ServerRenderWindow::dispatch_driver_usage_bits(uint64_t usage)
{
}

int mga::ServerRenderWindow::driver_requests_info(int key) const
{
    geom::Size size;
    switch(key)
    {
        case NATIVE_WINDOW_DEFAULT_WIDTH:
        case NATIVE_WINDOW_WIDTH:
            size = fb_bundle->fb_size();
            return size.width.as_uint32_t();
        case NATIVE_WINDOW_DEFAULT_HEIGHT:
        case NATIVE_WINDOW_HEIGHT:
            size = fb_bundle->fb_size();
            return size.height.as_uint32_t();
        case NATIVE_WINDOW_FORMAT:
            return format;
        case NATIVE_WINDOW_TRANSFORM_HINT:
            return 0;
        case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
            return 1;
        case NATIVE_WINDOW_CONCRETE_TYPE:
            return NATIVE_WINDOW_FRAMEBUFFER;
        case NATIVE_WINDOW_CONSUMER_USAGE_BITS:
            return GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_FB;
        case NATIVE_WINDOW_BUFFER_AGE:
            // 0 is a safe fallback since no buffer tracking is in place
            return 0;
        case NATIVE_WINDOW_LAST_QUEUE_DURATION:
            return 20;
        case NATIVE_WINDOW_LAST_DEQUEUE_DURATION:
            return 20;
        case NATIVE_WINDOW_DEFAULT_DATASPACE:
            return HAL_DATASPACE_V0_SRGB_LINEAR;
        case NATIVE_WINDOW_IS_VALID:
            // true
            return 1;
        case NATIVE_WINDOW_MAX_BUFFER_COUNT:
            // The default maximum count of BufferQueue items.
            // See android::BufferQueueDefs::NUM_BUFFER_SLOTS.
            return 64;
        default:
            {
            std::stringstream sstream;
            sstream << "driver requests info we dont provide. key: " << key;
            BOOST_THROW_EXCEPTION(std::runtime_error(sstream.str()));
            }
    }
}

void mga::ServerRenderWindow::sync_to_display(bool should_sync)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglSwapInterval(dpy, should_sync ? 1 : 0);
}

void mga::ServerRenderWindow::dispatch_driver_request_buffer_count(unsigned int)
{
    //note: Haven't seen a good reason to honor this request for a fb context
}
