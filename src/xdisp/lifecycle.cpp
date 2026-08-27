#include "lifecycle.h"

#include <utility>

namespace xdisp
{
char const* name(State state)
{
    switch (state)
    {
    case State::unavailable: return "unavailable";
    case State::available: return "available";
    case State::connecting: return "connecting";
    case State::active: return "active";
    case State::disconnecting: return "disconnecting";
    case State::recoverable_error: return "recoverable_error";
    case State::poisoned_transport: return "poisoned_transport";
    case State::disabled: return "disabled";
    }
    return "disabled";
}

Lifecycle::Lifecycle(bool enabled, bool poisoned, LifecycleActions actions) :
    actions{std::move(actions)},
    current{poisoned ? State::poisoned_transport : (enabled ? State::unavailable : State::disabled)},
    enabled_intent{enabled && !poisoned}
{
}

State Lifecycle::state() const { return current; }
bool Lifecycle::enabled() const { return enabled_intent; }
bool Lifecycle::activation_requested() const { return active_intent; }
bool Lifecycle::sink_present() const { return present; }
bool Lifecycle::recovery_observed() const { return recovered; }

void Lifecycle::transition(State next, std::string const& reason)
{
    if (current == next)
        return;
    auto const previous = current;
    current = next;
    if (actions.state_changed)
        actions.state_changed(previous, next, reason);
}

bool Lifecycle::enable()
{
    if (current == State::poisoned_transport)
        return false;
    if (!enabled_intent)
    {
        if (actions.persist_enabled)
        {
            try
            {
                actions.persist_enabled(true);
            }
            catch (...)
            {
                return false;
            }
        }
        enabled_intent = true;
    }
    transition(present ? State::available : State::unavailable, "enabled");
    start_if_requested();
    return true;
}

bool Lifecycle::disable()
{
    active_intent = false;
    if (enabled_intent)
    {
        if (actions.persist_enabled)
        {
            try
            {
                actions.persist_enabled(false);
            }
            catch (...)
            {
                return false;
            }
        }
        enabled_intent = false;
    }
    if (current == State::poisoned_transport)
        return true;
    if (child_running)
        stop_child(State::disabled, "disabled");
    else
        transition(State::disabled, "disabled");
    return true;
}

bool Lifecycle::activate()
{
    if (!enabled_intent || current == State::poisoned_transport)
        return false;
    active_intent = true;
    if (current == State::recoverable_error && present)
        transition(State::available, "explicit retry");
    start_if_requested();
    return current != State::recoverable_error;
}

bool Lifecycle::deactivate()
{
    active_intent = false;
    if (current == State::poisoned_transport)
        return false;
    if (child_running)
        stop_child(enabled_intent && present ? State::available :
            (enabled_intent ? State::unavailable : State::disabled), "deactivated");
    else if (current == State::recoverable_error)
        transition(present ? State::available : State::unavailable, "deactivated");
    return true;
}

void Lifecycle::sink_added()
{
    bool const newly_present = !present;
    present = true;
    if (current == State::poisoned_transport)
    {
        if (saw_poisoned_remove)
            recovered = true;
        return;
    }
    if (!newly_present || !enabled_intent || child_running)
        return;
    transition(State::available, "GUD sink available");
    start_if_requested();
}

void Lifecycle::sink_removed()
{
    present = false;
    // A physical removal ends the current activation request.  Reappearance
    // publishes availability only; the operator must explicitly Activate a
    // fresh mirgud child after reconnect.
    active_intent = false;
    if (current == State::poisoned_transport)
    {
        saw_poisoned_remove = true;
        recovered = false;
        return;
    }
    if (child_running)
        stop_child(enabled_intent ? State::unavailable : State::disabled, "GUD sink removed");
    else if (enabled_intent)
        transition(State::unavailable, "GUD sink removed");
}

void Lifecycle::start_if_requested()
{
    if (!active_intent || !enabled_intent || !present || child_running || current == State::poisoned_transport)
        return;
    child_running = true;
    transition(State::connecting, "activation requested");
    if (actions.start)
    {
        try
        {
            actions.start();
        }
        catch (...)
        {
            child_running = false;
            transition(State::recoverable_error, "failed to start mirgud");
        }
    }
}

void Lifecycle::stop_child(State target, std::string const& reason)
{
    disconnect_target = target;
    transition(State::disconnecting, reason);
    if (actions.stop)
        actions.stop();
}

void Lifecycle::child_active()
{
    if (child_running && current == State::connecting)
        transition(State::active, "first frame presented");
}

void Lifecycle::connecting_timed_out()
{
    if (!child_running || current != State::connecting)
        return;
    transition(State::recoverable_error, "mirgud did not present a frame before the activation deadline");
    if (actions.stop)
        actions.stop();
}

void Lifecycle::child_exited(ChildResult result, std::string const& reason)
{
    child_running = false;
    if (current == State::poisoned_transport)
        return;
    if (result == ChildResult::poisoned_transport)
    {
        poison(reason);
        return;
    }
    if (current == State::disconnecting && result == ChildResult::stopped)
    {
        // The sink may have reappeared while bounded containment was still
        // reaping the old child.  Publish the current availability after the
        // old child is gone; activation remains explicit because removal
        // cleared active_intent above.
        transition(enabled_intent && present ? State::available : disconnect_target, reason);
        return;
    }
    if (result == ChildResult::unavailable)
    {
        present = false;
        transition(enabled_intent ? State::unavailable : State::disabled, reason);
    }
    else if (result == ChildResult::recoverable_error || active_intent)
        transition(State::recoverable_error, reason);
    else
        transition(enabled_intent && present ? State::available :
            (enabled_intent ? State::unavailable : State::disabled), reason);
}

void Lifecycle::poison(std::string const& reason)
{
    if (current == State::poisoned_transport)
        return;
    try
    {
        if (actions.persist_poison)
            actions.persist_poison(reason);
        if (enabled_intent && actions.persist_enabled)
            actions.persist_enabled(false);
    }
    catch (...)
    {
        enabled_intent = false;
        active_intent = false;
        transition(State::poisoned_transport, "transport poison persistence failed closed");
        if (child_running && actions.stop)
            actions.stop();
        return;
    }
    enabled_intent = false;
    active_intent = false;
    transition(State::poisoned_transport, reason);
    if (child_running && actions.stop)
        actions.stop();
}

bool Lifecycle::clear_poison()
{
    if (current != State::poisoned_transport || !recovered)
        return false;
    if (actions.clear_poison)
    {
        try
        {
            actions.clear_poison();
        }
        catch (...)
        {
            return false;
        }
    }
    saw_poisoned_remove = false;
    recovered = false;
    transition(State::disabled, "transport poison cleared after physical recovery");
    return true;
}
}
