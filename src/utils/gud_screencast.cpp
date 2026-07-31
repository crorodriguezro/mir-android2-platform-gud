/*
 * XDISP-V0: a deliberately small Mir screencast -> pixel format -> GUD client.
 *
 * This is not a graphics-platform output.  Mir owns the screencast stream;
 * this process copies each completed buffer before swapping it back to Mir.
 * The only frames that cross the slow GUD/USB boundary are client-owned
 * pixel vectors, bounded to one active and one pending frame.
 *
 * Transport format is selectable via --pixel-format (rgb565 or xrgb8888).
 */
#include "gud_screencast_format.h"

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

#include <algorithm>
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
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

namespace po = boost::program_options;
namespace
{
std::atomic<bool> running;
volatile sig_atomic_t stop_signal_received{};
bool managed_mode{};
int managed_status_fd{-1};

enum ManagedExit
{
    managed_unavailable = 20,
    managed_recoverable = 21,
    managed_poisoned = 22
};

void managed_status(char const* value)
{
    if (!managed_mode || managed_status_fd < 0)
        return;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    auto const milliseconds = static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    auto const message = std::string{"XDISP1 "} + std::to_string(milliseconds) + " " + value + "\n";
    auto const ignored = write(managed_status_fd, message.data(), message.size());
    (void)ignored;
}

struct Stats
{
    uint64_t received{};
    uint64_t submitted{};
    uint64_t presented{};
    uint64_t dropped{};
    uint64_t conversion_failures{};
    uint64_t submit_failures{};
    mirgud::TimingSummary capture_us{};
    mirgud::TimingSummary conversion_us{};
    mirgud::TimingSummary submit_us{};
    uint64_t conversion_path_counts[5]{}; /* indexed by ConversionPath */
};

void stop(int)
{
    stop_signal_received = 1;
}

bool keep_running()
{
    return running && !stop_signal_received;
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
    explicit LatestFramePresenter(
        std::function<void(mirgud::Frame const&)> present,
        std::function<void()> first_presented = {}) :
        present{std::move(present)}, first_presented{std::move(first_presented)}, worker{[this] { work(); }}
    {
    }

    ~LatestFramePresenter()
    {
        stop();
    }

    LatestFramePresenter(LatestFramePresenter const&) = delete;
    LatestFramePresenter& operator=(LatestFramePresenter const&) = delete;

    void submit(mirgud::Frame frame)
    {
        auto incoming = std::unique_ptr<mirgud::Frame>{new mirgud::Frame{std::move(frame)}};
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

    void received(uint64_t capture_us, uint64_t conversion_us)
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++statistics.received;
        statistics.capture_us.add(capture_us);
        statistics.conversion_us.add(conversion_us);
        ++statistics.conversion_path_counts[static_cast<unsigned>(conversion_path)];
    }

    void presented(uint64_t us)
    {
        std::lock_guard<std::mutex> lock{mutex};
        statistics.submit_us.add(us);
        ++statistics.presented;
        if (statistics.presented == 1 && first_presented)
            first_presented();
    }

    Stats stats() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        return statistics;
    }

    void set_conversion_path(mirgud::ConversionPath path)
    {
        std::lock_guard<std::mutex> lock{mutex};
        conversion_path = path;
    }

