/* Startup-only selection of a single advertised GUD connector mode. */
#ifndef MIR_GRAPHICS_ANDROID_GUD_MODE_SELECTION_H_
#define MIR_GRAPHICS_ANDROID_GUD_MODE_SELECTION_H_

#include <cstddef>
#include <cstdint>

namespace mir
{
namespace graphics
{
namespace android
{
struct GudModeCandidate
{
    uint32_t width;
    uint32_t height;
    uint32_t index;
    bool preferred;
};

/*
 * P0.2 needs one source geometry, not a mode-management policy. Prefer the
 * mode advertised as preferred and otherwise keep the connector's first
 * usable mode. Hotplug and mode changes after this selection are P0.3.
 */
inline GudModeCandidate const* select_startup_gud_mode(
    GudModeCandidate const* candidates, std::size_t count)
{
    GudModeCandidate const* fallback = nullptr;
    for (std::size_t i = 0; i != count; ++i)
    {
        auto const& candidate = candidates[i];
        if (!candidate.width || !candidate.height)
            continue;
        if (!fallback)
            fallback = &candidate;
        if (candidate.preferred)
            return &candidate;
    }
    return fallback;
}
}
}
}

#endif
