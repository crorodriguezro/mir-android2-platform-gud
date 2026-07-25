/* Fixed 1280x720 RGB565 GUD sink used only by the external-output POC. */
#include "gud_output.h"

#include "buffer.h"
#include "display_device.h"
#include "display_name.h"
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

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

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

uint32_t property(int fd, uint32_t object, uint32_t type, char const* name)
{
    auto* properties = drmModeObjectGetProperties(fd, object, type);
    if (!properties) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < properties->count_props; ++i)
    {
        auto* candidate = drmModeGetProperty(fd, properties->props[i]);
        if (candidate)
        {
            if (!std::strcmp(candidate->name, name)) result = candidate->prop_id;
            drmModeFreeProperty(candidate);
        }
        if (result) break;
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
    struct Frame { drm_mode_create_dumb dumb{}; uint32_t fb{}; void* map{MAP_FAILED}; } frames[2];
    int fd{-1};
    uint32_t connector{}, crtc{}, plane{}, blob{};
    Props props{};
    unsigned next{};
    void allocate(Frame& frame);
    void commit(Frame const& frame, bool modeset);
};

Kms::Kms()
{
    fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (fd < 0 || drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
        throw std::runtime_error("cannot open GUD atomic KMS");

    auto* resources = drmModeGetResources(fd);
    drmModeConnector* selected = nullptr;
    drmModeModeInfo const* mode = nullptr;
    for (int i = 0; resources && i < resources->count_connectors && !selected; ++i)
    {
        auto* candidate = drmModeGetConnector(fd, resources->connectors[i]);
        if (candidate && candidate->connection == DRM_MODE_CONNECTED)
            for (int j = 0; j < candidate->count_modes; ++j)
                if (candidate->modes[j].hdisplay == width && candidate->modes[j].vdisplay == height)
                { selected = candidate; mode = &candidate->modes[j]; break; }
        if (candidate && candidate != selected) drmModeFreeConnector(candidate);
    }
    if (!selected) throw std::runtime_error("no connected 1280x720 GUD output");
    connector = selected->connector_id;
    auto* encoder = selected->encoder_id ? drmModeGetEncoder(fd, selected->encoder_id) : nullptr;
    if (!encoder && selected->count_encoders) encoder = drmModeGetEncoder(fd, selected->encoders[0]);
    if (!encoder) throw std::runtime_error("GUD encoder unavailable");
    crtc = encoder->crtc_id;
    uint32_t crtc_index = 0;
    for (int i = 0; resources && i < resources->count_crtcs; ++i)
    {
        if (!crtc && (encoder->possible_crtcs & (1U << i))) crtc = resources->crtcs[i];
        if (resources->crtcs[i] == crtc) crtc_index = i;
    }
    drmModeFreeEncoder(encoder);
    auto* planes = drmModeGetPlaneResources(fd);
    for (uint32_t i = 0; planes && i < planes->count_planes && !plane; ++i)
    {
        auto* candidate = drmModeGetPlane(fd, planes->planes[i]);
        if (candidate && (candidate->possible_crtcs & (1U << crtc_index)) && primary_plane(fd, candidate->plane_id))
            plane = candidate->plane_id;
        if (candidate) drmModeFreePlane(candidate);
    }
    if (planes) drmModeFreePlaneResources(planes);
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
    if (!resources || !crtc || !plane || !props.connector_crtc || !props.crtc_mode || !props.crtc_active ||
        !props.plane_fb || !props.plane_crtc || !props.src_x || !props.src_y || !props.src_w || !props.src_h ||
        !props.crtc_x || !props.crtc_y || !props.crtc_w || !props.crtc_h ||
        drmModeCreatePropertyBlob(fd, mode, sizeof(*mode), &blob))
        throw std::runtime_error("GUD KMS setup failed");
    drmModeFreeConnector(selected);
    drmModeFreeResources(resources);
    allocate(frames[0]); allocate(frames[1]);
    std::memset(frames[0].map, 0, frames[0].dumb.size);
    commit(frames[0], true);
}

Kms::~Kms()
{
    for (auto& frame : frames)
    {
        if (frame.map != MAP_FAILED) munmap(frame.map, frame.dumb.size);
        if (frame.fb) drmModeRmFB(fd, frame.fb);
        if (frame.dumb.handle) { drm_mode_destroy_dumb destroy{}; destroy.handle = frame.dumb.handle;
            drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy); }
    }
    if (blob) drmModeDestroyPropertyBlob(fd, blob);
    if (fd >= 0) close(fd);
}

