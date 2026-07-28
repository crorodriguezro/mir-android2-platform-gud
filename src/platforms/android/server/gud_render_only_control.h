/* Test-only policy for isolating the synthetic Android EGL render path. */
#ifndef MIR_GRAPHICS_ANDROID_GUD_RENDER_ONLY_CONTROL_H_
#define MIR_GRAPHICS_ANDROID_GUD_RENDER_ONLY_CONTROL_H_

namespace mir
{
namespace graphics
{
namespace android
{
inline bool should_start_gud_presentation_worker()
{
    return false;
}
}
}
}

#endif
