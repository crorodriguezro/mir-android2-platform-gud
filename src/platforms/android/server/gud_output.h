/* Minimal GUD scanout sink for the Android2 external-output proof of concept. */
#ifndef MIR_GRAPHICS_ANDROID_GUD_OUTPUT_H_
#define MIR_GRAPHICS_ANDROID_GUD_OUTPUT_H_

#include <cstdint>
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
    struct Mode
    {
        Mode(uint32_t width = 0, uint32_t height = 0, double vrefresh_hz = 0.0) :
            width{width},
            height{height},
            vrefresh_hz{vrefresh_hz}
        {
        }

        uint32_t width;
        uint32_t height;
        double vrefresh_hz;

        bool valid() const { return width && height; }
    };

    /*
     * Read the connected GUD mode once while Mir starts its synthetic output.
     * It deliberately does not subscribe to later card/mode changes (P0.3).
     */
    static Mode startup_mode();
    /* True only when a currently accessible GUD card has a connected mode. */
    static bool available();
    static void present_external(std::list<DisplayContents> const& contents);
    static void shutdown();
};
}
}
}

#endif