void Kms::allocate(Frame& frame)
{
    frame.dumb.width = width; frame.dumb.height = height; frame.dumb.bpp = 16;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &frame.dumb)) throw std::runtime_error("GUD dumb allocation failed");
    drm_mode_map_dumb map{}; map.handle = frame.dumb.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) throw std::runtime_error("GUD dumb mapping failed");
    frame.map = mmap(nullptr, frame.dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    uint32_t handles[] = {frame.dumb.handle, 0, 0, 0};
    uint32_t pitches[] = {frame.dumb.pitch, 0, 0, 0};
    uint32_t offsets[] = {0, 0, 0, 0};
    if (frame.map == MAP_FAILED || drmModeAddFB2(fd, width, height, DRM_FORMAT_RGB565,
        handles, pitches, offsets, &frame.fb, 0)) throw std::runtime_error("GUD framebuffer failed");
}

void Kms::commit(Frame const& frame, bool modeset)
{
    auto* request = drmModeAtomicAlloc();
    auto add = [request](uint32_t o, uint32_t p, uint64_t v) { return drmModeAtomicAddProperty(request, o, p, v) >= 0; };
    bool ok = add(plane, props.plane_fb, frame.fb);
    if (modeset) ok = ok && add(connector, props.connector_crtc, crtc) && add(crtc, props.crtc_mode, blob) &&
        add(crtc, props.crtc_active, 1) && add(plane, props.plane_crtc, crtc) && add(plane, props.src_x, 0) &&
        add(plane, props.src_y, 0) && add(plane, props.src_w, width << 16) && add(plane, props.src_h, height << 16) &&
        add(plane, props.crtc_x, 0) && add(plane, props.crtc_y, 0) && add(plane, props.crtc_w, width) && add(plane, props.crtc_h, height);
    int const rc = ok ? drmModeAtomicCommit(fd, request, modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0, nullptr) : -1;
    drmModeAtomicFree(request);
    if (rc) throw std::runtime_error("GUD atomic commit failed");
}

void Kms::present(mga::Buffer& source)
{
    if (source.size().width.as_uint32_t() != width || source.size().height.as_uint32_t() != height)
        throw std::runtime_error("GUD POC needs a 1280x720 Mir external output");
    auto& frame = frames[next];
    auto const stride = source.stride().as_uint32_t();
    auto const format = source.pixel_format();
    source.read([&](unsigned char const* data) {
        for (uint32_t y = 0; y < height; ++y)
        {
            auto const* src = data + y * stride;
            auto* dst = static_cast<uint16_t*>(frame.map) + y * frame.dumb.pitch / 2;
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t r, g, b;
                if (format == mir_pixel_format_argb_8888 || format == mir_pixel_format_xrgb_8888)
                { b = src[4*x]; g = src[4*x+1]; r = src[4*x+2]; }
                else { r = src[4*x]; g = src[4*x+1]; b = src[4*x+2]; }
                dst[x] = static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
            }
        }
    });
    commit(frame, false);
    next ^= 1;
}

std::unique_ptr<Kms>& output()
{
    static std::unique_ptr<Kms> instance;
    static bool attempted = false;
    if (!attempted)
    {
        attempted = true;
        try { instance = std::make_unique<Kms>(); mir::log_info("GUD POC output enabled"); }
        catch (std::exception const& e) { mir::log_warning("GUD POC unavailable: %s", e.what()); }
    }
    return instance;
}
}

void mga::GudOutput::present_external(std::list<DisplayContents> const& contents)
{
    auto& sink = output();
    if (!sink) return;
    for (auto const& content : contents)
    {
        if (content.name != DisplayName::external) continue;
        auto buffer = std::dynamic_pointer_cast<mga::Buffer>(content.context.last_rendered_buffer());
        if (!buffer) return;
        try { sink->present(*buffer); }
        catch (std::exception const& e) { mir::log_warning("GUD POC dropped frame: %s", e.what()); }
        return;
    }
}
