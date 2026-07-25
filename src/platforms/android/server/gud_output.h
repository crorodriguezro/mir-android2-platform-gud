/* Minimal GUD scanout sink for the Android2 external-output proof of concept. */
#ifndef MIR_GRAPHICS_ANDROID_GUD_OUTPUT_H_
#define MIR_GRAPHICS_ANDROID_GUD_OUTPUT_H_

#include <list>

namespace mir
{
namespace graphics
{
namespace android
{
struct DisplayContents;

class GudOutput
{
public:
    static void present_external(std::list<DisplayContents> const& contents);
};
}
}
}

#endif
