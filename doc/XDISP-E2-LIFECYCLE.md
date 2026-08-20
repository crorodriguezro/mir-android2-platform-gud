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

## Architecture decision

The current v1 production path is:

Lomiri -> Mir Virtual extended output / screencast -> `mirgud` -> `LatestFramePresenter` -> GUD DRM/KMS -> USB -> Pi Zero 2 W -> HDMI monitor

`xdispd` owns GUD detection, activation intent, lifecycle, child-process
management, and recovery. It starts the managed `mirgud` client and tracks the
owned Virtual output.

The older Android2 synthetic/offscreen GUD path in
`src/platforms/android/server/gud_output.cpp`,
`gud_offscreen_target.h`, and `gud_synthetic_output_control.h` is dormant for
v1. It remains in source as a historical POC and test scaffolding, but it is
not the production path unless a future decision explicitly reactivates it.

Under this architecture, E2-T01 means reconciling and proving the
source/render/capture gate for the `xdispd`/`mirgud` path, not reviving the old
Android2 synthetic target.

### E2-T03 producer/worker boundary

The capture/main thread creates a client-owned frame and calls
`LatestFramePresenter::submit()`. That method allocates the replacement frame,
holds its mutex only long enough to replace the single pending slot, then wakes
the worker. It does not perform GUD, KMS, or USB I/O.

The worker moves the pending frame out of that slot and releases the mutex
before calling `present(frame)`. Consequently, one frame can be in flight and
one newer frame can be pending; newer pending frames replace older pending
frames. GUD/KMS/USB waits are confined to the worker.

`enqueue_us` measures the producer-visible `submit()` call. The historical
`submit_us` metric remains for parser compatibility and measures the
worker-owned `present(frame)` duration. It is not evidence of producer
blocking.

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
- raw child and requested-stop diagnostics: string `LastChildExit`, boolean
  `LastStopForced`, unsigned 64-bit `ForcedStopCount`, and unsigned 64-bit
  monotonic `LastStopDurationMs`;
- standard property changes and a `StateChanged` signal.

`LastChildExit` records normal exit, requested graceful exit, confirmed daemon
`SIGKILL`, unexpected signal, source failure, or poisoned transport without
collapsing them into one stopped result. `LastStopForced` and
`LastStopDurationMs` describe the latest completed requested stop and are not
erased by a later spontaneous child failure. `ForcedStopCount` increments only
when the reaped child confirms the daemon's escalation ended in `SIGKILL`.

The daemon uses libudev to enumerate every primary DRM card rather than a
fixed card range. It verifies driver `gud`, a connected exact `1280x720` mode,
the DRM device number, sysfs identity, and connector before activation.
Multiple usable GUD cards are rejected as ambiguous. Udev add/remove/change
events and a passive reconciliation timer replace stale card numbering.

Only `xdispd` starts the private managed `mirgud` child. It passes a separately
opened and revalidated DRM fd plus connector id, requires an `XDISP1 <monotonic-ms> ACTIVE`
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

Ubuntu Touch mounts the system partition read-only, so `/var/lib` is not a
writable service-state location on this phone. The service creates root-owned
mode-`0700` state under the persistent writable user-data partition at
`/home/phablet/.local/share/lomiri-xdisp/`. The enabled and poison markers live
there; binaries, systemd units, and D-Bus policy remain on the read-only system
partition.

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
| `/home/phablet/xdispd-e22-c35df4d.bin` | `c35df4d8d12fd456b4602d8e9e6114a0b5d0da31e92e2031546e474f268a9bde` |
| `/home/phablet/mirgud-e22-managed.bin` | `5f74416709518fd787672f7e67c42da053eac6ba71b3d21e62667acef04da4ed` |

### Hardware gate (2026-07-29)

#### Pi SSH authentication

