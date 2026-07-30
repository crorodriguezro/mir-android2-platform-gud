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

#ifndef MIR_GRAPHICS_ANDROID_SERVER_RENDER_WINDOW_H_
#define MIR_GRAPHICS_ANDROID_SERVER_RENDER_WINDOW_H_

#include "android_driver_interpreter.h"
#include "device_quirks.h"
#include "display_name.h"
#include "mir_toolkit/common.h"

#include <memory>
#include <unordered_set>

namespace mir
{
namespace graphics
{
namespace android
{

class FramebufferBundle;
class InterpreterResourceCache;
class ServerRenderWindow : public AndroidDriverInterpreter
{
public:
    ServerRenderWindow(std::shared_ptr<FramebufferBundle> const& fb_bundle,
                       MirPixelFormat format,
                       std::shared_ptr<InterpreterResourceCache> const&,
                       DeviceQuirks& quirks,
                       DisplayName display_name = DisplayName::primary);

    std::shared_ptr<graphics::android::NativeBuffer> driver_requests_buffer(int fence_fd) override;
    void driver_returns_buffer(ANativeWindowBuffer*, int fence_fd) override;
    void driver_cancels_buffer(ANativeWindowBuffer*, int fence_fd) override;
    void lock_buffer(ANativeWindowBuffer*) override;
    void dispatch_driver_request_format(int format) override;
    void dispatch_driver_request_buffer_count(unsigned int count) override;
    void dispatch_driver_request_buffer_size(geometry::Size size) override;
    void dispatch_driver_request_damage(geometry::Rectangles areas) override;
    void dispatch_driver_usage_bits(uint64_t) override;
    int driver_requests_info(int key) const override;
    void sync_to_display(bool sync) override;

private:
    std::shared_ptr<FramebufferBundle> const fb_bundle;
    std::shared_ptr<InterpreterResourceCache> const resource_cache;
    int format;
    bool const clear_fence;
    DisplayName const display_name;
    unsigned dequeued_fences{0};
    unsigned copied_fences{0};
    unsigned returned_fences{0};
    unsigned requests{0};
    std::unordered_set<ANativeWindowBuffer*> returned_buffers;
};

}
}
}

#endif /* MIR_GRAPHICS_ANDROID_SERVER_RENDER_WINDOW_H_ */
