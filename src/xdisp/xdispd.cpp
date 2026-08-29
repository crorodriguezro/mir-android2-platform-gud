#include "lifecycle.h"
#include "child_termination.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
constexpr guint default_activation_timeout_seconds = 10;
constexpr guint diagnostic_activation_timeout_max_seconds = 45;
constexpr std::size_t max_drm_cards_to_inspect = 64;

guint diagnostic_activation_timeout_seconds()
{
    auto const* value = std::getenv("XDISP_DIAGNOSTIC_ACTIVATION_TIMEOUT_SECONDS");
    if (!value || !*value)
        return default_activation_timeout_seconds;
    try
    {
        std::size_t parsed_length{};
        auto const parsed = std::stoul(value, &parsed_length);
        if (parsed_length == std::strlen(value) && parsed >= 1 &&
            parsed <= diagnostic_activation_timeout_max_seconds)
        {
            g_message("xdispd: diagnostic activation timeout override=%lu seconds",
                static_cast<unsigned long>(parsed));
            return static_cast<guint>(parsed);
        }
    }
    catch (...)
    {
    }
    g_warning("xdispd: ignoring invalid XDISP_DIAGNOSTIC_ACTIVATION_TIMEOUT_SECONDS; using %u seconds",
        default_activation_timeout_seconds);
    return default_activation_timeout_seconds;
}

char const* const bus_name = "org.lomiri.XDisp";
char const* const object_path = "/org/lomiri/XDisp";
char const* const interface_name = "org.lomiri.XDisp1";
char const* const state_dir = "/home/phablet/.local/share/lomiri-xdisp";

char const introspection_xml[] = R"XML(
<node>
  <interface name="org.lomiri.XDisp1">
    <method name="Enable"/><method name="Disable"/>
    <method name="Activate"/><method name="Deactivate"/>
    <method name="Poison"><arg name="reason" type="s" direction="in"/></method>
    <method name="ClearPoison"/>
    <property name="State" type="s" access="read"/>
    <property name="Enabled" type="b" access="read"/>
    <property name="ActivationRequested" type="b" access="read"/>
    <property name="GudDevice" type="s" access="read"/>
    <property name="GudIdentity" type="s" access="read"/>
    <property name="ConnectorId" type="u" access="read"/>
    <property name="ChildPid" type="u" access="read"/>
    <property name="LastError" type="s" access="read"/>
    <property name="LastLifecycleEvent" type="s" access="read"/>
    <property name="RecoveryObserved" type="b" access="read"/>
    <property name="LastChildExit" type="s" access="read"/>
    <property name="LastStopForced" type="b" access="read"/>
    <property name="ForcedStopCount" type="t" access="read"/>
    <property name="LastStopDurationMs" type="t" access="read"/>
    <signal name="StateChanged">
      <arg name="old_state" type="s"/><arg name="new_state" type="s"/><arg name="reason" type="s"/>
    </signal>
  </interface>
</node>)XML";

struct Candidate
{
    int fd{-1};
    bool usable{};
    std::string devnode;
    std::string identity;
    uint32_t connector{};
    dev_t device_number{};

    Candidate() = default;
    Candidate(Candidate&& other) noexcept :
        fd{other.fd}, usable{other.usable}, devnode{std::move(other.devnode)}, identity{std::move(other.identity)},
        connector{other.connector}, device_number{other.device_number}
    {
        other.fd = -1;
    }
    Candidate& operator=(Candidate&& other) noexcept
    {
        if (this != &other)
        {
            if (fd >= 0)
                close(fd);
            fd = other.fd;
            usable = other.usable;
            devnode = std::move(other.devnode);
            identity = std::move(other.identity);
            connector = other.connector;
            device_number = other.device_number;
            other.fd = -1;
        }
        return *this;
    }
    ~Candidate() { if (fd >= 0) close(fd); }
    Candidate(Candidate const&) = delete;
    Candidate& operator=(Candidate const&) = delete;
    explicit operator bool() const { return usable; }
};

bool exists(std::string const& path)
{
    return access(path.c_str(), F_OK) == 0;
}

