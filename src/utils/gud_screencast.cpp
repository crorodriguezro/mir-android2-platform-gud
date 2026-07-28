/*
 * XDISP-V0: a deliberately small Mir screencast -> RGB565 -> GUD client.
 *
 * This is not a graphics-platform output.  Mir owns the screencast stream;
 * this process copies each completed buffer before swapping it back to Mir.
 * The only frames that cross the slow GUD/USB boundary are client-owned
 * RGB565 vectors, bounded to one active and one pending frame.
 */
#include "gud_screencast_rgb565.h"

#include "mir_toolkit/mir_client_library.h"
#include "mir_toolkit/mir_buffer_stream.h"
#include "mir_toolkit/mir_screencast.h"
#include "mir/raii.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <boost/program_options.hpp>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace po = boost::program_options;
namespace
{
std::atomic<bool> running;

struct Frame
{
    uint32_t width{};
    uint32_t height{};
    std::vector<uint16_t> pixels;
};

struct Stats
{
    uint64_t received{};
    uint64_t submitted{};
    uint64_t presented{};
    uint64_t dropped{};
    uint64_t conversion_failures{};
    uint64_t submit_failures{};
};

void stop(int)
{
    running = false;
}

std::system_error system_error(char const* action)
{
    return std::system_error{errno, std::system_category(), action};
}

unsigned fd_count(pid_t pid)
{
    auto const path = std::string{"/proc/"} + std::to_string(pid) + "/fd";
    auto* const directory = opendir(path.c_str());
    if (!directory)
        return 0;
    unsigned count{};
    while (auto const* entry = readdir(directory))
        if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, ".."))
            ++count;
    closedir(directory);
    return count;
}

unsigned sync_file_count(pid_t pid)
{
    auto const path = std::string{"/proc/"} + std::to_string(pid) + "/fd";
    auto* const directory = opendir(path.c_str());
    if (!directory)
        return 0;
    unsigned count{};
    while (auto const* entry = readdir(directory))
    {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, ".."))
            continue;
        auto const link = path + "/" + entry->d_name;
        char target[128]{};
        auto const size = readlink(link.c_str(), target, sizeof(target) - 1);
        if (size > 0 && std::strstr(target, "sync_file"))
            ++count;
    }
    closedir(directory);
    return count;
}

class LatestFramePresenter
{
public:
    explicit LatestFramePresenter(std::function<void(Frame const&)> present) :
        present{std::move(present)}, worker{[this] { work(); }}
    {
    }

    ~LatestFramePresenter()
    {
        stop();
    }

    LatestFramePresenter(LatestFramePresenter const&) = delete;
    LatestFramePresenter& operator=(LatestFramePresenter const&) = delete;

    void submit(Frame frame)
    {
        auto incoming = std::unique_ptr<Frame>{new Frame{std::move(frame)}};
        {
            std::lock_guard<std::mutex> lock{mutex};
            if (stopping)
                return;
            ++statistics.submitted;
            if (pending)
                ++statistics.dropped;
            pending = std::move(incoming);
        }
        wakeup.notify_one();
    }

    void conversion_failed()
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++statistics.conversion_failures;
    }

    void received()
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++statistics.received;
    }

    Stats stats() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        return statistics;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock{mutex};
            if (stopping)
                return;
            stopping = true;
            pending.reset();
        }
        wakeup.notify_one();
        if (worker.joinable())
            worker.join();
    }

private:
    void work()
    {
        while (true)
        {
            std::unique_ptr<Frame> frame;
            {
                std::unique_lock<std::mutex> lock{mutex};
                wakeup.wait(lock, [this] { return stopping || pending; });
                if (stopping)
                    return;
                frame = std::move(pending);
            }
            try
            {
                present(*frame);
                std::lock_guard<std::mutex> lock{mutex};
                ++statistics.presented;
            }
            catch (std::exception const& error)
            {
                std::cerr << "mirgud: GUD submission failed: " << error.what() << std::endl;
                std::lock_guard<std::mutex> lock{mutex};
                ++statistics.submit_failures;
            }
        }
    }

    std::function<void(Frame const&)> present;
    mutable std::mutex mutex;
    std::condition_variable wakeup;
    std::unique_ptr<Frame> pending;
    Stats statistics;
    bool stopping{};
    std::thread worker;
};

