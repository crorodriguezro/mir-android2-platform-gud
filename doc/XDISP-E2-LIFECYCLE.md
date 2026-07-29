# XDISP-E2 native external-display lifecycle

## E2.0 Settings and Aethercast audit

**Classification: E2.0-C - NOT SUITABLE.** The installed `External display`
switch controls Aethercast's Miracast/Wi-Fi Direct subsystem. It does not
create or own a Mir extended screencast when enabled, and its device model
cannot represent the USB GUD adapter without falsely presenting it as a
wireless display.

The smallest E2.1 architecture justified by this result is **Option C: a
dedicated `xdisp` service with separate native control**. Aethercast must remain
the owner of Miracast connections. The `xdisp` service must be the only owner of
GUD detection, `mirgud`, its Virtual-output lifetime, reconnect policy, and GUD
status.

### Installed versions and source match

The phone was audited on 2026-07-29 with no `mirgud` process running.

| Component | Installed version | Matching source |
| --- | --- | --- |
| Lomiri System Settings | `1.3.2+0~20260611100346.5+ubports24.04.1~1.gbp5ca979` | `ubports/core/lomiri-system-settings`, commit `5ca9798dcc4ba8bc9e34e372311143d844026b2e` |
| Aethercast | `0.4+0~20250902144210.2+ubports24.04.1~1.gbpd9ec3a` | `ubports/development/core/aethercast`, commit `d9ec3a885750e3f1d18c0fb5fca748ba7a1f77aa` |

The relevant installed Settings files are:

- `/usr/share/lomiri-system-settings/qml-plugins/brightness/PageComponent.qml`
- `/usr/share/lomiri-system-settings/qml-plugins/brightness/WifiDisplays.qml`
- `/usr/share/lomiri-system-settings/qml-plugins/brightness/WifiDisplaysAlert.qml`
- `/usr/lib/aarch64-linux-gnu/lomiri-system-settings/libbrightness-plugin.so`

Their matching backend source is under
`plugins/brightness/aethercast/`, principally `displays.cpp`, `device.cpp`,
`devicemodel.cpp`, and `org.aethercast.xml`. `brightness.cpp` exposes the UI
only when Android property `ubuntu.widi.supported` is nonzero; the audited
phone reports `1`.

The page labels the switch `External display`, but the only subordinate action
is explicitly `Wireless display`. The switch writes `AethercastDisplays.enabled`.
The wireless page calls `AethercastDisplays.scan()`, lists only Aethercast
devices, and calls `Connect` or `Disconnect` on the selected device object.

### D-Bus and service contract

Aethercast uses the system bus:

| Item | Value |
| --- | --- |
| Bus name | `org.aethercast` |
| Manager path | `/org/aethercast` |
| Manager interface | `org.aethercast.Manager` |
| Device paths | `/org/aethercast/dev_<Wi-Fi-peer-MAC>` |
| Device interface | `org.aethercast.Device` |
| Manager methods | `RegisterMediaManager`, `UnregisterMediaManager`, `Scan`, legacy `DisconnectAll` |
| Manager properties | `Enabled`, `State`, `Capabilities`, `Scanning` |
| Device methods | `Connect(role)`, `Disconnect` |
| Device properties | `State`, `Address`, `Name`, `Capabilities` |
| Media-manager methods | `Configure(address, port)`, `Play`, `Pause`, `Teardown` |

`aethercast.service` is a system `Type=dbus` service with
`BusName=org.aethercast`, `ExecStart=/usr/sbin/aethercast -d`, and
`Restart=on-failure`. D-Bus activation is declared by
`/usr/share/dbus-1/system-services/org.aethercast.service`. A legacy Upstart
job remains installed at `/etc/init/aethercast.conf`, but systemd owned the
observed process.

The system-bus policy permits root and the active console user to call
`org.aethercast` and denies other callers. The service activation file assigns
the `unconfined` AppArmor label. There is no Aethercast application profile;
the installed AppArmor fragment only permits `dhcpd` to read Aethercast's
Wi-Fi Direct DHCP configuration. The observed Settings and Aethercast
processes were unconfined.

The setting is not a GSettings key. Aethercast persists it as the presence or
absence of `/var/lib/aethercast/enabled`.

### Installed implementation trace

The installed source has three separate lifecycle stages:

1. Setting manager property `Enabled=true` initializes the Wi-Fi Direct
   network manager and creates `/var/lib/aethercast/enabled`. It does not call
   Mir.
2. `Scan` starts P2P discovery. Discovered peers with Wireless Display
   Information Elements become `org.aethercast.Device` objects whose paths are
   derived from their MAC addresses.
3. `Device.Connect` performs Wi-Fi Direct association and Miracast RTSP
   negotiation. Only after the sink supplies an H.264 video format does
   `SourceMediaManager::Configure()` create a `Mode::kExtend` producer and call
   `Screencast::Setup()` against `/run/mir_socket`. That retained screencast
   owns the Mir Virtual output. Teardown destroys the producer and disconnects
   the Virtual output.