The receiver uses SSH password authentication with user `cristian`. The
password is intentionally not tracked in this repository. On this development
host it is stored in `~/.config/linux-mobile-xdisp/pi.env`, outside all three
project repositories, with file mode `0600`. That local file defines
`PI_USER`, `PI_PASSWORD`, `PHONE_USER`, `PHONE_HOST`, and
`PHONE_SUDO_PASSWORD` and is the durable credential source for future hardware
sessions. Future agents must load this file rather than asking again for the
Pi password or phone sudo credential. The credential values must not be copied
into tracked files, command lines, or evidence logs.

The Pi address is dynamic and must be established from the current network or
known receiver address; do not treat an old address as device identity. This
host does not have `sshpass`, so noninteractive automation loads the local env
file and uses Python `pexpect` to invoke normal `ssh` or `scp`, accept a new
host key only when explicitly expected, and answer the password prompt. Never
place the password on a shell command line, in a tracked script, in evidence
logs, or in this document. Interactive access remains ordinary:

```bash
ssh cristian@<current-pi-address>
```

Before every payload-producing test, use that access only for read-only
preflight: verify `gud-userspace.service`, its PID/restart/result properties,
FunctionFS mount/endpoints, UDC state, and absence of `Poisoned`, short, or
invalid receive records. A poisoned receiver remains a terminal stop and must
not be restarted through SSH.

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

### Native activation gate (2026-07-29)

After a physical USB data replug, the phone enumerated a fresh high-speed GUD
generation at `1-1.2:1.0`. The capped `gud_xdisp_lz4_12800` driver created
dynamic `/dev/dri/card1`; `xdispd` discovered its full sysfs identity and
connector 25 without using fixed card numbering. The Pi receiver started a
fresh PID 944 with zero counted restarts, FunctionFS mounted, its bulk endpoint
open, UDC `configured`, and no poisoned, short, invalid, or in-flight receive.

The service was installed and started in `disabled` state. It reported card1
and connector 25 with child PID zero while system Mir kept `Virtual`
disconnected. `Enable` then produced `available` with no child or output,
proving that enable intent is distinct from activation.

The first activation attempt was safely contained before pixel transport:
`mirgud` selected the exact mode but its initial atomic commit returned
`EACCES`. The daemon entered `recoverable_error`, reaped the child, and left
Virtual disconnected. Root cause was the daemon retaining DRM master through
its discovery fd. Discovery now stores only verified identity metadata, drops
master, and closes the probe fd before activation.

The corrected activation passed:

- one managed child, PID 46112;
- `available -> connecting -> active` after the first committed live frame;
- Virtual connected/used at `1280x720+1080+0`;
- top-down CPU-owned RGB565 frames and the existing latest-frame presenter;
- flat 17 child FDs in sampled reports;
- zero conversion and GUD-submission failures;
- actual capped frame payload maximum observed at 12,793 bytes;
- every sampled Pi payload completed in one FunctionFS read and returned to
  Idle, with no poison, short read, or service restart.

Native `Deactivate` returned the daemon to `available`, child PID zero, and
Virtual disconnected in 2,956 ms; both compositors remained alive. This hit
the daemon's bounded three-second forced-stop fallback and therefore proves
contained non-blocking teardown, but not a graceful presenter-worker join.
That distinction remains open for E2.2 even though phone and receiver state
were clean after teardown.

### Active detach and recovery

The USB data path was physically unplugged while the native desktop was
active. The last normal stream sample had 7,216 received frames, 4,189
presented, 3,025 replaced/dropped, zero conversion failures, zero GUD submit
failures, and 17 child FDs. At detach the host received protocol `-71`, not
`-110`; the presenter reported one contained atomic failure as the DRM device
was removed. `xdispd` moved `active -> disconnecting -> unavailable`, reaped
the child, retained activation intent, removed card1/connector identity, and
disconnected Virtual. Lomiri remained usable.

The same physical action accidentally removed Pi power as well as USB data,
so it is not a valid same-boot Pi replug case. The fresh Pi boot's first
receiver start failed before UDC bind or host session because vc4 `set_crtc`
returned `EACCES` during early boot. With no gadget, endpoint owner, payload,
or poisoned state, one normal `systemctl start gud-userspace.service` after
boot was the smallest safe recovery. This is recorded as receiver boot timing,
not an xdisp reconnect pass or failure.

