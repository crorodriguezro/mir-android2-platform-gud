/*
 * Copyright \u00a9 2026 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 */

#ifndef MIR_GRAPHICS_ANDROID_GUD_HWC_BOUNDARY_H_
#define MIR_GRAPHICS_ANDROID_GUD_HWC_BOUNDARY_H_

#include "display_name.h"

namespace mir
{
namespace graphics
{
namespace android
{
inline bool should_submit_to_android_hwc(bool synthetic_gud_external, DisplayName display)
{
    return !synthetic_gud_external || display != DisplayName::external;
}
}
}
}

#endif