bool auto_enabled()
{
    // First install is enabled by default so a connected GUD display works
    // without an operator command. The explicit disabled marker preserves a
    // user's Disable choice across service restarts; the legacy enabled
    // marker remains accepted implicitly because its absence was previously
    // the only way to represent the initial state.
    return !exists(std::string{state_dir} + "/disabled");
}

std::string first_line(std::string const& path)
{
    std::ifstream input{path};
    std::string value;
    std::getline(input, value);
    return value;
}

void atomic_write(std::string const& path, std::string const& value)
{
    auto const temporary = path + ".tmp";
    int const fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        throw std::system_error{errno, std::system_category(), "cannot create xdisp state"};
    ssize_t const written = write(fd, value.data(), value.size());
    bool const failed = written != static_cast<ssize_t>(value.size()) || fsync(fd);
    int saved_errno = failed ? (errno ? errno : EIO) : 0;
    if (close(fd) && !saved_errno)
        saved_errno = errno;
    if (!saved_errno && rename(temporary.c_str(), path.c_str()))
        saved_errno = errno;
    if (saved_errno)
    {
        unlink(temporary.c_str());
        throw std::system_error{saved_errno, std::system_category(), "cannot persist xdisp state"};
    }
    int const directory = open(state_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0)
    {
        fsync(directory);
        close(directory);
    }
}

bool card_name(char const* sysname)
{
    if (!sysname || std::strncmp(sysname, "card", 4))
        return false;
    if (!sysname[4])
        return false;
    return std::all_of(sysname + 4, sysname + std::strlen(sysname), [](char c) { return c >= '0' && c <= '9'; });
}

Candidate probe(udev_device* device)
{
    Candidate result;
    auto const* devnode = udev_device_get_devnode(device);
    if (!devnode)
        return result;
    auto const device_number = udev_device_get_devnum(device);
    if (!device_number)
        return result;
    result.fd = open(devnode, O_RDWR | O_CLOEXEC);
    if (result.fd < 0)
        return result;
    struct stat card_stat{};
    if (fstat(result.fd, &card_stat) || card_stat.st_rdev != device_number)
        return {};
    auto* const version = drmGetVersion(result.fd);
    bool const gud = version && version->name && !std::strcmp(version->name, "gud");
    if (version)
        drmFreeVersion(version);
    if (!gud)
    {
        close(result.fd);
        result.fd = -1;
        return result;
    }
    auto resources = std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)>{
        drmModeGetResources(result.fd), drmModeFreeResources};
    if (!resources)
        return {};
    for (int index = 0; index != resources->count_connectors && !result.connector; ++index)
    {
        auto connector = std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>{
            drmModeGetConnector(result.fd, resources->connectors[index]), drmModeFreeConnector};
        if (!connector || connector->connection != DRM_MODE_CONNECTED)
            continue;
        for (int mode = 0; mode != connector->count_modes; ++mode)
            if (connector->modes[mode].hdisplay == 1280 && connector->modes[mode].vdisplay == 720)
            {
                result.connector = connector->connector_id;
                break;
            }
    }
    if (!result.connector)
        return {};
    result.devnode = devnode;
    result.device_number = udev_device_get_devnum(device);
    auto const* syspath = udev_device_get_syspath(device);
    result.identity = syspath ? syspath : devnode;
    result.usable = true;
    drmDropMaster(result.fd);
    close(result.fd);
    result.fd = -1;
    return result;
}

std::vector<Candidate> discover(udev* context)
{
    std::vector<Candidate> candidates;
    auto enumerate = std::unique_ptr<udev_enumerate, decltype(&udev_enumerate_unref)>{
        udev_enumerate_new(context), udev_enumerate_unref};
    if (!enumerate)
        return candidates;
    udev_enumerate_add_match_subsystem(enumerate.get(), "drm");
    udev_enumerate_scan_devices(enumerate.get());
    udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate.get());
    udev_list_entry* entry{};
    std::size_t inspected{};
    udev_list_entry_foreach(entry, devices)
    {
        if (inspected++ >= max_drm_cards_to_inspect)
            break;
        auto device = std::unique_ptr<udev_device, decltype(&udev_device_unref)>{
            udev_device_new_from_syspath(context, udev_list_entry_get_name(entry)), udev_device_unref};
        if (!device || !card_name(udev_device_get_sysname(device.get())))
            continue;
        auto candidate = probe(device.get());
        if (candidate)
            candidates.push_back(std::move(candidate));
    }
    return candidates;
}