int open_gud_card()
{
    for (unsigned card = 0; card != 16; ++card)
    {
        auto const node = std::string{"/dev/dri/card"} + std::to_string(card);
        int const fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        auto* const version = drmGetVersion(fd);
        bool const is_gud = version && version->name && !std::strcmp(version->name, "gud");
        if (version)
            drmFreeVersion(version);
        if (is_gud)
            return fd;
        close(fd);
    }
    return -1;
}

uint32_t property(int fd, uint32_t object, uint32_t type, char const* name)
{
    auto* const properties = drmModeObjectGetProperties(fd, object, type);
    if (!properties)
        return 0;
    uint32_t result{};
    for (uint32_t i = 0; i != properties->count_props; ++i)
    {
        auto* const candidate = drmModeGetProperty(fd, properties->props[i]);
        if (candidate)
        {
            if (!std::strcmp(candidate->name, name))
                result = candidate->prop_id;
            drmModeFreeProperty(candidate);
        }
        if (result)
            break;
    }
    drmModeFreeObjectProperties(properties);
    return result;
}

bool primary_plane(int fd, uint32_t id)
{
    auto const type_property = property(fd, id, DRM_MODE_OBJECT_PLANE, "type");
    auto* const properties = type_property ? drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE) : nullptr;
    bool primary{};
    if (properties)
    {
        for (uint32_t i = 0; i != properties->count_props; ++i)
            if (properties->props[i] == type_property && properties->prop_values[i] == DRM_PLANE_TYPE_PRIMARY)
                primary = true;
        drmModeFreeObjectProperties(properties);
    }
    return primary;
}

struct Properties
{
    uint32_t connector_crtc{}, crtc_mode{}, crtc_active{}, plane_fb{}, plane_crtc{};
    uint32_t src_x{}, src_y{}, src_w{}, src_h{}, crtc_x{}, crtc_y{}, crtc_w{}, crtc_h{};
};

class GudKms
{
public:
    GudKms(uint32_t required_width, uint32_t required_height) :
        required_width{required_width}, required_height{required_height}
    {
        fd = open_gud_card();
        if (fd < 0)
            throw std::runtime_error{"no accessible GUD DRM card"};
        try
        {
            if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
                drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
                throw system_error("cannot enable GUD atomic KMS capabilities");
        }
        catch (...)
        {
            teardown();
            throw;
        }
    }

    ~GudKms()
    {
        teardown();
    }

    void setup()
    {
        if (setup_complete)
            throw std::logic_error{"GUD KMS setup was requested twice"};
        discover_kms();
        allocate(frames[0]);
        allocate(frames[1]);
        setup_complete = true;
    }

    void initial_modeset()
    {
        if (!setup_complete)
            throw std::logic_error{"GUD initial modeset requires completed setup"};
        if (initial_modeset_complete)
            throw std::logic_error{"GUD initial modeset was requested twice"};

        std::cerr << "mirgud: V0.1 initial modeset begin" << std::endl;
        std::memset(frames[0].map, 0, frames[0].dumb.size);
        try
        {
            commit(frames[0], true);
            initial_modeset_complete = true;
            std::cerr << "mirgud: V0.1 initial modeset complete" << std::endl;
        }
        catch (std::system_error const& error)
        {
            std::cerr << "mirgud: V0.1 initial modeset failed errno=" << error.code().value() <<
                " (" << error.code().message() << ")" << std::endl;
            throw;
        }
    }