    void rethrow_failure()
    {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> lock{mutex};
            failure = presentation_failure;
        }
        if (failure)
            std::rethrow_exception(failure);
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
            std::unique_ptr<mirgud::Frame> frame;
            {
                std::unique_lock<std::mutex> lock{mutex};
                wakeup.wait(lock, [this] { return stopping || pending; });
                if (stopping)
                    return;
                frame = std::move(pending);
            }
            try
            {
                auto const submit_start = std::chrono::steady_clock::now();
                present(*frame);
                auto const submit_end = std::chrono::steady_clock::now();
                presented(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start).count()));
            }
            catch (std::exception const& error)
            {
                std::cerr << "mirgud: GUD submission failed: " << error.what() << std::endl;
                {
                    std::lock_guard<std::mutex> lock{mutex};
                    ++statistics.submit_failures;
                    presentation_failure = std::current_exception();
                    pending.reset();
                }
                running = false;
                return;
            }
        }
    }

    std::function<void(mirgud::Frame const&)> present;
    std::function<void()> first_presented;
    mutable std::mutex mutex;
    std::condition_variable wakeup;
    std::unique_ptr<mirgud::Frame> pending;
    Stats statistics;
    mirgud::ConversionPath conversion_path{mirgud::ConversionPath::channel_reorder};
    std::exception_ptr presentation_failure;
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
    GudKms(uint32_t required_width, uint32_t required_height, mirgud::PixelFormat pixel_format,
           int inherited_fd = -1, uint32_t required_connector = 0) :
        required_width{required_width}, required_height{required_height},
        pixel_format{pixel_format}, required_connector{required_connector}
    {
        fd = inherited_fd >= 0 ? inherited_fd : open_gud_card();
        if (fd < 0)
            throw std::runtime_error{"no accessible GUD DRM card"};
        try
        {
            auto* const version = drmGetVersion(fd);
            bool const is_gud = version && version->name && !std::strcmp(version->name, "gud");
            if (version)
                drmFreeVersion(version);
            if (!is_gud)
                throw std::runtime_error{"managed DRM fd is not a GUD card"};
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
        managed_status("KMS_TEARDOWN_BEGIN");
        teardown();
        managed_status("KMS_TEARDOWN_COMPLETE");
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

    void present(mirgud::Frame const& source)
    {
        if (!initial_modeset_complete)
            throw std::logic_error{"GUD update attempted before the initial modeset"};
        if (source.width != width || source.height != height ||
            source.pixels.size() != static_cast<std::size_t>(width) * height * mirgud::bytes_per_pixel(pixel_format))
            throw std::runtime_error{"screencast frame does not match the selected GUD mode"};
        auto const update = ++update_sequence;
        if (!managed_mode)
            std::cerr << "mirgud: V0.1 update " << update << " begin" << std::endl;
        auto& frame = frames[next];
        auto const bpp = mirgud::bytes_per_pixel(pixel_format);
        for (uint32_t y = 0; y != height; ++y)
        {
            auto const* const source_row = source.pixels.data() + static_cast<std::size_t>(y) * width * bpp;
            auto* const destination_row = static_cast<uint8_t*>(frame.map) +
                static_cast<std::size_t>(y) * frame.dumb.pitch;
            std::memcpy(destination_row, source_row, static_cast<std::size_t>(width) * bpp);
        }
        try
        {
            commit(frame, false);
            if (!managed_mode)
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
            if (!candidate || candidate->connection != DRM_MODE_CONNECTED ||
                (required_connector && candidate->connector_id != required_connector))
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
        std::cerr << "mirgud: GUD enabled at " << width << "x" << height << " " <<
            mirgud::format_name(pixel_format) << std::endl;
    }

    void allocate(KmsFrame& frame)
    {
        frame.dumb.width = width;
        frame.dumb.height = height;
        frame.dumb.bpp = mirgud::bytes_per_pixel(pixel_format) * 8;
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
        if (frame.map == MAP_FAILED || drmModeAddFB2(fd, width, height, mirgud::drm_format(pixel_format),
            handles, pitches, offsets, &frame.framebuffer, 0))
            throw system_error("GUD framebuffer allocation failed");
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
    mirgud::PixelFormat pixel_format;
    uint32_t required_connector;
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

MirRectangle aethercast_extend_region(MirDisplayConfig const& config)
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
        return {mir_output_mode_get_width(mode), 0, 0, 0};
    }
    throw std::runtime_error{"Aethercast extend mode requires an enabled physical output"};
}

void log_topology(char const* phase, MirDisplayConfig const& config)
{
    auto const count = mir_display_config_get_num_outputs(&config);
    for (int i = 0; i != count; ++i)
    {
        auto const* output = mir_display_config_get_output(&config, i);
        auto const modes = mir_output_get_num_modes(output);
        auto const current_mode_index = mir_output_get_current_mode_index(output);
        auto const* mode = current_mode_index < static_cast<size_t>(modes) ?
            mir_output_get_current_mode(output) : nullptr;
        std::cerr << "mirgud: xdisp topology phase=" << phase <<
            " id=" << mir_output_get_id(output) <<
            " connected=" << (mir_output_get_connection_state(output) == mir_output_connection_state_connected) <<
            " used=" << mir_output_is_enabled(output) <<
            " mode=" << (mode ? std::to_string(mir_output_mode_get_width(mode)) + "x" +
                std::to_string(mir_output_mode_get_height(mode)) : "none") <<
            " top_left=(" << mir_output_get_position_x(output) << "," << mir_output_get_position_y(output) << ")"
            << std::endl;
    }
}

uint64_t sampled_fingerprint(mirgud::Frame const& frame)
{
    uint64_t hash{1469598103934665603ULL};
    auto const samples = std::min<std::size_t>(256, frame.pixels.size());
    if (!samples)
        return hash;
    auto const step = std::max<std::size_t>(1, frame.pixels.size() / samples);
    for (std::size_t i = 0; i < frame.pixels.size(); i += step)
    {
        hash ^= frame.pixels[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

void write_ppm(mirgud::Frame const& frame, std::string const& path)
{
    if (frame.width == 0 || frame.height == 0)
        throw std::runtime_error{"cannot dump an invalid frame"};

    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        throw std::runtime_error{"cannot open frame dump " + path};
    output << "P6\n" << frame.width << " " << frame.height << "\n255\n";
    auto const bpp = mirgud::bytes_per_pixel(frame.format);
    for (uint32_t y = 0; y != frame.height; ++y)
        for (uint32_t x = 0; x != frame.width; ++x)
        {
            auto const offset = (static_cast<std::size_t>(y) * frame.width + x) * bpp;
            uint8_t r, g, b;
            if (frame.format == mirgud::PixelFormat::rgb565)
            {
                auto const pixel = *reinterpret_cast<uint16_t const*>(&frame.pixels[offset]);
                r = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
                g = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
                b = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
            }
            else
            {
                auto const pixel = *reinterpret_cast<uint32_t const*>(&frame.pixels[offset]);
                r = static_cast<uint8_t>((pixel >> 16) & 0xff);
                g = static_cast<uint8_t>((pixel >> 8) & 0xff);
                b = static_cast<uint8_t>(pixel & 0xff);
            }
            output.write(reinterpret_cast<char const*>(&r), 1);
            output.write(reinterpret_cast<char const*>(&g), 1);
            output.write(reinterpret_cast<char const*>(&b), 1);
        }
    if (!output)
        throw std::runtime_error{"cannot write frame dump " + path};
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
    DirectCapture(MirBufferStream* stream, mirgud::PixelFormat pixel_format, mirgud::RowOrder row_order) :
        stream{stream}, pixel_format{pixel_format}, row_order{row_order}
    {
        auto const region = graphics_region(stream);
        if (region.width <= 0 || region.height <= 0 || region.stride <= 0)
            throw std::runtime_error{"invalid CPU screencast graphics region"};
        source_format = mirgud::make_source_format(
            region.pixel_format,
            static_cast<uint32_t>(region.width),
            static_cast<uint32_t>(region.height),
            region.stride, row_order, pixel_format);
        std::cerr << "mirgud: virtual frame source is CPU mapped " <<
            source_format.width << "x" << source_format.height <<
            " mir_format=" << static_cast<int>(source_format.pixel_format) <<
            " stride=" << source_format.stride <<
            " row_order=" << (row_order == mirgud::RowOrder::top_down ? "top-down" : "bottom-up") <<
            " transport=" << mirgud::format_name(pixel_format) <<
            " conversion_path=" << source_format.conversion_path_name() << std::endl;
    }

    mirgud::SourceFormat const& source_format_info() const { return source_format; }

    mirgud::Frame next(uint64_t* capture_us, uint64_t* conversion_us)
    {
        auto const capture_start = std::chrono::steady_clock::now();
        auto const region = graphics_region(stream);
        if (region.width <= 0 || region.height <= 0 || region.stride <= 0)
            throw std::runtime_error{"invalid CPU screencast graphics region"};
        auto const conversion_start = std::chrono::steady_clock::now();
        mirgud::Frame frame{static_cast<uint32_t>(region.width), static_cast<uint32_t>(region.height),
            pixel_format,
            std::vector<uint8_t>(static_cast<std::size_t>(region.width) * region.height *
                mirgud::bytes_per_pixel(pixel_format))};
        try
        {
            mirgud::copy_rows(pixel_format, region.pixel_format,
                reinterpret_cast<uint8_t const*>(region.vaddr), region.stride,
                region.width, region.height, row_order, frame.pixels.data());
            auto const conversion_end = std::chrono::steady_clock::now();
            *capture_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                conversion_start - capture_start).count());
            *conversion_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                conversion_end - conversion_start).count());
            mir_buffer_stream_swap_buffers_sync(stream);
            return frame;
        }
        catch (...)
        {
            mir_buffer_stream_swap_buffers_sync(stream);
            throw;
        }
    }

private:
    MirBufferStream* stream;
    mirgud::PixelFormat pixel_format;
    mirgud::RowOrder row_order;
    mirgud::SourceFormat source_format;
};

class EglCapture
{
public:
    EglCapture(MirConnection* connection, MirBufferStream* stream, uint32_t width, uint32_t height,
               mirgud::PixelFormat pixel_format) : width{width}, height{height}, pixel_format{pixel_format}
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
        std::cerr << "mirgud: virtual frame source uses EGL readback fallback " << width << "x" << height <<
            " transport=" << mirgud::format_name(pixel_format) << std::endl;
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

    mirgud::Frame next(uint64_t* capture_us, uint64_t* conversion_us)
    {
        auto const capture_start = std::chrono::steady_clock::now();
        try
        {
            glReadPixels(0, 0, width, height, read_format, GL_UNSIGNED_BYTE, bytes.data());
            if (glGetError() != GL_NO_ERROR)
                throw std::runtime_error{"EGL screencast readback failed"};
            auto const conversion_start = std::chrono::steady_clock::now();
            mirgud::Frame frame{width, height, pixel_format,
                std::vector<uint8_t>(static_cast<std::size_t>(width) * height *
                    mirgud::bytes_per_pixel(pixel_format))};
            mirgud::copy_rows_from_rgba(pixel_format, bytes.data(), width, height,
                mirgud::RowOrder::bottom_up, frame.pixels.data());
            auto const conversion_end = std::chrono::steady_clock::now();
            *capture_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                conversion_start - capture_start).count());
            *conversion_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                conversion_end - conversion_start).count());
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
    mirgud::PixelFormat pixel_format;
    std::vector<uint8_t> bytes;
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLContext context{EGL_NO_CONTEXT};
    EGLSurface surface{EGL_NO_SURFACE};
    EGLConfig config{};
    GLenum read_format{};

