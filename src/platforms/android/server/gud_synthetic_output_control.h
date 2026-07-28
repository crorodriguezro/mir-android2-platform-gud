/* Test-only control for determining whether the second output is required. */
#ifndef MIR_GRAPHICS_ANDROID_GUD_SYNTHETIC_OUTPUT_CONTROL_H_
#define MIR_GRAPHICS_ANDROID_GUD_SYNTHETIC_OUTPUT_CONTROL_H_

namespace mir
{
namespace graphics
{
namespace android
{
inline bool should_expose_synthetic_gud_output()
{
    return true;
}

inline bool should_bypass_synthetic_hwc_bookkeeping()
{
    return true;
}

inline bool should_offer_synthetic_output_to_compositor()
{
    return false;
}

inline bool should_mark_synthetic_output_used()
{
    return true;
}

inline bool should_mark_primary_output_used()
{
    return false;
}
}
}
}

#endif