    void present(Frame const& source)
    {
        if (!initial_modeset_complete)
            throw std::logic_error{"GUD update attempted before the initial modeset"};
        if (source.width != width || source.height != height ||
            source.pixels.size() != static_cast<std::size_t>(width) * height)
            throw std::runtime_error{"screencast frame does not match the selected GUD mode"};
        auto const update = ++update_sequence;
        std::cerr << "mirgud: V0.1 update " << update << " begin" << std::endl;
        auto& frame = frames[next];
        for (uint32_t y = 0; y != height; ++y)
        {
            auto const* const source_row = source.pixels.data() + static_cast<std::size_t>(y) * width;
            auto* const destination_row = static_cast<uint16_t*>(frame.map) +
                static_cast<std::size_t>(y) * frame.dumb.pitch / sizeof(uint16_t);
            std::memcpy(destination_row, source_row, static_cast<std::size_t>(width) * sizeof(uint16_t));
        }
        try
        {
            commit(frame, false);
            std::cerr << "mirgud: V0.1 update " << update << " complete" << std::endl;
        }
        catch (std::system_error const& error)
        {
            std::cerr << "mirgud: V0.1 update " << update << " failed errno=" << error.code().value() <<
                " (" << error.code().message() << ")" << std::endl;
            throw;
        }
        next ^= 1;
    }

private:
    struct KmsFrame
    {
        drm_mode_create_dumb dumb{};
        uint32_t framebuffer{};
        void* map{MAP_FAILED};
    };

    void discover_kms()
    {
        auto resources = std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)>{
            drmModeGetResources(fd), drmModeFreeResources};
        if (!resources)
            throw system_error("cannot query GUD KMS resources");

        uint32_t encoder_id{};
        for (int i = 0; i != resources->count_connectors && !connector; ++i)
        {
            auto candidate = std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>{
                drmModeGetConnector(fd, resources->connectors[i]), drmModeFreeConnector};
            if (!candidate || candidate->connection != DRM_MODE_CONNECTED)
                continue;
            for (int m = 0; m != candidate->count_modes; ++m)
                if (candidate->modes[m].hdisplay == required_width && candidate->modes[m].vdisplay == required_height)
                {
                    connector = candidate->connector_id;
                    mode = candidate->modes[m];
                    encoder_id = candidate->encoder_id ? candidate->encoder_id :
                        (candidate->count_encoders ? candidate->encoders[0] : 0);
                    break;
                }
        }
        if (!connector || !encoder_id)
            throw std::runtime_error{"GUD has no connected mode matching the size requested by mirgud"};