class Daemon
{
public:
    Daemon() :
        context{udev_new(), udev_unref},
        lifecycle{auto_enabled(), exists(std::string{state_dir} + "/poisoned"), {
            [this] { start_child(); }, [this] { stop_child(); },
            [](bool value) {
                auto const enabled_path = std::string{state_dir} + "/enabled";
                auto const disabled_path = std::string{state_dir} + "/disabled";
                if (value)
                {
                    atomic_write(enabled_path, "enabled\n");
                    unlink(disabled_path.c_str());
                }
                else
                {
                    atomic_write(disabled_path, "disabled\n");
                    unlink(enabled_path.c_str());
                }
            },
            [this](std::string const& reason) {
                atomic_write(std::string{state_dir} + "/poisoned", candidate.identity + "\n" + reason + "\n");
            },
            [] { unlink((std::string{state_dir} + "/poisoned").c_str()); },
            [this](xdisp::State old_state, xdisp::State new_state, std::string const& reason) {
                last_error = new_state == xdisp::State::recoverable_error ||
                    new_state == xdisp::State::poisoned_transport ? reason : std::string{};
                emit_state(old_state, new_state, reason);
            }
        }, true}
    {
        if (!context)
            throw std::runtime_error{"cannot initialize udev"};
        if (lifecycle.state() == xdisp::State::poisoned_transport)
            poisoned_identity = first_line(std::string{state_dir} + "/poisoned");
    }

    void run()
    {
        loop = g_main_loop_new(nullptr, FALSE);
        owner = g_bus_own_name(G_BUS_TYPE_SYSTEM, bus_name, G_BUS_NAME_OWNER_FLAGS_NONE,
            &Daemon::bus_acquired, nullptr, &Daemon::name_lost, this, nullptr);
        monitor.reset(udev_monitor_new_from_netlink(context.get(), "udev"));
        if (!monitor || udev_monitor_filter_add_match_subsystem_devtype(monitor.get(), "drm", nullptr) ||
            udev_monitor_enable_receiving(monitor.get()))
            throw std::runtime_error{"cannot monitor DRM udev events"};
        monitor_channel = g_io_channel_unix_new(udev_monitor_get_fd(monitor.get()));
        g_io_channel_set_close_on_unref(monitor_channel, FALSE);
        monitor_watch = g_io_add_watch(monitor_channel, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP),
            &Daemon::udev_event, this);
        if (lifecycle.state() != xdisp::State::poisoned_transport)
            reconcile();
        g_main_loop_run(loop);
    }

    void shutdown()
    {
        shutting_down = true;
        lifecycle.deactivate();
        if (child_pid <= 0 && loop)
            g_main_loop_quit(loop);
    }

    ~Daemon()
    {
        if (child_pid > 0)
        {
            kill(child_pid, SIGKILL);
            waitpid(child_pid, nullptr, 0);
        }
        if (kill_source) g_source_remove(kill_source);
        if (activation_source) g_source_remove(activation_source);
        if (monitor_watch) g_source_remove(monitor_watch);
        if (monitor_channel) g_io_channel_unref(monitor_channel);
        if (registration && connection) g_dbus_connection_unregister_object(connection, registration);
        if (owner) g_bus_unown_name(owner);
        if (node) g_dbus_node_info_unref(node);
        if (loop) g_main_loop_unref(loop);
    }

