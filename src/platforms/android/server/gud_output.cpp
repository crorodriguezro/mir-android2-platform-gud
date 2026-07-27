/* Fixed 1280x720 RGB565 GUD sink used only by the external-output POC. */
#include "gud_output.h"

#include "buffer.h"
#include "display_device.h"
#include "display_name.h"
#include "gud_presentation_worker.h"
#include "swapping_gl_context.h"

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#define MIR_LOG_COMPONENT "android-gud-poc"
#include <mir/log.h>

namespace mg = mir::graphics;
namespace mga = mir::graphics::android;

namespace
{
constexpr uint32_t width = 1280;
constexpr uint32_t height = 720;

struct Props
{
    uint32_t connector_crtc, crtc_mode, crtc_active, plane_fb, plane_crtc;
    uint32_t src_x, src_y, src_w, src_h, crtc_x, crtc_y, crtc_w, crtc_h;
};

std::runtime_error drm_error(char const* operation)
{
    return std::runtime_error{std::string{operation} + ": " + std::strerror(errno)};
}

/*
 * This is intentionally only a present-time lookup. It avoids baking a card
 * number into the POC, but it is not P0.3's remove/add event handling.
 */
int open_gud_card()
{
    constexpr unsigned maximum_cards_to_scan = 16;
    for (unsigned card = 0; card != maximum_cards_to_scan; ++card)
    {
        auto const node = std::string{"/dev/dri/card"} + std::to_string(card);
        int const fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        auto* version = drmGetVersion(fd);
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
    auto* properties = drmModeObjectGetProperties(fd, object, type);
    if (!properties)
        return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < properties->count_props; ++i)
    {
        auto* candidate = drmModeGetProperty(fd, properties->props[i]);
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
    auto const type = property(fd, id, DRM_MODE_OBJECT_PLANE, "type");
    auto* properties = type ? drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE) : nullptr;
    bool result = false;
    if (properties)
    {
        for (uint32_t i = 0; i < properties->count_props; ++i)
            if (properties->props[i] == type && properties->prop_values[i] == DRM_PLANE_TYPE_PRIMARY)
                result = true;
        drmModeFreeObjectProperties(properties);
    }
    return result;
}

class Kms
{
public:
    Kms();
    ~Kms();
    void present(mga::Buffer& source);
private:
    struct Frame
    {
        drm_mode_create_dumb dumb{};
        uint32_t fb{};
        void* map{MAP_FAILED};
    };

    Frame frames[2];
    int fd{-1};
    uint32_t connector{}, crtc{}, plane{}, blob{};
    Props props{};
    unsigned next{};

    void setup();
    void teardown();
    void allocate(Frame& frame);
    void commit(Frame const& frame, bool modeset);
};

Kms::Kms()
{
    fd = open_gud_card();
    if (fd < 0)
        throw std::runtime_error{"no accessible GUD DRM card"};

    try
    {
        if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
            drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
            throw drm_error("cannot enable GUD atomic KMS client capabilities");

        setup();
        allocate(frames[0]);
        allocate(frames[1]);
        std::memset(frames[0].map, 0, frames[0].dumb.size);
        commit(frames[0], true);
    }
    catch (...)
    {
        teardown();
        throw;
    }
}

Kms::~Kms()
{
    teardown();
}

void Kms::setup()
{
    auto resources = std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)>{
        drmModeGetResources(fd), drmModeFreeResources};
    if (!resources)
        throw drm_error("cannot query GUD KMS resources");

    uint32_t encoder_id{};
    drmModeModeInfo mode{};
    for (int i = 0; i < resources->count_connectors && !connector; ++i)
    {
        auto candidate = std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>{
            drmModeGetConnector(fd, resources->connectors[i]), drmModeFreeConnector};
        if (!candidate || candidate->connection != DRM_MODE_CONNECTED)
            continue;
        for (int j = 0; j < candidate->count_modes; ++j)
            if (candidate->modes[j].hdisplay == width && candidate->modes[j].vdisplay == height)
            {
                connector = candidate->connector_id;
                encoder_id = candidate->encoder_id;
                if (!encoder_id && candidate->count_encoders)
                    encoder_id = candidate->encoders[0];
                mode = candidate->modes[j];
                break;
            }
    }
    if (!connector || !encoder_id)
        throw std::runtime_error{"no connected 1280x720 GUD output"};