After that fresh receiver became active, the phone enumerated a new high-speed
GUD generation and recreated card1. Retained xdisp activation intent then
performed `unavailable -> available -> connecting -> active` automatically,
with a new managed child and Virtual restored at `1280x720+1080+0`. No LightDM,
phone service, or session restart was required.

### Ten-cycle matrix

Ten consecutive native `Activate -> active -> Deactivate -> available` cycles
passed on the fresh receiver instance:

- every cycle dynamically resolved USB `1-1.2`, `/dev/dri/card1`, and connector
  25;
- every cycle had exactly one unique managed child and no residual child;
- every teardown disconnected Virtual and retained both compositors;
- teardown times ranged from 2,807 to 4,072 ms;
- zero phone conversion or GUD-submission failures were recorded;
- no new GUD `-110`, kernel `BUG`, or `Oops` appeared;
- the phone's maximum actual payload was 12,799 bytes;
- the Pi recorded 3,206 complete payloads with maximum 12,798 bytes, zero
  poison/short/invalid/receive-error match, UDC configured, PID 978 active,
  and zero service restarts.

The matrix proves repeatable bounded containment and activation ownership. The
2.8-4.1 second teardown distribution confirms the known synchronous Mir
release often reaches the forced-stop deadline, so graceful join remains an
explicit lifecycle limitation rather than being claimed as passed.

### Teardown boundary follow-up (2026-07-29)

A controlled one-cycle probe added monotonic managed-child milestones around
the existing shutdown path. It observed `TERM_OBSERVED`, `CAPTURE_LOOP_EXIT`,
`PRESENTER_STOP_COMPLETE`, and `SCREENCAST_RELEASE_COMPLETE` within 155 ms of
the request. The final `MIR_CONNECTION_RELEASE_BEGIN` was then followed by no
completion before xdispd's 3-second containment deadline; the daemon recorded
`forced-sigkill:signal:9` after 3,459 ms. This excludes the capture loop,
in-flight GUD presenter, KMS teardown, and synchronous screencast release as
the routine blocking boundary.

The matching Mir 1.8 client implementation makes `mir_connection_release()`
call `MirConnection::disconnect()` and wait without a timeout for the server
Disconnect reply. A signalfd-based probe that prevented SIGTERM from
interrupting any Mir client thread reproduced the same wait, so asynchronous
signal delivery is not the cause. That probe was not retained: its test binary
was replaced with the verified prior artifact, xdisp was restarted to
`available`, and the Ubuntu Touch system partition was remounted read-only.

No shutdown workaround was committed. In particular, xdisp must not turn an
unanswered Mir Disconnect request into an untracked process exit or silently
skip connection ownership release. The remaining graceful-stop fix belongs at
the installed Mir client/server Disconnect boundary (or needs a supported,
bounded client API there), rather than in GUD transport or the frame
presenter.

## E2.3 Lomiri UX

**Core UX result: pass.** During native xdisp activation, direct observation
confirmed Virtual Touchpad mode, usable pointer motion, tap/click, two-finger
scrolling, launcher operation, application switching, and sensible external
window/dialog placement. The HDMI desktop remained upright, correctly colored
and proportioned. Normal `Deactivate` and active USB detach both disconnected
Virtual and returned the phone from touchpad mode without restarting Lomiri.

Lock/unlock, rotation changes, and application-specific placement were not
exhaustively tested. The existing System Settings `External display` switch
continues to report Aethercast/Miracast state by design and does not represent
GUD; E2.0 established that reusing it would be misleading.

## E2.4 reproducible capped transport

**Supported OnePlus profile: qualified v1 at source commit `2a8f59b`.** The
normal module remains a separate artifact and is not silently overwritten.
The current v2 source remains test-only: it contains experimental bounded and
predictive policies and the safety staging helper intentionally rejects it.