        auto encoder = std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)>{
            drmModeGetEncoder(fd, encoder_id), drmModeFreeEncoder};
        if (!encoder)
            throw system_error("GUD encoder unavailable");
        crtc = encoder->crtc_id;
        uint32_t crtc_index{};
        for (int i = 0; i != resources->count_crtcs; ++i)
        {
            if (!crtc && (encoder->possible_crtcs & (1U << i)))
                crtc = resources->crtcs[i];
            if (resources->crtcs[i] == crtc)
                crtc_index = static_cast<uint32_t>(i);
        }
        auto planes = std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)>{
            drmModeGetPlaneResources(fd), drmModeFreePlaneResources};
        if (planes)
            for (uint32_t i = 0; i != planes->count_planes && !plane; ++i)
            {
                auto candidate = std::unique_ptr<drmModePlane, decltype(&drmModeFreePlane)>{
                    drmModeGetPlane(fd, planes->planes[i]), drmModeFreePlane};
                if (candidate && (candidate->possible_crtcs & (1U << crtc_index)) && primary_plane(fd, candidate->plane_id))
                    plane = candidate->plane_id;
            }
        properties = {property(fd, connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"),
            property(fd, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), property(fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"),
            property(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID"), property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID"),
            property(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X"), property(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_Y"),
            property(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W"), property(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H"),
            property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X"), property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y"),
            property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W"), property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H")};
        if (!crtc || !plane || !properties.connector_crtc || !properties.crtc_mode || !properties.crtc_active ||
            !properties.plane_fb || !properties.plane_crtc || !properties.src_x || !properties.src_y ||
            !properties.src_w || !properties.src_h || !properties.crtc_x || !properties.crtc_y ||
            !properties.crtc_w || !properties.crtc_h)
            throw std::runtime_error{"GUD atomic KMS setup is incomplete"};
        if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &mode_blob))
            throw system_error("cannot create GUD mode blob");
        width = required_width;
        height = required_height;
        std::cerr << "mirgud: GUD enabled at " << width << "x" << height << " RGB565" << std::endl;
    }

    void allocate(KmsFrame& frame)
    {
        frame.dumb.width = width;
        frame.dumb.height = height;
        frame.dumb.bpp = 16;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &frame.dumb))
            throw system_error("GUD dumb allocation failed");
        drm_mode_map_dumb mapping{};
        mapping.handle = frame.dumb.handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mapping))
            throw system_error("GUD dumb mapping failed");
        frame.map = mmap(nullptr, frame.dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mapping.offset);
        uint32_t handles[] = {frame.dumb.handle, 0, 0, 0};
        uint32_t pitches[] = {frame.dumb.pitch, 0, 0, 0};
        uint32_t offsets[] = {0, 0, 0, 0};
        if (frame.map == MAP_FAILED || drmModeAddFB2(fd, width, height, DRM_FORMAT_RGB565,
            handles, pitches, offsets, &frame.framebuffer, 0))
            throw system_error("GUD RGB565 framebuffer allocation failed");
    }

    void commit(KmsFrame const& frame, bool modeset)
    {
        auto* const request = drmModeAtomicAlloc();
        if (!request)
            throw std::runtime_error{"cannot allocate GUD atomic request"};
        auto const add = [request](uint32_t object, uint32_t id, uint64_t value)
        {
            return drmModeAtomicAddProperty(request, object, id, value) >= 0;
        };
        bool ok = add(plane, properties.plane_fb, frame.framebuffer);
        if (modeset)
            ok = ok && add(connector, properties.connector_crtc, crtc) && add(crtc, properties.crtc_mode, mode_blob) &&
                add(crtc, properties.crtc_active, 1) && add(plane, properties.plane_crtc, crtc) &&
                add(plane, properties.src_x, 0) && add(plane, properties.src_y, 0) &&
                add(plane, properties.src_w, width << 16) && add(plane, properties.src_h, height << 16) &&
                add(plane, properties.crtc_x, 0) && add(plane, properties.crtc_y, 0) &&
                add(plane, properties.crtc_w, width) && add(plane, properties.crtc_h, height);
        int const result = ok ? drmModeAtomicCommit(fd, request, modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0, nullptr) : -1;
        drmModeAtomicFree(request);
        if (result)
        {
            auto const saved_errno = errno;
            throw std::system_error{saved_errno, std::system_category(), "GUD atomic commit failed"};
        }
    }

    void teardown()
    {
        for (auto& frame : frames)
        {
            if (frame.map != MAP_FAILED)
                munmap(frame.map, frame.dumb.size);
            if (fd >= 0 && frame.framebuffer)
                drmModeRmFB(fd, frame.framebuffer);
            if (fd >= 0 && frame.dumb.handle)
            {
                drm_mode_destroy_dumb destroy{};
                destroy.handle = frame.dumb.handle;
                drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
            }
            frame = {};
            frame.map = MAP_FAILED;
        }
        if (fd >= 0 && mode_blob)
            drmModeDestroyPropertyBlob(fd, mode_blob);
        mode_blob = 0;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }

    uint32_t required_width;
    uint32_t required_height;
    int fd{-1};
    uint32_t connector{}, crtc{}, plane{}, mode_blob{}, width{}, height{}, next{};
    uint64_t update_sequence{};
    bool setup_complete{};
    bool initial_modeset_complete{};
    drmModeModeInfo mode{};
    Properties properties{};
    KmsFrame frames[2];
};

MirRectangle first_enabled_output(MirDisplayConfig const& config)
{
    auto const count = mir_display_config_get_num_outputs(&config);
    for (int i = 0; i != count; ++i)
    {
        auto const* output = mir_display_config_get_output(&config, i);
        if (mir_output_get_connection_state(output) != mir_output_connection_state_connected || !mir_output_is_enabled(output))
            continue;
        auto const modes = mir_output_get_num_modes(output);
        if (mir_output_get_current_mode_index(output) >= static_cast<size_t>(modes))
            continue;
        auto const* mode = mir_output_get_current_mode(output);
        return {mir_output_get_position_x(output), mir_output_get_position_y(output),
            static_cast<unsigned int>(mir_output_mode_get_width(mode)),
            static_cast<unsigned int>(mir_output_mode_get_height(mode))};
    }
    throw std::runtime_error{"no enabled physical output is available for the screencast source"};
}

