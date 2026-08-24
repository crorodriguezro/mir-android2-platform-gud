/* Canonical E4-T01 identity shared with gud-gadget without changing GUD. */
#ifndef MIRGUD_MODE_CONTRACT_H_
#define MIRGUD_MODE_CONTRACT_H_

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace mirgud
{
struct ModeContractTiming
{
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint32_t flags;
};

constexpr uint32_t gud_mode_flag_user_mask = 0x000033ffU;

inline uint64_t mode_contract_id(
    ModeContractTiming const& timing, uint8_t format, uint8_t connector)
{
    uint64_t hash = 14695981039346656037ULL;
    auto const add = [&hash](uint8_t byte)
    {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    auto const add_u16 = [&add](uint16_t value)
    {
        add(static_cast<uint8_t>(value));
        add(static_cast<uint8_t>(value >> 8));
    };
    auto const add_u32 = [&add](uint32_t value)
    {
        add(static_cast<uint8_t>(value));
        add(static_cast<uint8_t>(value >> 8));
        add(static_cast<uint8_t>(value >> 16));
        add(static_cast<uint8_t>(value >> 24));
    };

    add(1); /* Contract schema version. */
    add(connector);
    add(format);
    add_u32(timing.clock);
    add_u16(timing.hdisplay);
    add_u16(timing.hsync_start);
    add_u16(timing.hsync_end);
    add_u16(timing.htotal);
    add_u16(timing.vdisplay);
    add_u16(timing.vsync_start);
    add_u16(timing.vsync_end);
    add_u16(timing.vtotal);
    add_u32(timing.flags & gud_mode_flag_user_mask);
    return hash;
}

inline std::string mode_contract_id_string(uint64_t id)
{
    std::ostringstream output;
    output << "e4c1-" << std::hex << std::setfill('0') << std::setw(16) << id;
    return output.str();
}
}

#endif
