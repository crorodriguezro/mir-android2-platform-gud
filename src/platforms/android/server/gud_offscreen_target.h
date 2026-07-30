/* The synthetic GUD sink must not use Android's window-surface queue. */
#ifndef MIR_GRAPHICS_ANDROID_GUD_OFFSCREEN_TARGET_H_
#define MIR_GRAPHICS_ANDROID_GUD_OFFSCREEN_TARGET_H_

#include "display_name.h"

namespace mir
{
namespace graphics
{
namespace android
{
inline bool should_use_gud_offscreen_target(bool synthetic_gud_external, DisplayName display)
{
    return synthetic_gud_external && display == DisplayName::external;
}
}
}
}

#endif
