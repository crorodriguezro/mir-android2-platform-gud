#include "child_termination.h"

#include <gtest/gtest.h>

#include <csignal>
#include <sys/wait.h>

namespace
{
int exited(int code)
{
    return W_EXITCODE(code, 0);
}

int signaled(int signal)
{
    return signal;
}
}

TEST(XdispChildTermination, graceful_exit_after_requested_stop)
{
    auto const result = xdisp::classify_child_termination(exited(0), true, false);

    EXPECT_EQ(result.result, xdisp::ChildResult::stopped);
    EXPECT_EQ(result.description, "requested-graceful-stop:exit:0");
    EXPECT_FALSE(result.forced);
}

TEST(XdispChildTermination, forced_timeout_preserves_sigkill_and_containment)
{
    auto const result = xdisp::classify_child_termination(signaled(SIGKILL), true, true);

    EXPECT_EQ(result.result, xdisp::ChildResult::stopped);
    EXPECT_EQ(result.description, "forced-sigkill:signal:9");
    EXPECT_TRUE(result.forced);
}

TEST(XdispChildTermination, forced_stop_counter_increments)
{
    xdisp::StopDiagnostics diagnostics;
    auto const termination = xdisp::classify_child_termination(signaled(SIGKILL), true, true);
    diagnostics.child_exited(termination);
    diagnostics.stop_completed(termination, 3007);

    EXPECT_TRUE(diagnostics.last_stop_forced());
    EXPECT_EQ(diagnostics.forced_stop_count(), 1u);
    EXPECT_EQ(diagnostics.last_stop_duration_ms(), 3007u);
    EXPECT_EQ(diagnostics.last_child_exit(), "forced-sigkill:signal:9");
}

TEST(XdispChildTermination, spontaneous_exit_does_not_erase_last_stop_diagnostics)
{
    xdisp::StopDiagnostics diagnostics;
    auto const forced = xdisp::classify_child_termination(signaled(SIGKILL), true, true);
    diagnostics.child_exited(forced);
    diagnostics.stop_completed(forced, 3007);

    diagnostics.child_exited(xdisp::classify_child_termination(exited(21), false, false));

    EXPECT_EQ(diagnostics.last_child_exit(), "recoverable-source-failure:exit:21");
    EXPECT_TRUE(diagnostics.last_stop_forced());
    EXPECT_EQ(diagnostics.forced_stop_count(), 1u);
    EXPECT_EQ(diagnostics.last_stop_duration_ms(), 3007u);
}

TEST(XdispChildTermination, poison_is_never_hidden_by_stop_request)
{
    auto const result = xdisp::classify_child_termination(exited(22), true, true);

    EXPECT_EQ(result.result, xdisp::ChildResult::poisoned_transport);
    EXPECT_EQ(result.description, "poisoned-transport-failure:exit:22");
    EXPECT_FALSE(result.forced);
}

TEST(XdispChildTermination, unexpected_signal_is_recoverable)
{
    auto const result = xdisp::classify_child_termination(signaled(SIGSEGV), true, false);

    EXPECT_EQ(result.result, xdisp::ChildResult::recoverable_error);
    EXPECT_EQ(result.description, "unexpected-signal:11");
    EXPECT_FALSE(result.forced);
}

TEST(XdispChildTermination, requested_sigterm_is_graceful)
{
    auto const result = xdisp::classify_child_termination(signaled(SIGTERM), true, false);

    EXPECT_EQ(result.result, xdisp::ChildResult::stopped);
    EXPECT_EQ(result.description, "requested-graceful-stop:signal:15");
}