private:
    using Udev = std::unique_ptr<udev, decltype(&udev_unref)>;
    using Monitor = std::unique_ptr<udev_monitor, decltype(&udev_monitor_unref)>;

    static void bus_acquired(GDBusConnection* connection, char const*, gpointer data)
    {
        auto& self = *static_cast<Daemon*>(data);
        self.connection = connection;
        GError* error{};
        self.node = g_dbus_node_info_new_for_xml(introspection_xml, &error);
        if (!self.node)
        {
            g_warning("xdispd: invalid introspection XML: %s", error->message);
            g_error_free(error);
            return;
        }
        static GDBusInterfaceVTable const vtable{&Daemon::method_call, &Daemon::get_property, nullptr, {nullptr}};
        self.registration = g_dbus_connection_register_object(connection, object_path, self.node->interfaces[0],
            &vtable, &self, nullptr, &error);
        if (!self.registration)
        {
            g_warning("xdispd: cannot register D-Bus object: %s", error->message);
            g_error_free(error);
        }
    }

    static void name_lost(GDBusConnection*, char const*, gpointer data)
    {
        auto& self = *static_cast<Daemon*>(data);
        if (self.loop)
            g_main_loop_quit(self.loop);
    }

    static bool root_sender(GDBusConnection* connection, char const* sender)
    {
        GError* error{};
        auto* value = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "GetConnectionUnixUser", g_variant_new("(s)", sender),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
        if (!value)
        {
            if (error) g_error_free(error);
            return false;
        }
        guint32 uid{};
        g_variant_get(value, "(u)", &uid);
        g_variant_unref(value);
        return uid == 0;
    }

    static void method_call(GDBusConnection* connection, char const* sender, char const*, char const*,
        char const* method, GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data)
    {
        auto& self = *static_cast<Daemon*>(data);
        bool accepted = true;
        if (!std::strcmp(method, "Enable")) accepted = self.lifecycle.enable();
        else if (!std::strcmp(method, "Disable")) accepted = self.lifecycle.disable();
        else if (!std::strcmp(method, "Activate")) accepted = self.lifecycle.activate();
        else if (!std::strcmp(method, "Deactivate")) accepted = self.lifecycle.deactivate();
        else if (!std::strcmp(method, "Poison"))
        {
            if (!root_sender(connection, sender))
                accepted = false;
            else
            {
                char const* reason{};
                g_variant_get(parameters, "(&s)", &reason);
                self.lifecycle.poison(reason && *reason ? reason : "transport poison reported");
            }
        }
        else if (!std::strcmp(method, "ClearPoison"))
            accepted = root_sender(connection, sender) && self.lifecycle.clear_poison();
        else
        {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                "Unknown xdisp method");
            return;
        }
        self.emit_properties();
        if (!accepted)
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                "Operation is not allowed in state %s", xdisp::name(self.lifecycle.state()));
        else
            g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    static GVariant* get_property(GDBusConnection*, char const*, char const*, char const*, char const* property,
        GError**, gpointer data)
    {
        auto& self = *static_cast<Daemon*>(data);
        if (!std::strcmp(property, "State")) return g_variant_new_string(xdisp::name(self.lifecycle.state()));
        if (!std::strcmp(property, "Enabled")) return g_variant_new_boolean(self.lifecycle.enabled());
        if (!std::strcmp(property, "ActivationRequested")) return g_variant_new_boolean(self.lifecycle.activation_requested());
        if (!std::strcmp(property, "GudDevice")) return g_variant_new_string(self.candidate.devnode.c_str());
        if (!std::strcmp(property, "GudIdentity")) return g_variant_new_string(self.candidate.identity.c_str());
        if (!std::strcmp(property, "ConnectorId")) return g_variant_new_uint32(self.candidate.connector);
        if (!std::strcmp(property, "ChildPid")) return g_variant_new_uint32(self.child_pid > 0 ? self.child_pid : 0);
        if (!std::strcmp(property, "LastError")) return g_variant_new_string(self.last_error.c_str());
        if (!std::strcmp(property, "LastLifecycleEvent")) return g_variant_new_string(self.last_lifecycle_event.c_str());
        if (!std::strcmp(property, "RecoveryObserved")) return g_variant_new_boolean(self.lifecycle.recovery_observed());
        if (!std::strcmp(property, "LastChildExit")) return g_variant_new_string(self.stop_diagnostics.last_child_exit().c_str());
        if (!std::strcmp(property, "LastStopForced")) return g_variant_new_boolean(self.stop_diagnostics.last_stop_forced());
        if (!std::strcmp(property, "ForcedStopCount")) return g_variant_new_uint64(self.stop_diagnostics.forced_stop_count());
        if (!std::strcmp(property, "LastStopDurationMs")) return g_variant_new_uint64(self.stop_diagnostics.last_stop_duration_ms());
        return nullptr;
    }

    void emit_state(xdisp::State old_state, xdisp::State new_state, std::string const& reason)
    {
        last_lifecycle_event = std::string{xdisp::name(new_state)} + ":" +
            (reason.empty() ? "state change" : reason);
        g_message("xdispd: %s -> %s: %s", xdisp::name(old_state), xdisp::name(new_state), reason.c_str());
        if (!connection)
            return;
        g_dbus_connection_emit_signal(connection, nullptr, object_path, interface_name, "StateChanged",
            g_variant_new("(sss)", xdisp::name(old_state), xdisp::name(new_state), reason.c_str()), nullptr);
        emit_properties();
    }

    void emit_properties()
    {
        if (!connection)
            return;
        GVariantBuilder changed;
        g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&changed, "{sv}", "State", g_variant_new_string(xdisp::name(lifecycle.state())));
        g_variant_builder_add(&changed, "{sv}", "Enabled", g_variant_new_boolean(lifecycle.enabled()));
        g_variant_builder_add(&changed, "{sv}", "ActivationRequested", g_variant_new_boolean(lifecycle.activation_requested()));
        g_variant_builder_add(&changed, "{sv}", "GudDevice", g_variant_new_string(candidate.devnode.c_str()));
        g_variant_builder_add(&changed, "{sv}", "GudIdentity", g_variant_new_string(candidate.identity.c_str()));
        g_variant_builder_add(&changed, "{sv}", "ConnectorId", g_variant_new_uint32(candidate.connector));
        g_variant_builder_add(&changed, "{sv}", "ChildPid", g_variant_new_uint32(child_pid > 0 ? child_pid : 0));
        g_variant_builder_add(&changed, "{sv}", "LastError", g_variant_new_string(last_error.c_str()));
        g_variant_builder_add(&changed, "{sv}", "LastLifecycleEvent", g_variant_new_string(last_lifecycle_event.c_str()));
        g_variant_builder_add(&changed, "{sv}", "RecoveryObserved", g_variant_new_boolean(lifecycle.recovery_observed()));
        g_variant_builder_add(&changed, "{sv}", "LastChildExit", g_variant_new_string(stop_diagnostics.last_child_exit().c_str()));
        g_variant_builder_add(&changed, "{sv}", "LastStopForced", g_variant_new_boolean(stop_diagnostics.last_stop_forced()));
        g_variant_builder_add(&changed, "{sv}", "ForcedStopCount", g_variant_new_uint64(stop_diagnostics.forced_stop_count()));
        g_variant_builder_add(&changed, "{sv}", "LastStopDurationMs", g_variant_new_uint64(stop_diagnostics.last_stop_duration_ms()));
        GVariantBuilder invalidated;
        g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
        g_dbus_connection_emit_signal(connection, nullptr, object_path, "org.freedesktop.DBus.Properties",
            "PropertiesChanged", g_variant_new("(sa{sv}as)", interface_name, &changed, &invalidated), nullptr);
    }

    static gboolean udev_event(GIOChannel*, GIOCondition condition, gpointer data)
    {
        auto& self = *static_cast<Daemon*>(data);
        if (condition & (G_IO_ERR | G_IO_HUP))
            return TRUE;
        auto device = std::unique_ptr<udev_device, decltype(&udev_device_unref)>{
            udev_monitor_receive_device(self.monitor.get()), udev_device_unref};
        if (!device)
            return TRUE;
        auto const* action = udev_device_get_action(device.get());
        auto const* syspath = udev_device_get_syspath(device.get());
        if (self.lifecycle.state() == xdisp::State::poisoned_transport && syspath)
        {
            if (action && !std::strcmp(action, "remove") && card_name(udev_device_get_sysname(device.get())) &&
                (self.poisoned_identity.empty() || syspath == self.poisoned_identity))
            {
                self.poisoned_identity = syspath;
                self.poison_remove_observed = true;
                self.candidate = {};
                self.lifecycle.sink_removed();
            }
            else if (self.poison_remove_observed && action &&
                (!std::strcmp(action, "add") || !std::strcmp(action, "change")) &&
                card_name(udev_device_get_sysname(device.get())))
            {
                auto recovered = probe(device.get());
                if (recovered && recovered.identity == self.poisoned_identity)
                {
                    self.candidate = std::move(recovered);
                    self.lifecycle.sink_added();
                }
            }
            self.emit_properties();
        }
        else
            self.reconcile();
        return TRUE;
    }

    void reconcile()
    {
        auto candidates = discover(context.get());
        if (candidates.size() > 1)
        {
            last_error = "multiple usable GUD devices are ambiguous";
            if (candidate)
            {
                candidate = {};
                lifecycle.sink_removed();
            }
            emit_properties();
            return;
        }
        if (candidates.empty())
        {
            if (candidate)
            {
                candidate = {};
                lifecycle.sink_removed();
                emit_properties();
            }
            return;
        }
        if (candidate && lifecycle.sink_present() && candidate.identity == candidates[0].identity &&
            candidate.connector == candidates[0].connector)
            return;
        if (candidate)
            lifecycle.sink_removed();
        candidate = std::move(candidates[0]);
        lifecycle.sink_added();
        emit_properties();
    }

    void start_child()
    {
        if (!candidate || child_pid > 0)
            return;
        int status_pipe[2];
        if (pipe2(status_pipe, O_CLOEXEC | O_NONBLOCK))
            throw std::system_error{errno, std::system_category(), "cannot create mirgud status pipe"};
        int const child_card = open(candidate.devnode.c_str(), O_RDWR | O_CLOEXEC);
        if (child_card < 0)
        {
            close(status_pipe[0]); close(status_pipe[1]);
            throw std::system_error{errno, std::system_category(), "cannot reopen current GUD card"};
        }
        struct stat card_stat{};
        if (fstat(child_card, &card_stat) || card_stat.st_rdev != candidate.device_number)
        {
            int const saved_errno = errno ? errno : ENODEV;
            close(child_card);
            close(status_pipe[0]); close(status_pipe[1]);
            throw std::system_error{saved_errno, std::system_category(), "GUD card identity changed before activation"};
        }
        auto reopened_device = std::unique_ptr<udev_device, decltype(&udev_device_unref)>{
            udev_device_new_from_devnum(context.get(), 'c', card_stat.st_rdev), udev_device_unref};
        auto const* reopened_path = reopened_device ? udev_device_get_syspath(reopened_device.get()) : nullptr;
        if (!reopened_path || candidate.identity != reopened_path)
        {
            close(child_card);
            close(status_pipe[0]); close(status_pipe[1]);
            throw std::system_error{ENODEV, std::system_category(), "GUD card sysfs identity changed before activation"};
        }
        int const spawn_card = fcntl(child_card, F_DUPFD_CLOEXEC, 10);
        int const spawn_status = fcntl(status_pipe[1], F_DUPFD_CLOEXEC, 10);
        close(child_card);
        if (spawn_card < 0 || spawn_status < 0)
        {
            if (spawn_card >= 0) close(spawn_card);
            if (spawn_status >= 0) close(spawn_status);
            close(status_pipe[0]); close(status_pipe[1]);
            throw std::system_error{errno, std::system_category(), "cannot prepare mirgud descriptors"};
        }
        char connector[32];
        std::snprintf(connector, sizeof(connector), "%u", candidate.connector);
        // Keep the production path direct: Mir delivers packed RGB565 and GUD
        // scans out the same format without an intermediate conversion.
        char* arguments[] = {
            const_cast<char*>(XDISP_MIRGUD_PATH), const_cast<char*>("--managed"), const_cast<char*>("--gud-fd"),
            const_cast<char*>("3"), const_cast<char*>("--gud-connector"), connector,
            const_cast<char*>("--status-fd"), const_cast<char*>("4"), const_cast<char*>("--source-mode"),
            const_cast<char*>("extend"), const_cast<char*>("--pixel-format"),
            const_cast<char*>("rgb565"), const_cast<char*>("--source-pixel-format"), const_cast<char*>("rgb565"),
            const_cast<char*>("--mir-socket-file"), const_cast<char*>("/run/mir_socket"),
            const_cast<char*>("--size"), const_cast<char*>("1280"), const_cast<char*>("720"), nullptr};
        posix_spawn_file_actions_t actions;
        int action_result = posix_spawn_file_actions_init(&actions);
        bool const actions_initialized = action_result == 0;
        if (!action_result) action_result = posix_spawn_file_actions_adddup2(&actions, spawn_card, 3);
        if (!action_result) action_result = posix_spawn_file_actions_adddup2(&actions, spawn_status, 4);
        if (!action_result) action_result = posix_spawn_file_actions_addclose(&actions, spawn_card);
        if (!action_result) action_result = posix_spawn_file_actions_addclose(&actions, spawn_status);
        if (!action_result && status_pipe[0] != 3 && status_pipe[0] != 4)
            action_result = posix_spawn_file_actions_addclose(&actions, status_pipe[0]);
        if (!action_result && status_pipe[1] != 3 && status_pipe[1] != 4)
            action_result = posix_spawn_file_actions_addclose(&actions, status_pipe[1]);
        stop_requested = false;
        forced_kill_requested = false;
        int const spawn_result = action_result ? action_result :
            posix_spawn(&child_pid, XDISP_MIRGUD_PATH, &actions, nullptr, arguments, environ);
        if (actions_initialized)
            posix_spawn_file_actions_destroy(&actions);
        close(spawn_card);
        close(spawn_status);
        close(status_pipe[1]);
        if (spawn_result)
        {
            close(status_pipe[0]);
            child_pid = 0;
            throw std::system_error{spawn_result, std::system_category(), "cannot spawn mirgud"};
        }
        status_channel = g_io_channel_unix_new(status_pipe[0]);
        g_io_channel_set_close_on_unref(status_channel, TRUE);
        g_io_channel_set_encoding(status_channel, nullptr, nullptr);
        status_watch = g_io_add_watch(status_channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
            &Daemon::child_status, this);
        child_watch = g_child_watch_add(child_pid, &Daemon::child_exit, this);
        activation_source = g_timeout_add_seconds(activation_timeout_seconds, [](gpointer data) -> gboolean {
            auto& self = *static_cast<Daemon*>(data);
            self.activation_source = 0;
            self.lifecycle.connecting_timed_out();
            return FALSE;
        }, this);
        emit_properties();
    }

    void stop_child()
    {
        if (child_pid <= 0)
            return;
        stop_requested = true;
        if (!stop_started)
        {
            stop_started = true;
            stop_started_at = std::chrono::steady_clock::now();
            g_message("xdispd: teardown stop requested");
        }
        kill(child_pid, SIGTERM);
        if (!kill_source)
            kill_source = g_timeout_add_seconds(3, [](gpointer data) -> gboolean {
                auto& self = *static_cast<Daemon*>(data);
                self.kill_source = 0;
                if (self.child_pid > 0)
                {
                    self.forced_kill_requested = true;
                    g_warning("xdispd: teardown deadline reached; sending SIGKILL");
                    kill(self.child_pid, SIGKILL);
                }
                return FALSE;
            }, this);
    }

    static gboolean child_status(GIOChannel* channel, GIOCondition condition, gpointer data)
    {
        auto& self = *static_cast<Daemon*>(data);
        while (condition & G_IO_IN)
        {
            gchar* line{};
            gsize length{};
            GIOStatus const status = g_io_channel_read_line(channel, &line, &length, nullptr, nullptr);
            if (status != G_IO_STATUS_NORMAL)
                break;
            std::string const message{line, length};
            g_free(line);
            g_message("xdispd: child milestone: %s", message.substr(0, message.find_last_not_of("\r\n") + 1).c_str());
            if (message.find(" ACTIVE\n") != std::string::npos)
            {
                if (self.activation_source) { g_source_remove(self.activation_source); self.activation_source = 0; }
                self.lifecycle.child_active();
            }
            else if (message.find(" FATAL P\n") != std::string::npos)
            {
                self.poisoned_identity = self.candidate.identity;
                self.lifecycle.poison("mirgud reported an unsafe GUD transport failure");
            }
        }
        if (condition & (G_IO_HUP | G_IO_ERR))
        {
            self.status_watch = 0;
            return FALSE;
        }
        return TRUE;
    }

    static void child_exit(GPid pid, gint status, gpointer data)
    {
        auto& self = *static_cast<Daemon*>(data);
        g_spawn_close_pid(pid);
        if (self.kill_source) { g_source_remove(self.kill_source); self.kill_source = 0; }
        if (self.activation_source) { g_source_remove(self.activation_source); self.activation_source = 0; }
        self.child_pid = 0;
        self.child_watch = 0;
        if (self.status_watch) { g_source_remove(self.status_watch); self.status_watch = 0; }
        if (self.status_channel) { g_io_channel_unref(self.status_channel); self.status_channel = nullptr; }
        auto const termination = xdisp::classify_child_termination(
            status, self.stop_requested, self.forced_kill_requested);
        self.stop_diagnostics.child_exited(termination);
        uint64_t stop_duration_ms{};
        if (self.stop_started)
        {
            stop_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - self.stop_started_at).count();
            self.stop_diagnostics.stop_completed(termination, stop_duration_ms);
        }
        g_message("xdispd: child exit %s stop_duration_ms=%llu", termination.description.c_str(),
            static_cast<unsigned long long>(stop_duration_ms));
        self.stop_requested = false;
        self.forced_kill_requested = false;
        self.stop_started = false;
        if (termination.result == xdisp::ChildResult::poisoned_transport)
            self.poisoned_identity = self.candidate.identity;
        self.lifecycle.child_exited(termination.result, termination.description);
        self.emit_properties();
        if (self.shutting_down && self.loop)
            g_main_loop_quit(self.loop);
    }

    Udev context;
    Monitor monitor{nullptr, udev_monitor_unref};
    Candidate candidate;
    xdisp::Lifecycle lifecycle;
    std::string last_error;
    std::string last_lifecycle_event{"startup"};
    std::string poisoned_identity;
    xdisp::StopDiagnostics stop_diagnostics;
    GMainLoop* loop{};
    GDBusConnection* connection{};
    GDBusNodeInfo* node{};
    guint owner{};
    guint registration{};
    guint monitor_watch{};
    GIOChannel* monitor_channel{};
    pid_t child_pid{};
    guint child_watch{};
    guint kill_source{};
    guint status_watch{};
    guint activation_source{};
    guint activation_timeout_seconds{diagnostic_activation_timeout_seconds()};
    GIOChannel* status_channel{};
    bool shutting_down{};
    bool stop_requested{};
    bool forced_kill_requested{};
    bool stop_started{};
    std::chrono::steady_clock::time_point stop_started_at{};
    bool poison_remove_observed{};
};
}

int main()
try
{
    Daemon daemon;
    g_unix_signal_add(SIGTERM, [](gpointer data) -> gboolean {
        static_cast<Daemon*>(data)->shutdown();
        return FALSE;
    }, &daemon);
    g_unix_signal_add(SIGINT, [](gpointer data) -> gboolean {
        static_cast<Daemon*>(data)->shutdown();
        return FALSE;
    }, &daemon);
    daemon.run();
    return 0;
}
catch (std::exception const& error)
{
    g_critical("xdispd: %s", error.what());
    return 1;
}