    auto encoder = std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)>{
        drmModeGetEncoder(fd, encoder_id), drmModeFreeEncoder};
    if (!encoder)
        throw drm_error("GUD encoder unavailable");
    crtc = encoder->crtc_id;
    uint32_t crtc_index{};
    for (int i = 0; i < resources->count_crtcs; ++i)
    {
        if (!crtc && (encoder->possible_crtcs & (1U << i)))
            crtc = resources->crtcs[i];
        if (resources->crtcs[i] == crtc)
            crtc_index = i;
    }

    auto planes = std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)>{
        drmModeGetPlaneResources(fd), drmModeFreePlaneResources};
    if (planes)
        for (uint32_t i = 0; i < planes->count_planes && !plane; ++i)
        {
            auto candidate = std::unique_ptr<drmModePlane, decltype(&drmModeFreePlane)>{
                drmModeGetPlane(fd, planes->planes[i]), drmModeFreePlane};
            if (candidate && (candidate->possible_crtcs & (1U << crtc_index)) &&
                primary_plane(fd, candidate->plane_id))
                plane = candidate->plane_id;
        }

    props = {property(fd, connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"),
             property(fd, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"),
             property(fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_Y"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W"),
             property(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H")};
    if (!crtc || !plane || !props.connector_crtc || !props.crtc_mode || !props.crtc_active ||
        !props.plane_fb || !props.plane_crtc || !props.src_x || !props.src_y || !props.src_w ||
        !props.src_h || !props.crtc_x || !props.crtc_y || !props.crtc_w || !props.crtc_h)
        throw std::runtime_error{"GUD KMS setup is incomplete"};
    if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob))
        throw drm_error("cannot create GUD mode blob");
}

void Kms::teardown()
{
    for (auto& frame : frames)
    {
        if (frame.map != MAP_FAILED)
            munmap(frame.map, frame.dumb.size);
        if (fd >= 0 && frame.fb)
            drmModeRmFB(fd, frame.fb);
        if (fd >= 0 && frame.dumb.handle)
        {
            drm_mode_destroy_dumb destroy{};
            destroy.handle = frame.dumb.handle;
            drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
        frame = {};
        frame.map = MAP_FAILED;
    }
    if (fd >= 0 && blob)
        drmModeDestroyPropertyBlob(fd, blob);
    blob = 0;
    if (fd >= 0)
        close(fd);
    fd = -1;
}

void Kms::allocate(Frame& frame)
{
    frame.dumb.width = width;
    frame.dumb.height = height;
    frame.dumb.bpp = 16;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &frame.dumb))
        throw drm_error("GUD dumb allocation failed");
    drm_mode_map_dumb map{};
    map.handle = frame.dumb.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
        throw drm_error("GUD dumb mapping failed");
    frame.map = mmap(nullptr, frame.dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    uint32_t handles[] = {frame.dumb.handle, 0, 0, 0};
    uint32_t pitches[] = {frame.dumb.pitch, 0, 0, 0};
    uint32_t offsets[] = {0, 0, 0, 0};
    if (frame.map == MAP_FAILED || drmModeAddFB2(fd, width, height, DRM_FORMAT_RGB565,
        handles, pitches, offsets, &frame.fb, 0))
        throw drm_error("GUD framebuffer allocation failed");
}

void Kms::commit(Frame const& frame, bool modeset)
{
    auto* request = drmModeAtomicAlloc();
    if (!request)
        throw std::runtime_error{"cannot allocate GUD atomic request"};
    auto add = [request](uint32_t object, uint32_t prop, uint64_t value)
    {
        return drmModeAtomicAddProperty(request, object, prop, value) >= 0;
    };
    bool ok = add(plane, props.plane_fb, frame.fb);
    if (modeset)
        ok = ok && add(connector, props.connector_crtc, crtc) && add(crtc, props.crtc_mode, blob) &&
            add(crtc, props.crtc_active, 1) && add(plane, props.plane_crtc, crtc) &&
            add(plane, props.src_x, 0) && add(plane, props.src_y, 0) &&
            add(plane, props.src_w, width << 16) && add(plane, props.src_h, height << 16) &&
            add(plane, props.crtc_x, 0) && add(plane, props.crtc_y, 0) &&
            add(plane, props.crtc_w, width) && add(plane, props.crtc_h, height);
    int const rc = ok ? drmModeAtomicCommit(fd, request,
        modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0, nullptr) : -1;
    drmModeAtomicFree(request);
    if (rc)
        throw drm_error("GUD atomic commit failed");
}

void Kms::present(mga::Buffer& source)
{
    if (source.size().width.as_uint32_t() != width || source.size().height.as_uint32_t() != height)
        throw std::runtime_error{"GUD POC needs a 1280x720 Mir external output"};
    auto& frame = frames[next];
    auto const stride = source.stride().as_uint32_t();
    auto const format = source.pixel_format();
    source.read([&](unsigned char const* data)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            auto const* src = data + y * stride;
            auto* dst = static_cast<uint16_t*>(frame.map) + y * frame.dumb.pitch / 2;
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t r, g, b;
                if (format == mir_pixel_format_argb_8888 || format == mir_pixel_format_xrgb_8888)
                {
                    b = src[4*x];
                    g = src[4*x+1];
                    r = src[4*x+2];
                }
                else
                {
                    r = src[4*x];
                    g = src[4*x+1];
                    b = src[4*x+2];
                }
                dst[x] = static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
            }
        }
    });
    commit(frame, false);
    next ^= 1;
}

