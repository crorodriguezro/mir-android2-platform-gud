#ifndef XDISP_LIFECYCLE_H_
#define XDISP_LIFECYCLE_H_

#include <functional>
#include <string>

namespace xdisp
{
enum class State
{
    unavailable,
    available,
    connecting,
    active,
    disconnecting,
    recoverable_error,
    poisoned_transport,
    disabled
};

enum class ChildResult
{
    stopped,
    unavailable,
    recoverable_error,
    poisoned_transport
};

char const* name(State state);

struct LifecycleActions
{
    std::function<void()> start;
    std::function<void()> stop;
    std::function<void(bool)> persist_enabled;
    std::function<void(std::string const&)> persist_poison;
    std::function<void()> clear_poison;
    std::function<void(State, State, std::string const&)> state_changed;
};

class Lifecycle
{
public:
    Lifecycle(bool enabled, bool poisoned, LifecycleActions actions);

    State state() const;
    bool enabled() const;
    bool activation_requested() const;
    bool sink_present() const;
    bool recovery_observed() const;

    bool enable();
    bool disable();
    bool activate();
    bool deactivate();
    void sink_added();
    void sink_removed();
    void child_active();
    void connecting_timed_out();
    void child_exited(ChildResult result, std::string const& reason);
    void poison(std::string const& reason);
    bool clear_poison();

private:
    void transition(State next, std::string const& reason);
    void start_if_requested();
    void stop_child(State target, std::string const& reason);

    LifecycleActions actions;
    State current;
    State disconnect_target{State::unavailable};
    bool enabled_intent{};
    bool active_intent{};
    bool present{};
    bool child_running{};
    bool saw_poisoned_remove{};
    bool recovered{};
};
}

#endif