The qualified source and build identity are:

| Item | Value |
| --- | --- |
| Repository | `https://github.com/crorodriguezro/gud.git` |
| Remote branch | `xdisp-oneplus-12800` |
| Immutable annotated tag | `xdisp-oneplus-12800-v1` |
| Source commit | `2a8f59ba84120f6b6f821b83a02c4d6a9459b32a` |
| Source tree | `793dadf199bbb080c02a3807cfca37ad50a4527f` |
| Commit subject | `gud: add adaptive LZ4 XDISP diagnostic` |
| Kernel repository | `kernel-oneplus-sdm845.git` |
| Kernel commit | `6b190d86bd895acf891617e26840b1a85df6602c` |
| Kernel release | `4.9.112-g6b190d86b` |
| Architecture | `arm64` |
| Compiler | `aarch64-redhat-linux-gcc 16.1.1` |
| Config SHA-256 | `48ee04061346d4b3c5d27fd0874ae285c80c9851807cffac83abfac83680af12` |
| Module.symvers SHA-256 | `a677333e4c2304f627896533e356cc78886bb74e56f54728fe0849cd1b09a95c` |
| Module version | `xdisp-p0.1-adaptive-12800-v1` |
| Module srcversion | `A6C725860B997A5E68AF374` |
| ELF build ID | `a394e386bb6200ce34b9a2f46111767cdb726c73` |
| Artifact SHA-256 | `2369eccc5cf3afc7ff8364921b8ce94fec8363518466f727b71d2d009c6c5999` |
| Phone artifact | `/home/phablet/gud.xdisp-p0.1-adaptive-12800-2a8f59b.ko` |
| Normal module SHA-256 | `bd15c2c1bc4cd941bcac88bb13276b67620d9e2eec515973ff815add68f3630c` |

Clone the immutable tag and prepare its ignored local manifest and kernel tree:

```bash
git clone --branch xdisp-oneplus-12800-v1 --depth 1 \
  https://github.com/crorodriguezro/gud.git gud-xdisp-oneplus-v1
cd gud-xdisp-oneplus-v1/backport-4.9
cp env/target-manifest.env.example env/target-manifest.env
```

Set the manifest to the public kernel URL
`https://gitlab.com/ubports/community-ports/android9/oneplus-6/kernel-oneplus-sdm845.git`,
kernel commit `6b190d86bd895acf891617e26840b1a85df6602c`, the values in the
table above, the available AArch64 GCC 16.1.1 prefix, and an isolated kernel
build directory. Then prepare and build both distinct modules:

```bash
./env/prepare-kernel.sh
make MANIFEST="$PWD/env/target-manifest.env" clean modules
make MANIFEST="$PWD/env/target-manifest.env" \
  xdisp-lz4-12800-clean xdisp-lz4-12800
sha256sum variants/xdisp-lz4-12800/gud.ko
modinfo variants/xdisp-lz4-12800/gud.ko
readelf -n variants/xdisp-lz4-12800/gud.ko
```

The rebuild completed against the recorded kernel tree and reproduced the
qualified artifact bit-for-bit: SHA-256, srcversion, vermagic, and ELF build ID
all match the retained v1 module.

Publication was independently verified after pushing: the remote branch and
the peeled annotated tag both resolve to full source commit
`2a8f59ba84120f6b6f821b83a02c4d6a9459b32a`. No workstation-only path or
credential is needed to clone the source or kernel; generated captures,
prepared kernel output, artifacts, and evidence remain intentionally ignored.

The exact-commit normal and sanitizer suites, contract suite, and staging-tool
suite all pass. They cover exact 12,800, adjacent 12,799/12,801 limits,
one-row-over rejection, complete-row source lengths and full-frame coverage,
640-pixel narrow and 1920-pixel wide rectangles, compressed round trips, raw
fallback/backoff, red zones, and the adjacent pre-submit hard guard. Current
v2 planner, sanitizer, and contract tests also pass, while its v1-only staging
test rejects the v2 module identity as intended.

