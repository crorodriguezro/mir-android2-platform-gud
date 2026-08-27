#include "child_termination.h"

#include <csignal>
#include <sys/wait.h>

namespace xdisp
{
ChildTermination classify_child_termination(int status, bool stop_requested, bool forced_kill_requested)
{
    if (WIFSIGNALED(status))
    {
        int const signal = WTERMSIG(status);
        if (stop_requested && forced_kill_requested && signal == SIGKILL)
            return {ChildResult::stopped, "forced-sigkill:signal:" + std::to_string(signal), true};
        if (stop_requested && signal == SIGTERM)
            return {ChildResult::stopped, "requested-graceful-stop:signal:" + std::to_string(signal), false};
        return {ChildResult::recoverable_error, "unexpected-signal:" + std::to_string(signal), false};
    }

    if (!WIFEXITED(status))
        return {ChildResult::recoverable_error, "unknown-wait-status:" + std::to_string(status), false};

    int const code = WEXITSTATUS(status);
    if (code == 22)
        return {ChildResult::poisoned_transport, "poisoned-transport-failure:exit:22", false};
    if (code == 20)
        return {ChildResult::unavailable, "source-unavailable:exit:20", false};
    if (code != 0)
        return {ChildResult::recoverable_error, "recoverable-source-failure:exit:" + std::to_string(code), false};
    if (stop_requested)
        return {ChildResult::stopped, "requested-graceful-stop:exit:0", false};
    return {ChildResult::stopped, "normal-exit:0", false};
}

void StopDiagnostics::child_exited(ChildTermination const& termination)
{
    last_exit = termination.description;
}

void StopDiagnostics::stop_completed(ChildTermination const& termination, uint64_t duration_ms)
{
    last_forced = termination.forced;
    last_duration_ms = duration_ms;
    if (termination.forced)
        ++forced_count;
}
}