The current installed Aethercast source requests the negotiated width and
height at `{primary_width, 0}` and uses the known vertical-mirror semantics.
This is the source lifecycle reproduced by E0/E1, but it is reached only as
part of a negotiated Miracast session.

### Runtime trace

Baseline before the UI cycle:

- `org.aethercast` was activatable but had no owner.
- `aethercast.service` was inactive and no Aethercast process existed.
- system Mir reported `Virtual, disconnected` with its retained `1280x720`
  mode.
- no `mirgud` process was running.

The observed Settings cycle was: open `Brightness & Display`, switch
`External display` off then on, open `Wireless display`, wait for discovery,
return, and close Settings.

Results:

- Settings activated `org.aethercast` through systemd.
- Aethercast initialized `p2p-dev-wlan0`, entered Miracast source mode, and
  started P2P discovery.
- Seven wireless-display peers were exported under MAC-derived
  `/org/aethercast/dev_*` paths. A non-WFD peer was explicitly ignored because
  it had no Wireless Display Information Elements.
- Opening the wireless page issued `Scan`; Aethercast logged the call and a
  30-second scan timeout.
- The manager remained `Enabled=true`, `State="disconnected"`; the enabled
  marker remained present.
- No sink was selected, no RTSP/media negotiation occurred, and system Mir's
  `Virtual` output remained disconnected.
- Closing the page did not turn the persistent setting off. After scan expiry
  and peer removal, Aethercast's 60-second idle path exited the service
  cleanly. A later audit introspection call reactivated it; that activation was
  caused by the audit, not by Settings teardown.

An unprivileged `dbus-monitor` could not acquire the system-bus monitoring
role and its fallback did not capture the point-to-point calls. The exact calls
above are established by the matching installed Settings source and confirmed
at the service boundary by activation, Aethercast's `OnHandleScan` log, manager
properties, exported peer objects, persisted marker, and unchanged Mir output.

### E2.0 questions answered

1. The option enables Aethercast's wireless-display network manager and
   discovery capability. It is not an extended-output enable switch.
2. It does not directly create or own a Mir screencast. Aethercast's negotiated
   media producer owns that lifecycle only after a Miracast sink connects.
3. Yes. Aethercast creates the Mir Virtual output only after peer association,
   RTSP negotiation, and sink video-format selection reach media configuration.
4. Settings owns user intent and displays Aethercast's status/errors;
   Aethercast owns wireless connect, disconnect, failure, and media teardown;
   its retained Mir screencast owns the Virtual output. Lomiri changes the
   internal display to `DisabledScreenNotice`/`VirtualTouchPad` based on display
   configuration. The touchpad's close button sets Aethercast `Enabled=false`.
5. No. The public device model requires a MAC-addressed Wi-Fi Direct peer,
   WFD capabilities, association states, and Miracast connect/disconnect.
6. Reusing the existing model would require an invasive Aethercast provider
   abstraction or a misleading fake peer. A small adapter behind the existing
   surface cannot cleanly bypass those semantics. The justified choice is an
   independent `xdisp` service.
7. The existing UI cannot be reused unchanged: it labels discovery as
   `Wireless display`, lists nearby Miracast devices, depends on Wi-Fi state,
   and its touchpad disconnect action disables Aethercast globally. Presenting
   a USB GUD sink there would be misleading.

## E2.1 lifecycle architecture

**Selected architecture: Option C - dedicated `xdisp` service with separate
native control.** The source/component implementation is complete. Hardware
activation and lifecycle classification are pending a verified safe receiver
state and a current GUD DRM card.

The service will be the sole authoritative owner of:

- dynamic GUD USB and DRM discovery;
- starting and stopping the proven `mirgud --source-mode extend` command;
- the retained Mir Virtual output and `LatestFramePresenter` worker;
- active GUD card/connector identity and stale-node rejection;
- contained detach, safe reconnect, transport poison, and UI-visible state.

Aethercast and its Settings switch will not start, stop, or report GUD. This
avoids two components independently owning an extended screencast and preserves
truthful wireless-display UX.

### Required state model

| State | Meaning |
| --- | --- |
| `unavailable` | Enabled, but no usable GUD USB/DRM sink is present. |
| `available` | A current GUD card and connected connector are discoverable. |
| `connecting` | The owner is starting `mirgud` and waiting for active presentation. |
| `active` | One owned `mirgud` process is presenting the retained Virtual output. |
| `disconnecting` | The owner has requested orderly source and presenter teardown. |
| `recoverable_error` | A contained non-poisoning failure stopped presentation; rediscovery or explicit activation may retry. |
| `poisoned_transport` | A receiver poison or equivalent transport stop was reported; no automatic retry is permitted. |
| `disabled` | User intent forbids discovery activation and presentation. |

Allowed transitions are:

```text
disabled -> unavailable
unavailable -> available | disabled
available -> connecting | unavailable | disabled
connecting -> active | recoverable_error | unavailable | poisoned_transport | disconnecting
active -> disconnecting | recoverable_error | unavailable | poisoned_transport
disconnecting -> available | unavailable | recoverable_error | poisoned_transport | disabled
recoverable_error -> connecting | available | unavailable | poisoned_transport | disabled
poisoned_transport -> disabled
disabled -> poisoned_transport  (restored persisted poison state)
```