public:
    GLenum source_read_format() const { return read_format; }
};

void report(Stats const& stats, pid_t monitor_pid)
{
    std::cerr << "mirgud: frames_received=" << stats.received <<
        " frames_submitted=" << stats.submitted <<
        " frames_presented=" << stats.presented <<
        " frames_dropped=" << stats.dropped;
    if (stats.submitted > 0)
    {
        auto const drop_pct = (static_cast<double>(stats.dropped) * 100.0) /
            static_cast<double>(stats.submitted);
        std::cerr << " drop_percent=" << drop_pct;
    }
    else
    {
        std::cerr << " drop_percent=0";
    }
    std::cerr << " conversion_failures=" << stats.conversion_failures <<
        " gud_submit_failures=" << stats.submit_failures;

    auto const report_timing = [](char const* label, mirgud::TimingSummary const& ts)
    {
        std::cerr << " " << label << "_us[min=" << ts.min <<
            " avg=" << ts.average() << " max=" << ts.max <<
            " p50=" << ts.percentile(50.0) << " p95=" << ts.percentile(95.0) << "]";
    };
    report_timing("capture", stats.capture_us);
    report_timing("conversion", stats.conversion_us);
    report_timing("submit", stats.submit_us);

    std::cerr << " submit_us_avg=" << mirgud::average(stats.submit_us.total, stats.presented) <<
        " capture_us_avg=" << mirgud::average(stats.capture_us.total, stats.received) <<
        " conversion_us_avg=" << mirgud::average(stats.conversion_us.total, stats.received);

    for (unsigned i = 0; i < 5; ++i)
    {
        if (stats.conversion_path_counts[i] > 0)
            std::cerr << " conversion_path=" <<
                mirgud::conversion_path_name(static_cast<mirgud::ConversionPath>(i)) <<
                ":" << stats.conversion_path_counts[i];
    }

    std::cerr << " self_fds=" << fd_count(getpid());
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
    std::string source_mode{"primary"};
    std::string pixel_format_str{"rgb565"};
    bool pattern{};
    bool quality{};
    bool no_gud{};
    bool extend_hold{};
    bool managed{};
    int gud_fd{-1};
    uint32_t gud_connector{};
    int status_fd{-1};
    std::string dump_frame;
    uint64_t dump_frame_after{1};
    uint64_t dump_frame_interval{0};
    std::vector<int> capture_region;
    po::options_description options{"Usage"};
    options.add_options()
        ("help,h", "show this help")
        ("mir-socket-file,m", po::value<std::string>(&socket), "Mir server socket")
        ("source-mode", po::value<std::string>(&source_mode),
            "source selection: primary (default) or extend (Aethercast-compatible)")
        ("pixel-format", po::value<std::string>(&pixel_format_str),
            "transport pixel format: rgb565 (default) or xrgb8888")
        ("size,s", po::value<std::vector<uint32_t>>()->multitoken(), "GUD/screencast size (default 1280 720)")
        ("cap-interval", po::value<uint32_t>(&capture_interval), "capture every N display intervals")
        ("monitor-pid", po::value<pid_t>(&monitor_pid), "sample this compositor PID's fd/sync_file counts")
        ("pattern", po::bool_switch(&pattern), "generated transport workload (static checkerboard, no Mir)")
        ("quality", po::bool_switch(&quality),
            "generate deterministic quality reference patterns and dump both RGB565 and XRGB8888 PPMs; requires --dump-frame")
        ("no-gud", po::bool_switch(&no_gud), "Mir source/copy/conversion benchmark: copy/release frames without opening GUD")
        ("extend-hold", po::bool_switch(&extend_hold),
            "hold an Aethercast-compatible extend screencast without reading frames")
        ("managed", po::bool_switch(&managed), "run as an xdispd-managed child")
        ("gud-fd", po::value<int>(&gud_fd), "inherited GUD DRM fd (managed mode)")
        ("gud-connector", po::value<uint32_t>(&gud_connector), "required GUD connector id (managed mode)")
        ("status-fd", po::value<int>(&status_fd), "managed status pipe fd")
        ("dump-frame", po::value<std::string>(&dump_frame),
            "write completed owned frames as binary PPM (prefix or single path)")
        ("dump-frame-after", po::value<uint64_t>(&dump_frame_after),
            "dump after this completed frame (default 1; requires --dump-frame)")
        ("dump-frame-interval", po::value<uint64_t>(&dump_frame_interval),
            "dump every N frames after --dump-frame-after (0 = dump once; requires --dump-frame)")
        ("capture-region", po::value<std::vector<int>>(&capture_region)->multitoken(),
            "experimental Mir screencast rectangle: X Y WIDTH HEIGHT");
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
    if (!capture_region.empty() && (capture_region.size() != 4 || capture_region[2] <= 0 || capture_region[3] <= 0))
        throw std::runtime_error{"capture-region requires X Y WIDTH HEIGHT, with positive WIDTH and HEIGHT"};
    if (source_mode != "primary" && source_mode != "extend")
        throw std::runtime_error{"source-mode must be primary or extend"};
    if (source_mode == "extend" && socket.empty())
        throw std::runtime_error{"Aethercast-compatible extend mode requires --mir-socket-file /run/mir_socket"};
    if (source_mode == "extend" && !capture_region.empty())
        throw std::runtime_error{"Aethercast-compatible extend mode calculates its own capture region; do not pass --capture-region"};
    if (extend_hold && (source_mode != "extend" || !no_gud))
        throw std::runtime_error{"--extend-hold requires --source-mode extend and --no-gud"};
    if (variables.count("dump-frame-after") && dump_frame.empty())
        throw std::runtime_error{"--dump-frame-after requires --dump-frame"};
    if (!dump_frame.empty() && dump_frame_after == 0)
        throw std::runtime_error{"--dump-frame-after must be positive"};
    if (variables.count("dump-frame-interval") && dump_frame.empty())
        throw std::runtime_error{"--dump-frame-interval requires --dump-frame"};
    if (quality && dump_frame.empty())
        throw std::runtime_error{"--quality requires --dump-frame"};
    if (quality && (pattern || extend_hold || managed))
        throw std::runtime_error{"--quality is exclusive with --pattern, --extend-hold, and --managed"};
    if (managed && (gud_fd < 0 || !gud_connector || status_fd < 0 || pattern || no_gud || extend_hold ||
        !dump_frame.empty() || source_mode != "extend" || socket != "/run/mir_socket"))
        throw std::runtime_error{"managed mode requires inherited GUD/status fds and the fixed extend source"};
    if (!managed && (variables.count("gud-fd") || variables.count("gud-connector") || variables.count("status-fd")))
        throw std::runtime_error{"managed fd options require --managed"};

    mirgud::PixelFormat pixel_format = mirgud::parse_pixel_format(pixel_format_str);

    managed_mode = managed;
    managed_status_fd = status_fd;
    if (managed_mode)
        prctl(PR_SET_PDEATHSIG, SIGTERM);
    running = true;
    stop_signal_received = 0;
    struct sigaction action{};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    if (quality)
    {
        std::cerr << "mirgud: deterministic quality reference generation" << std::endl;
        auto const write_quality = [&](std::string const& tag, std::vector<uint8_t> const& rgb888)
        {
            auto const rgb565_pixels = mirgud::rgb888_to_rgb565(rgb888.data(), width, height);
            auto const xrgb_pixels = mirgud::rgb888_to_xrgb8888(rgb888.data(), width, height);
            mirgud::Frame const rgb565_frame{width, height, mirgud::PixelFormat::rgb565, rgb565_pixels};
            mirgud::Frame const xrgb_frame{width, height, mirgud::PixelFormat::xrgb8888, xrgb_pixels};
            auto const rgb565_rgb = mirgud::frame_to_rgb888(rgb565_frame);
            auto const xrgb_rgb = mirgud::frame_to_rgb888(xrgb_frame);
            mirgud::write_ppm_rgb888(rgb565_rgb.data(), width, height,
                dump_frame + "." + tag + ".rgb565.ppm");
            mirgud::write_ppm_rgb888(xrgb_rgb.data(), width, height,
                dump_frame + "." + tag + ".xrgb8888.ppm");
        };
        write_quality("gradient", mirgud::gradient_pattern(width, height));
        write_quality("ramps", mirgud::color_ramps_pattern(width, height));
        write_quality("hfreq", mirgud::high_frequency_pattern(width, height));
        write_quality("photo", mirgud::photo_like_pattern(width, height));
        std::cerr << "mirgud: quality reference generation complete" << std::endl;
        return EXIT_SUCCESS;
    }
    std::unique_ptr<GudKms> kms;
    if (no_gud)
        std::cerr << "mirgud: GUD disabled for source-only stability probe" << std::endl;
    else
    {
        kms = std::make_unique<GudKms>(width, height, pixel_format, gud_fd, gud_connector);
        kms->setup();
        managed_status("MODESET_BEGIN");
        kms->initial_modeset();
        managed_status("MODESET_COMPLETE");
    }
    LatestFramePresenter presenter{[&kms](mirgud::Frame const& frame)
    {
        if (kms)
            kms->present(frame);
    }, [] { managed_status("ACTIVE"); }};

    if (pattern)
    {
        std::cerr << "mirgud: Stage A checkerboard started; verify it on HDMI before Stage B" << std::endl;
        auto const frame = mirgud::Frame{width, height, pixel_format,
            mirgud::checkerboard(pixel_format, width, height)};
        auto next_report = std::chrono::steady_clock::now();
        while (keep_running())
        {
            presenter.submit(frame);
            std::this_thread::sleep_for(std::chrono::seconds{1});
            if (std::chrono::steady_clock::now() >= next_report)
            {
                report(presenter.stats(), monitor_pid);
                next_report = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            }
        }
        presenter.stop();
        presenter.rethrow_failure();
        return EXIT_SUCCESS;
    }

    auto const connection = mir::raii::deleter_for(mir_connect_sync(socket.empty() ? nullptr : socket.c_str(),
        source_mode == "extend" ? "aethercast screencast client" : "mirgud"),
        [](MirConnection* value) {
            if (value)
            {
                managed_status("MIR_CONNECTION_RELEASE_BEGIN");
                mir_connection_release(value);
                managed_status("MIR_CONNECTION_RELEASE_COMPLETE");
            }
        });
    if (!connection || !mir_connection_is_valid(connection.get()))
        throw std::runtime_error{"cannot connect to Mir for virtual/screencast capture"};
    auto const display_configuration = mir::raii::deleter_for(
        mir_connection_create_display_configuration(connection.get()), &mir_display_config_release);
    if (!display_configuration)
        throw std::runtime_error{"cannot obtain Mir display configuration"};
    log_topology("before-screencast", *display_configuration);
    auto region = capture_region.empty() ? first_enabled_output(*display_configuration) :
        MirRectangle{capture_region[0], capture_region[1],
            static_cast<unsigned int>(capture_region[2]), static_cast<unsigned int>(capture_region[3])};
    if (source_mode == "extend")
    {
        region = aethercast_extend_region(*display_configuration);
        region.width = width;
        region.height = height;
        std::cerr << "mirgud: xdisp extend request size=" << width << "x" << height <<
            " mode=extend socket=" << socket << std::endl;
    }
    std::cerr << "mirgud: xdisp " << (source_mode == "extend" ? "extend" : "capture") <<
        " region requested=(" << region.left << "," << region.top << "," << region.width << "," <<
        region.height << ")" << std::endl;
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
    if (source_mode == "extend")
    {
        mir_screencast_spec_set_mirror_mode(spec, mir_mirror_mode_vertical);
        mir_screencast_spec_set_number_of_buffers(spec, 2);
    }
    auto const screencast = mir::raii::deleter_for(mir_screencast_create_sync(spec),
        [](MirScreencast* value) {
            if (value)
            {
                managed_status("SCREENCAST_RELEASE_BEGIN");
                mir_screencast_release_sync(value);
                managed_status("SCREENCAST_RELEASE_COMPLETE");
            }
        });
    mir_screencast_spec_release(spec);
    if (!screencast)
        throw std::runtime_error{"cannot create the Mir virtual/screencast stream"};
    auto const after_screencast_configuration = mir::raii::deleter_for(
        mir_connection_create_display_configuration(connection.get()), &mir_display_config_release);
    if (after_screencast_configuration)
        log_topology("after-screencast", *after_screencast_configuration);
    auto* const stream = mir_screencast_get_buffer_stream(screencast.get());
    if (!stream)
        throw std::runtime_error{"Mir screencast has no buffer stream"};
    std::cerr << "mirgud: Stage B virtual/screencast source enabled " << width << "x" << height <<
        " requested_format=" << static_cast<int>(format) << " transport=" <<
        mirgud::format_name(pixel_format) << std::endl;

    if (extend_hold)
    {
        std::cerr << "mirgud: xdisp extend hold active; preserving the Aethercast-compatible screencast lifetime"
            << std::endl;
        auto next_report = std::chrono::steady_clock::now();
        while (keep_running())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            if (std::chrono::steady_clock::now() >= next_report)
            {
                report(presenter.stats(), monitor_pid);
                next_report += std::chrono::seconds{1};
            }
        }
        return EXIT_SUCCESS;
    }

    auto next_report = std::chrono::steady_clock::now();
    auto dump_completed_frame = [&dump_frame, dump_frame_after, dump_frame_interval](
        mirgud::Frame const& frame, uint64_t frame_number)
    {
        if (dump_frame.empty())
            return;
        if (frame_number < dump_frame_after)
            return;
        if (dump_frame_interval == 0)
        {
            if (frame_number == dump_frame_after)
            {
                write_ppm(frame, dump_frame);
                std::cerr << "mirgud: dumped completed frame=" << frame_number << " path=" << dump_frame <<
                    " size=" << frame.width << "x" << frame.height << " format=" <<
                    mirgud::format_name(frame.format) << std::endl;
                dump_frame.clear();
            }
        }
        else if ((frame_number - dump_frame_after) % dump_frame_interval == 0)
        {
            auto const path = dump_frame + "." + std::to_string(frame_number) + ".ppm";
            write_ppm(frame, path);
            std::cerr << "mirgud: dumped completed frame=" << frame_number << " path=" << path <<
                " size=" << frame.width << "x" << frame.height << " format=" <<
                mirgud::format_name(frame.format) << std::endl;
        }
    };
    std::unique_ptr<DirectCapture> direct;
    try
    {
        direct = std::make_unique<DirectCapture>(stream, pixel_format,
            source_mode == "extend" ? mirgud::RowOrder::top_down : mirgud::RowOrder::bottom_up);
        presenter.set_conversion_path(direct->source_format_info().conversion_path);
    }
    catch (std::exception const& direct_error)
    {
        std::cerr << "mirgud: CPU mapping unavailable (" << direct_error.what() << "); using EGL readback" << std::endl;
    }
    if (direct)
    {
        bool first{};
        uint64_t prior_fingerprint{};
        bool have_prior_fingerprint{};
        uint64_t frame_number{};
        while (keep_running())
        {
            auto const start = std::chrono::steady_clock::now();
            try
            {
                uint64_t capture_us{};
                uint64_t conversion_us{};
                auto frame = direct->next(&capture_us, &conversion_us);
                presenter.received(capture_us, conversion_us);
                ++frame_number;
                dump_completed_frame(frame, frame_number);
                if (no_gud && (frame_number == 1 || frame_number % 60 == 0))
                {
                    auto const fingerprint = sampled_fingerprint(frame);
                    std::cerr << "mirgud: xdisp frame=" << frame_number << " hash=0x" << std::hex << fingerprint <<
                        std::dec << " changed=" << (!have_prior_fingerprint || fingerprint != prior_fingerprint) <<
                        " format=" << mirgud::format_name(frame.format) << std::endl;
                    prior_fingerprint = fingerprint;
                    have_prior_fingerprint = true;
                }
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
        EglCapture capture{connection.get(), stream, width, height, pixel_format};
        presenter.set_conversion_path(mirgud::conversion_path_for(pixel_format,
            capture.source_read_format() == GL_BGRA_EXT ? mir_pixel_format_argb_8888 : mir_pixel_format_abgr_8888));
        bool first{};
        uint64_t prior_fingerprint{};
        bool have_prior_fingerprint{};
        uint64_t frame_number{};
        while (keep_running())
        {
            auto const start = std::chrono::steady_clock::now();
            uint64_t capture_us{};
            uint64_t conversion_us{};
            auto frame = capture.next(&capture_us, &conversion_us);
            presenter.received(capture_us, conversion_us);
            ++frame_number;
            dump_completed_frame(frame, frame_number);
            if (no_gud && (frame_number == 1 || frame_number % 60 == 0))
            {
                auto const fingerprint = sampled_fingerprint(frame);
                std::cerr << "mirgud: xdisp frame=" << frame_number << " hash=0x" << std::hex << fingerprint <<
                    std::dec << " changed=" << (!have_prior_fingerprint || fingerprint != prior_fingerprint) <<
                    " format=" << mirgud::format_name(frame.format) << std::endl;
                prior_fingerprint = fingerprint;
                have_prior_fingerprint = true;
            }
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
    if (stop_signal_received)
        managed_status("TERM_OBSERVED");
    managed_status("CAPTURE_LOOP_EXIT");
    managed_status("PRESENTER_STOP_BEGIN");
    presenter.stop();
    managed_status("PRESENTER_STOP_COMPLETE");
    presenter.rethrow_failure();
    managed_status("PROCESS_EXIT");
    return EXIT_SUCCESS;
}
catch (std::exception const& error)
{
    std::cerr << "mirgud: " << error.what() << std::endl;
    if (!managed_mode)
        return EXIT_FAILURE;
    auto const* system = dynamic_cast<std::system_error const*>(&error);
    int const code = system ? system->code().value() : 0;
    if (code == ENODEV || code == ENOENT)
    {
        managed_status("FATAL U");
        return managed_unavailable;
    }
    if (code == ETIMEDOUT || code == EPROTO || code == EIO || code == EPIPE)
    {
        managed_status("FATAL P");
        return managed_poisoned;
    }
    managed_status("FATAL R");
    return managed_recoverable;
}
