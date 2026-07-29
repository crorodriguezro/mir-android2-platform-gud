#include "lifecycle.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct Harness
{
    Harness(bool enabled = false, bool poisoned = false) :
        lifecycle{enabled, poisoned, {
            [this] { ++starts; events.push_back("start"); },
            [this] { ++stops; events.push_back("stop"); },
            [this](bool value) { persisted_enabled = value; persisted_enabled_values.push_back(value); },
            [this](std::string const&) { events.push_back("persist_poison"); },
            [this] { events.push_back("clear_poison"); },
            [this](xdisp::State, xdisp::State next, std::string const&) { events.push_back(xdisp::name(next)); }
        }}
    {
    }

    int starts{};
    int stops{};
    bool persisted_enabled{};
    std::vector<bool> persisted_enabled_values;
    std::vector<std::string> events;
    xdisp::Lifecycle lifecycle;
};
}

TEST(XdispLifecycle, enables_discovers_and_activates_once)
{
    Harness h;
    EXPECT_TRUE(h.lifecycle.enable());
    EXPECT_EQ(xdisp::State::unavailable, h.lifecycle.state());
    h.lifecycle.sink_added();
    EXPECT_EQ(xdisp::State::available, h.lifecycle.state());
    EXPECT_TRUE(h.lifecycle.activate());
    EXPECT_TRUE(h.lifecycle.activate());
    EXPECT_EQ(1, h.starts);
    EXPECT_EQ(xdisp::State::connecting, h.lifecycle.state());
    h.lifecycle.child_active();
    EXPECT_EQ(xdisp::State::active, h.lifecycle.state());
}

TEST(XdispLifecycle, normal_deactivation_joins_before_available)
{
    Harness h{true};
    h.lifecycle.sink_added();
    h.lifecycle.activate();
    h.lifecycle.child_active();
    EXPECT_TRUE(h.lifecycle.deactivate());
    EXPECT_EQ(xdisp::State::disconnecting, h.lifecycle.state());
    EXPECT_EQ(1, h.stops);
    h.lifecycle.child_exited(xdisp::ChildResult::stopped, "stopped");
    EXPECT_EQ(xdisp::State::available, h.lifecycle.state());
}

TEST(XdispLifecycle, contained_forced_stop_returns_to_available)
{
    Harness h{true};
    h.lifecycle.sink_added();
    h.lifecycle.activate();
    h.lifecycle.child_active();
    h.lifecycle.deactivate();

    h.lifecycle.child_exited(xdisp::ChildResult::stopped, "forced-sigkill:signal:9");

    EXPECT_EQ(xdisp::State::available, h.lifecycle.state());
}

TEST(XdispLifecycle, detach_retains_intent_and_reconnects_only_after_fresh_add)
{
    Harness h{true};
    h.lifecycle.sink_added();
    h.lifecycle.activate();
    h.lifecycle.child_active();
    h.lifecycle.sink_removed();
    h.lifecycle.child_exited(xdisp::ChildResult::stopped, "detached");
    EXPECT_EQ(xdisp::State::unavailable, h.lifecycle.state());
    EXPECT_EQ(1, h.starts);
    h.lifecycle.sink_added();
    EXPECT_EQ(2, h.starts);
    EXPECT_EQ(xdisp::State::connecting, h.lifecycle.state());
}

TEST(XdispLifecycle, recoverable_error_requires_explicit_retry)
{
    Harness h{true};
    h.lifecycle.sink_added();
    h.lifecycle.activate();
    h.lifecycle.child_exited(xdisp::ChildResult::recoverable_error, "Mir failed");
    EXPECT_EQ(xdisp::State::recoverable_error, h.lifecycle.state());
    h.lifecycle.sink_added();
    EXPECT_EQ(1, h.starts);
    EXPECT_TRUE(h.lifecycle.activate());
    EXPECT_EQ(2, h.starts);
}

TEST(XdispLifecycle, poison_is_persisted_before_stop_and_blocks_activation)
{
    Harness h{true};
    h.lifecycle.sink_added();
    h.lifecycle.activate();
    h.lifecycle.poison("receiver poisoned");
    ASSERT_GE(h.events.size(), 3u);
    EXPECT_EQ("persist_poison", h.events[h.events.size() - 3]);
    EXPECT_EQ("poisoned_transport", h.events[h.events.size() - 2]);
    EXPECT_EQ("stop", h.events.back());
    ASSERT_FALSE(h.persisted_enabled_values.empty());
    EXPECT_FALSE(h.persisted_enabled_values.back());
    EXPECT_FALSE(h.lifecycle.activate());
    EXPECT_FALSE(h.lifecycle.enable());
}

TEST(XdispLifecycle, poison_clear_requires_remove_and_readd_and_returns_disabled)
{
    Harness h{false, true};
    EXPECT_FALSE(h.lifecycle.clear_poison());
    h.lifecycle.sink_removed();
    EXPECT_FALSE(h.lifecycle.clear_poison());
    h.lifecycle.sink_added();
    EXPECT_TRUE(h.lifecycle.recovery_observed());
    EXPECT_TRUE(h.lifecycle.clear_poison());
    EXPECT_EQ(xdisp::State::disabled, h.lifecycle.state());
    EXPECT_EQ(0, h.starts);
}

TEST(XdispLifecycle, disable_stops_child_and_clears_activation_intent)
{
    Harness h{true};
    h.lifecycle.sink_added();
    h.lifecycle.activate();
    EXPECT_TRUE(h.lifecycle.disable());
    EXPECT_FALSE(h.lifecycle.enabled());
    EXPECT_FALSE(h.lifecycle.activation_requested());
    EXPECT_EQ(xdisp::State::disconnecting, h.lifecycle.state());
    h.lifecycle.child_exited(xdisp::ChildResult::stopped, "disabled");
    EXPECT_EQ(xdisp::State::disabled, h.lifecycle.state());
}

TEST(XdispLifecycle, start_failure_becomes_recoverable_without_a_phantom_child)
{
    xdisp::Lifecycle lifecycle{true, false, {
        [] { throw std::runtime_error{"fork failed"}; }, {}, {}, {}, {}, {}
    }};
    lifecycle.sink_added();
    EXPECT_FALSE(lifecycle.activate());
    EXPECT_EQ(xdisp::State::recoverable_error, lifecycle.state());
    EXPECT_FALSE(lifecycle.activate());
    EXPECT_EQ(xdisp::State::recoverable_error, lifecycle.state());
}

TEST(XdispLifecycle, connecting_deadline_stops_child_in_recoverable_error)
{
    Harness h{true};
    h.lifecycle.sink_added();
    h.lifecycle.activate();
    h.lifecycle.connecting_timed_out();
    EXPECT_EQ(xdisp::State::recoverable_error, h.lifecycle.state());
    EXPECT_EQ(1, h.stops);
    h.lifecycle.child_exited(xdisp::ChildResult::stopped, "terminated");
    EXPECT_EQ(xdisp::State::recoverable_error, h.lifecycle.state());
}

TEST(XdispLifecycle, restored_poison_never_enables_or_activates)
{
    Harness h{true, true};
    EXPECT_EQ(xdisp::State::poisoned_transport, h.lifecycle.state());
    EXPECT_FALSE(h.lifecycle.enabled());
    h.lifecycle.sink_added();
    EXPECT_FALSE(h.lifecycle.activate());
    EXPECT_EQ(0, h.starts);
}