Physical receiver recovery and an explicit administrative poison-clear action
are required before `poisoned_transport` may return to `disabled`. Automatic
restart, reconnect, Pi service manipulation, and retry are forbidden in that
state.

### Implemented owner

`xdispd` is a system-bus daemon owning `org.lomiri.XDisp` at
`/org/lomiri/XDisp` with interface `org.lomiri.XDisp1`. It exposes:

- `Enable`, `Disable`, `Activate`, and `Deactivate` for normal lifecycle
  control;
- root-only `Poison(reason)` and `ClearPoison` operations;
- state, intent, dynamic GUD device/identity/connector, child PID, last error,
  and recovery-observed properties;
- standard property changes and a `StateChanged` signal.

The daemon uses libudev to enumerate every primary DRM card rather than a
fixed card range. It verifies driver `gud`, a connected exact `1280x720` mode,
the DRM device number, sysfs identity, and connector before activation.
Multiple usable GUD cards are rejected as ambiguous. Udev add/remove/change
events and a passive reconciliation timer replace stale card numbering.

Only `xdispd` starts the private managed `mirgud` child. It passes a separately
opened and revalidated DRM fd plus connector id, requires an `XDISP1 ACTIVE`
record after the first live frame commit, and applies a 10-second activation
deadline. Normal stop sends `SIGTERM`, then `SIGKILL` after three seconds if
the synchronous Mir path does not return. The child reports unavailable,
recoverable, and unsafe transport exits separately; unsafe timeout/protocol/I/O
errors durably enter `poisoned_transport` before stopping the child.

The poison marker stores the affected sysfs identity. Recovery requires an
observed removal of that identity followed by a usable add/change of the same
identity; merely restarting the daemon, adding another DRM card, or calling
`Activate` cannot clear poison. `ClearPoison` returns only to `disabled` and
never activates automatically.

The dormant platform POC no longer scans GUD or exposes a synthetic output.
Normal Android primary/external compositor policy remains enabled. This leaves
the dedicated daemon as the sole GUD lifecycle owner without changing the E1
Virtual-output architecture.

### Build and component result

The phone-matched Noble/AArch64 build produced `xdispd`, private managed
`mirgud`, and the Android2 platform module. Ten lifecycle tests pass, covering
single-child activation, orderly disconnect, retained hotplug intent,
explicit-only recoverable retry, persist-before-stop poison handling,
physical-recovery gating, disable semantics, start failure, activation
deadline, and restart with persisted poison. Twelve focused Android/GUD tests
also pass, including the preserved RGB565 conversion/row-order and bounded
presenter behavior plus synthetic-owner disable policy.

The complete install image places:

- `xdispd` and private `mirgud` under `/usr/libexec/lomiri-xdisp/`;
- `xdisp.service` under `/usr/lib/systemd/system/`;
- D-Bus activation and policy files under their standard system locations;
- no public or unowned xdisp test binary.

Final artifacts were staged, but not installed or run, as:

| Artifact | SHA-256 |
| --- | --- |
| `/home/phablet/xdispd-e21-a1d0882.bin` | `a1d08829eddb9d41488d22ece98597a2d8e5b73535478867f55989fc0e258135` |
| `/home/phablet/mirgud-e21-bbe5bb7.bin` | `bbe5bb7d8addfe1d3b825f494e82b6ee684eabf2f660fa968cc59c2f8f62c73b` |

### Hardware gate (2026-07-29)

The mandatory phone-side enumeration scan found the receiver dynamically at
`/sys/bus/usb/devices/1-1.2` with `1d50:614d`. The `gud` module name was loaded,
but only MSM `/dev/dri/card0` existed; there was no current GUD DRM card. The
phone contained no running `xdispd` or `mirgud`, and system Mir remained at
`Virtual, disconnected`.

Direct Pi SSH safety verification was unavailable with the locally available
credentials. Because receiver state could not be confirmed and no GUD card
existed, no module unload/load, Pi service action, daemon installation,
`Enable`, `Activate`, modeset, or USB payload submission was attempted. This
is a pre-activation hardware block, not an E2.1 lifecycle failure.

## E2.2 reconnect matrix

Pending implementation and fresh-safe-receiver hardware runs.

## E2.3 Lomiri UX

E1 directly established an upright, correctly proportioned and colored usable
HDMI image and confirmed that the phone operated as Lomiri's Virtual Touchpad.
The complete E2 interaction matrix remains pending.

## E2.4 reproducible capped transport

The exact capped-driver source is tracked in the sibling `gud` repository at
`backport-4.9/variants/xdisp-lz4-12800/`, with build, unit, sanitizer, contract,
metadata, and explicit staging tooling. E2.4 verification and supported-profile
classification remain pending until E2.1-E2.3 results are recorded.

## E2.5 stability

The required unlocked 30-minute interactive run has not started.

## Final classification

No final E2 classification is assigned before E2.1-E2.5 complete.