class GudPresenter
{
public:
    void present(std::shared_ptr<mga::Buffer> const& source)
    {
        auto const now = std::chrono::steady_clock::now();
        if (now < retry_after)
            return;

        try
        {
            if (!kms)
            {
                kms = std::make_unique<Kms>();
                mir::log_info("GUD POC output enabled");
            }
            kms->present(*source);
        }
        catch (...)
        {
            /* Do not retain a disconnected fd or half-initialized KMS state. */
            kms.reset();
            retry_after = now + std::chrono::seconds{1};
            throw;
        }
    }

private:
    std::unique_ptr<Kms> kms;
    std::chrono::steady_clock::time_point retry_after{};
};

void report_gud_failure(std::exception_ptr error)
{
    try
    {
        std::rethrow_exception(error);
    }
    catch (std::exception const& e)
    {
        mir::log_warning("GUD POC frame dropped; retrying when the card is usable: %s", e.what());
    }
    catch (...)
    {
        mir::log_warning("GUD POC frame dropped after an unknown presentation failure");
    }
}

class GudPresentation
{
public:
    GudPresentation() :
        presenter{std::make_shared<GudPresenter>()},
        worker{
            [presenter = presenter](std::shared_ptr<mga::Buffer> const& frame)
            {
                presenter->present(frame);
            },
            report_gud_failure}
    {
    }

    std::shared_ptr<GudPresenter> const presenter;
    mga::LatestPresentationWorker<std::shared_ptr<mga::Buffer>> worker;
};

std::mutex output_mutex;
std::unique_ptr<GudPresentation> output;
std::once_flag no_external_frame_notice;
std::once_flag non_android_frame_notice;
}

bool mga::GudOutput::available()
{
    int const fd = open_gud_card();
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

void mga::GudOutput::present_external(std::list<DisplayContents> const& contents)
{
    for (auto const& content : contents)
    {
        if (content.name != DisplayName::external)
            continue;
        auto buffer = std::dynamic_pointer_cast<mga::Buffer>(content.context.last_rendered_buffer());
        if (!buffer)
        {
            std::call_once(non_android_frame_notice, []
            {
                mir::log_info("GUD POC external output has no Android frame; worker remains idle");
            });
            return;
        }

        /* This lock only protects worker lifetime; it is never held by KMS I/O. */
        std::lock_guard<std::mutex> lock{output_mutex};
        if (!output)
        {
            output = std::make_unique<GudPresentation>();
            mir::log_info("GUD POC presentation worker started");
        }
        output->worker.submit(std::move(buffer));
        return;
    }

    std::call_once(no_external_frame_notice, []
    {
        mir::log_info("GUD POC external output is configured but has no DisplayContents frame");
    });
}

void mga::GudOutput::shutdown()
{
    std::unique_ptr<GudPresentation> old_output;
    {
        std::lock_guard<std::mutex> lock{output_mutex};
        old_output = std::move(output);
    }
    /* Destruction drops pending work and joins before KMS memory is released. */
}