The real submission path validates `chunk.payload_length <= 12800` immediately
before constructing `SET_BUFFER` and the bulk URB. It sends compressed length
only when compression is selected, otherwise source length, and updates the
maximum-payload diagnostic only after a complete transfer. The default 3,000
ms host bulk timeout and established retry behavior are unchanged.

Staging uses an explicit commit-qualified path and verifies both local/remote
diagnostic hashes plus the unchanged normal-module hash without loading either
artifact:

```bash
REMOTE_MODULE_PATH=/home/phablet/gud.xdisp-p0.1-adaptive-12800-2a8f59b.ko \
  backport-4.9/env/stage-xdisp-module.sh
```

Activation is explicit: unload the internal module name `gud` only from a
fresh safe receiver state, then `insmod` the qualified path. Never copy it over
`/home/phablet/gud.ko`; both artifacts intentionally have internal module name
`gud` and cannot coexist.

Hardware qualification includes the retained v1 10/10 adaptive matrix with
maximum payload 12,799 and no `-110`, short/impossible/poisoned receive, DWC2
fault, or reboot. This E2 run independently observed phone maximum 12,799 and
Pi maximum 12,798 across 3,206 complete matrix payloads. The qualified capped
variant therefore remains a OnePlus-specific supported profile rather than
replacing the normal GUD project configuration globally.

## E2.5 stability

**Result: failed at 88 seconds due to LightDM/system-compositor restart during
USB HID attachment.** The required 30-minute run did not complete and must not
be counted as stable.

The unlocked interactive run began at 17:23:20 with native xdisp active. The
last complete child report at 17:24:47 recorded 2,283 frames received, 564
presented, 1,717 replaced/dropped, zero conversion failures, zero GUD submit
failures, and 17 FDs. Temperature sampled at 35.5 C initially and 32.5 C near
one minute. The phone's maximum capped payload remained 12,799 bytes.

At 17:24:45 a USB2 hub added a Razer Viper mouse plus two keyboard interfaces
and a HOLDCHIP USB Gaming Keyboard plus two keyboard interfaces. Lomiri logged
the pointer count change while already in Windowed 1280x720 external mode. At
17:24:48 it applied the new keymap; immediately afterward the LightDM main
process exited status 1. LightDM restarted twice, replacing system Mir. The
managed `mirgud` consequently lost `/run/mir_socket`, exited status 21, and
`xdispd` entered `recoverable_error` with no child and Virtual disconnected.

There was no GUD transport stop condition during this interval. The Pi
recorded 1,926 frame payloads, maximum 12,798 bytes, at least 1,928 returns to
Idle, no short read, poison, invalid receive, receiver error, or service
restart. The phone recorded no GUD `-110`, conversion failure, or submission
failure. This separates the failure from Raw GUD transport and attributes the
test stop to system-compositor/session behavior concurrent with USB HID
attachment.

After the automatic LightDM recovery, the phone returned to a normal
single-display greeter. `Deactivate` cleared retained activation intent and
returned xdisp to `available`; child PID is zero, Virtual is disconnected, and
the Pi receiver remains active/configured. No manual session restart was added
beyond LightDM's own two restart attempts.

Do not retry the 30-minute run in this receiver session. The next work is to
reproduce and diagnose the HID/convergence LightDM exit independently of
transport, then repeat E2.5 without a compositor restart.

## Final classification

**Classification: E2-C - RECONNECT OR OWNERSHIP FAILURE.** Native lifecycle
ownership, activation/deactivation, active USB-detach containment, fresh GUD
rediscovery, retained-intent reconnect, 10/10 normal cycles, core Virtual
Touchpad UX, and reproducible capped transport all pass. E2-A is blocked because
teardown relies on the bounded forced-stop fallback and the required stability
run ended in a LightDM/system-compositor restart when USB HID devices were
attached. This is not E2-D: the qualified capped driver is source-reproducible
and bit-for-bit rebuilt. E2 remains lifecycle/product incomplete; do not begin
H.264 work.