MirGraphicsRegion graphics_region(MirBufferStream* stream)
{
    MirGraphicsRegion region{0, 0, 0, mir_pixel_format_invalid, nullptr};
    mir_buffer_stream_get_graphics_region(stream, &region);
    if (!region.vaddr)
        throw std::runtime_error{"screencast buffer is not CPU mapped"};
    return region;
}

class DirectCapture
{
public:
    explicit DirectCapture(MirBufferStream* stream) : stream{stream}, region{graphics_region(stream)}
    {
        if (region.width <= 0 || region.height <= 0 || region.stride <= 0)
            throw std::runtime_error{"invalid CPU screencast graphics region"};
        std::cerr << "mirgud: virtual frame source is CPU mapped " << region.width << "x" << region.height <<
            " format=" << static_cast<int>(region.pixel_format) << " stride=" << region.stride << std::endl;
    }

    Frame next()
    {
        Frame frame{static_cast<uint32_t>(region.width), static_cast<uint32_t>(region.height),
            std::vector<uint16_t>(static_cast<std::size_t>(region.width) * region.height)};
        auto const* row = reinterpret_cast<uint8_t const*>(region.vaddr) +
            static_cast<std::size_t>(region.height - 1) * region.stride;
        try
        {
            for (int y = 0; y != region.height; ++y)
            {
                mirgud::convert_row_to_rgb565(region.pixel_format, row,
                    frame.pixels.data() + static_cast<std::size_t>(y) * region.width, region.width);
                row -= region.stride; // Mir's screencast region is bottom-up.
            }
            /* The vector is now independent; this immediately releases the Mir buffer. */
            mir_buffer_stream_swap_buffers_sync(stream);
            return frame;
        }
        catch (...)
        {
            /* Do not retain a failed conversion's borrowed Mir buffer either. */
            mir_buffer_stream_swap_buffers_sync(stream);
            throw;
        }
    }

private:
    MirBufferStream* stream;
    MirGraphicsRegion region;
};

class EglCapture
{
public:
    EglCapture(MirConnection* connection, MirBufferStream* stream, uint32_t width, uint32_t height) : width{width}, height{height}
    {
        static EGLint const attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
        static EGLint const context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        auto const native_display = reinterpret_cast<EGLNativeDisplayType>(mir_connection_get_egl_native_display(connection));
        auto const native_window = reinterpret_cast<EGLNativeWindowType>(mir_buffer_stream_get_egl_native_window(stream));
#pragma GCC diagnostic pop
        display = eglGetDisplay(native_display);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
            throw std::runtime_error{"cannot initialize EGL screencast fallback"};
        EGLint count{};
        if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1)
            throw std::runtime_error{"cannot choose EGL screencast fallback configuration"};
        surface = eglCreateWindowSurface(display, config, native_window, nullptr);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
            throw std::runtime_error{"cannot create EGL screencast fallback context"};
        uint32_t test_pixel{};
        glReadPixels(0, 0, 1, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &test_pixel);
        read_format = glGetError() == GL_NO_ERROR ? GL_BGRA_EXT : GL_RGBA;
        bytes.resize(static_cast<std::size_t>(width) * height * 4);
        std::cerr << "mirgud: virtual frame source uses EGL readback fallback " << width << "x" << height << std::endl;
    }

    ~EglCapture()
    {
        if (display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE)
                eglDestroySurface(display, surface);
            if (context != EGL_NO_CONTEXT)
                eglDestroyContext(display, context);
            eglTerminate(display);
        }
    }

    Frame next()
    {
        try
        {
            glReadPixels(0, 0, width, height, read_format, GL_UNSIGNED_BYTE, bytes.data());
            if (glGetError() != GL_NO_ERROR)
                throw std::runtime_error{"EGL screencast readback failed"};
            Frame frame{width, height, std::vector<uint16_t>(static_cast<std::size_t>(width) * height)};
            auto const format = read_format == GL_BGRA_EXT ? mir_pixel_format_argb_8888 : mir_pixel_format_abgr_8888;
            for (uint32_t y = 0; y != height; ++y)
            {
                auto const* const row = bytes.data() + static_cast<std::size_t>(height - 1 - y) * width * 4;
                mirgud::convert_row_to_rgb565(format, row, frame.pixels.data() + static_cast<std::size_t>(y) * width, width);
            }
            if (!eglSwapBuffers(display, surface))
                throw std::runtime_error{"EGL screencast buffer release failed"};
            return frame;
        }
        catch (...)
        {
            eglSwapBuffers(display, surface);
            throw;
        }
    }

