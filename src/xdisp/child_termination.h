#pragma once

#include "lifecycle.h"

#include <cstdint>
#include <string>

namespace xdisp
{
struct ChildTermination
{
    ChildResult result;
    std::string description;
    bool forced;
};

ChildTermination classify_child_termination(int wait_status, bool stop_requested, bool forced_kill_requested);

class StopDiagnostics
{
public:
    void child_exited(ChildTermination const& termination);
    void stop_completed(ChildTermination const& termination, uint64_t duration_ms);

    std::string const& last_child_exit() const { return last_exit; }
    bool last_stop_forced() const { return last_forced; }
    uint64_t forced_stop_count() const { return forced_count; }
    uint64_t last_stop_duration_ms() const { return last_duration_ms; }

private:
    std::string last_exit{"none"};
    bool last_forced{};
    uint64_t forced_count{};
    uint64_t last_duration_ms{};
};
}