private:
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> bytes;
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLContext context{EGL_NO_CONTEXT};
    EGLSurface surface{EGL_NO_SURFACE};
    EGLConfig config{};
    GLenum read_format{};
};

void report(Stats const& stats, pid_t monitor_pid)
{
    std::cerr << "mirgud: frames_received=" << stats.received << " frames_presented=" << stats.presented <<
        " frames_dropped=" << stats.dropped << " conversion_failures=" << stats.conversion_failures <<
        " gud_submit_failures=" << stats.submit_failures << " self_fds=" << fd_count(getpid());
    if (monitor_pid > 0)
        std::cerr << " monitor_pid=" << monitor_pid << " monitor_fds=" << fd_count(monitor_pid) <<
            " monitor_sync_files=" << sync_file_count(monitor_pid);
    std::cerr << std::endl;
}
}

int main(int argc, char* argv[])
try
{
    uint32_t width{1280};
    uint32_t height{720};
    uint32_t capture_interval{1};
    pid_t monitor_pid{};
    std::string socket;
    bool pattern{};
    bool no_gud{};
    po::options_description options{"Usage"};
    options.add_options()
        ("help,h", "show this help")
        ("mir-socket-file,m", po::value<std::string>(&socket), "Mir server socket")
        ("size,s", po::value<std::vector<uint32_t>>()->multitoken(), "GUD/screencast size (default 1280 720)")
        ("cap-interval", po::value<uint32_t>(&capture_interval), "capture every N display intervals")
        ("monitor-pid", po::value<pid_t>(&monitor_pid), "sample this compositor PID's fd/sync_file counts")
        ("pattern", po::bool_switch(&pattern), "Stage A: send the static RGB565 checkerboard, without Mir")
        ("no-gud", po::bool_switch(&no_gud), "source-only stop-condition probe: copy/release frames without opening GUD");
    po::variables_map variables;
    po::store(po::parse_command_line(argc, argv, options), variables);
    po::notify(variables);
    if (variables.count("help"))
    {
        std::cout << options << std::endl;
        return EXIT_SUCCESS;
    }
    if (capture_interval == 0)
        throw std::runtime_error{"cap-interval must be positive"};
    if (variables.count("size"))
    {
        auto const size = variables["size"].as<std::vector<uint32_t>>();
        if (size.size() != 2 || !size[0] || !size[1])
            throw std::runtime_error{"size requires positive width and height"};
        width = size[0];
        height = size[1];
    }
    running = true;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    std::unique_ptr<GudKms> kms;
    if (no_gud)
        std::cerr << "mirgud: GUD disabled for source-only stability probe" << std::endl;
    else
    {
        kms = std::make_unique<GudKms>(width, height);
        kms->setup();
        kms->initial_modeset();
    }
    LatestFramePresenter presenter{[&kms](Frame const& frame)
    {
        if (kms)
            kms->present(frame);
    }};

    if (pattern)
    {
        std::cerr << "mirgud: Stage A checkerboard started; verify it on HDMI before Stage B" << std::endl;
        auto const frame = Frame{width, height, mirgud::checkerboard_rgb565(width, height)};
        auto next_report = std::chrono::steady_clock::now();
        while (running)
        {
            presenter.submit(frame);
            std::this_thread::sleep_for(std::chrono::seconds{1});
            if (std::chrono::steady_clock::now() >= next_report)
            {
                report(presenter.stats(), monitor_pid);
                next_report = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            }
        }
        return EXIT_SUCCESS;
    }

    auto const connection = mir::raii::deleter_for(mir_connect_sync(socket.empty() ? nullptr : socket.c_str(), "mirgud"),
        [](MirConnection* value) { if (value) mir_connection_release(value); });
    if (!connection || !mir_connection_is_valid(connection.get()))
        throw std::runtime_error{"cannot connect to Mir for virtual/screencast capture"};
    auto const display_configuration = mir::raii::deleter_for(
        mir_connection_create_display_configuration(connection.get()), &mir_display_config_release);
    if (!display_configuration)
        throw std::runtime_error{"cannot obtain Mir display configuration"};
    auto const region = first_enabled_output(*display_configuration);
    MirPixelFormat format{};
    unsigned int formats{};
    mir_connection_get_available_surface_formats(connection.get(), &format, 1, &formats);
    if (!formats)
        throw std::runtime_error{"Mir supplied no screencast pixel format"};
    auto* const spec = mir_create_screencast_spec(connection.get());
    mir_screencast_spec_set_width(spec, width);
    mir_screencast_spec_set_height(spec, height);
    mir_screencast_spec_set_pixel_format(spec, format);
    mir_screencast_spec_set_capture_region(spec, &region);
    auto const screencast = mir::raii::deleter_for(mir_screencast_create_sync(spec),
        [](MirScreencast* value) { if (value) mir_screencast_release_sync(value); });
    mir_screencast_spec_release(spec);
    if (!screencast)
        throw std::runtime_error{"cannot create the Mir virtual/screencast stream"};
    auto* const stream = mir_screencast_get_buffer_stream(screencast.get());
    if (!stream)
        throw std::runtime_error{"Mir screencast has no buffer stream"};
    std::cerr << "mirgud: Stage B virtual/screencast source enabled " << width << "x" << height <<
        " requested_format=" << static_cast<int>(format) << std::endl;

    auto next_report = std::chrono::steady_clock::now();
    std::unique_ptr<DirectCapture> direct;
    try
    {
        direct = std::make_unique<DirectCapture>(stream);
    }
    catch (std::exception const& direct_error)
    {
        std::cerr << "mirgud: CPU mapping unavailable (" << direct_error.what() << "); using EGL readback" << std::endl;
    }
    if (direct)
    {
        bool first{};
        while (running)
        {
            auto const start = std::chrono::steady_clock::now();
            try
            {
                auto frame = direct->next();
                presenter.received();
                if (!first)
                {
                    std::cerr << "mirgud: first CPU-complete virtual frame copied and converted" << std::endl;
                    first = true;
                }
                presenter.submit(std::move(frame));
            }
            catch (...)
            {
                presenter.conversion_failed();
                throw;
            }
            if (std::chrono::steady_clock::now() >= next_report)
            {
                report(presenter.stats(), monitor_pid);
                next_report += std::chrono::seconds{1};
            }
            std::this_thread::sleep_until(start + std::chrono::milliseconds{16 * capture_interval});
        }
    }
    else
    {
        EglCapture capture{connection.get(), stream, width, height};
        bool first{};
        while (running)
        {
            auto const start = std::chrono::steady_clock::now();
            auto frame = capture.next();
            presenter.received();
            if (!first)
            {
                std::cerr << "mirgud: first GPU-complete virtual frame read back and converted" << std::endl;
                first = true;
            }
            presenter.submit(std::move(frame));
            if (std::chrono::steady_clock::now() >= next_report)
            {
                report(presenter.stats(), monitor_pid);
                next_report += std::chrono::seconds{1};
            }
            std::this_thread::sleep_until(start + std::chrono::milliseconds{16 * capture_interval});
        }
    }
    return EXIT_SUCCESS;
}
catch (std::exception const& error)
{
    std::cerr << "mirgud: " << error.what() << std::endl;
    return EXIT_FAILURE;
}
