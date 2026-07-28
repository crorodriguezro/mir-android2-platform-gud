# XDISP-V0: Mir virtual/screencast to GUD

## Status

**V0-A — MIR SCREENCAST TO GUD TO HDMI WORKS IN MIRROR/CAPTURE MODE.** Static
GUD presentation was recovered and live Mir/Lomiri application pixels reached
the Pi HDMI monitor from a Lomiri-authorized Terminal session. This is a
working scaled mirror/capture path, not an independent or extended desktop.
The earlier V0-B timeout is retained below as dated historical evidence.

**Current development has moved to the E0 Aethercast-compatible extended-display
path documented in `XDISP-E0-AETHERCAST-EXTEND-AUDIT.md`.** The V0 mirror path
remains useful as a fallback and transport diagnostic, but the roadmap at the
end of this document is historical and is superseded by E0.

`mirgud` is a standalone client POC. It deliberately does not alter, enable,
or consume the retired synthetic Android DisplayPort output. The normal Android
primary remains the active phone panel while Mir services the screencast.

## Source audit and ownership

The Android graphics platform provides `Display::create_virtual_output()` in
`src/platforms/android/server/display.cpp`. Upstream Mir screencast
implementations may use that facility under certain capture conditions. The
V0.2 session-server runtime did not expose an enabled `virt` output during the
tested screencast sessions; E0 later proved that the Aethercast-compatible
system-server path does activate that virtual output.

The raw consumer hook is the existing client screencast API, not an Android
`DisplayBuffer` borrowed from the compositor:

```
Mir screencast session -> MirBufferStream -> MirGraphicsRegion / EGL surface
```

`src/utils/screencast.cpp` is the pre-existing reference implementation. It
uses `mir_buffer_stream_get_graphics_region()` when the stream is CPU mapped;
the returned `MirGraphicsRegion` has actual width, height, byte stride, pixel
format, and a bottom-up virtual address. It releases that current frame through
`mir_buffer_stream_swap_buffers_sync()`. There is no exported acquire-fence
object at this client boundary: successful synchronous swap is the completion
and release operation. If direct mapping is unavailable, the same utility uses
an EGL surface and `glReadPixels()`, followed by `eglSwapBuffers()` to release
the stream buffer.

Thus the Mir buffer stream can provide consecutive completed frames without any
video encoder. V0 deliberately keeps the source and Raw GUD presentation path
independent of OMX or compressed-video transport. Any H.264/OMX investigation
is a post-milestone optimization and is not part of the active extended-display
work.

V0 uses the enabled primary output as its capture region and requests a
1280x720 screencast image, so V0 output is a scaled **mirror/capture**, not an
independent desktop.

`mirgud` always follows this ownership sequence:

```
completed Mir buffer -> CPU copy and RGB565 conversion -> swap/release Mir buffer
                       -> one owned RGB565 frame pending -> GUD atomic commit
```

The worker has one active GUD submission and one replaceable pending vector;
it never stores a `MirGraphicsRegion`, gralloc buffer, EGL image, or fence after
the source buffer is swapped. This is intentionally conservative around USB
backpressure.

## Implementation

- `src/utils/gud_screencast.cpp` adds `mirgud`, the POC executable.
- `src/utils/gud_screencast_rgb565.h` explicitly converts the delivered
  ABGR/XBGR, ARGB/XRGB, RGB/BGR888, or RGB565 rows to RGB565, respecting byte
  stride and the source's inverted row order.
- `--pattern` runs Stage A through exactly the same GUD KMS presenter as Stage
  B. Normal operation requests a 1280x720 screencast and performs Stage B.
- The existing GUD FunctionFS/kernel/Pi path is reused through the phone's GUD
  DRM card and atomic RGB565 framebuffer commits. No GUD protocol, gadget, or
  HDMI component is changed.

The presenter selects only an advertised connected GUD mode matching the
requested size. This avoids silently scaling or selecting a physical mode that
does not match the source. The default remains 1280x720; pass `--size W H`
only when the Pi advertises that exact mode and the test is explicitly being
repeated at that size.

## Build and focused test

Use the phone-matched container procedure in `XDISP-P0.2-GUD-PRESENTATION-TEST.md`.
After configuration, add `mirgud` to the target list and run:

```bash
cmake --build /build --target mirgud mir_unit_tests_android2 --parallel 1
/build/bin/mir_unit_tests_android2.bin \
  --gtest_filter='GudScreencastRgb565.*:GudPresentationWorker.*:GudHwcBoundary.*'
```

The new focused tests cover RGBA/BGRA channel order and direct RGB565 row
handling. Build and hash the `mirgud.bin` artifact separately from the platform
plugin; this V0 client does not require mounting a modified graphics plugin.

## Hardware procedure and evidence

Complete the existing safe GUD/Pi preflight before either stage. Start with the
normal packaged Mir platform and a working primary phone display.

### V0.1 transport gate

V0.1 separates the black initial GUD modeset from Stage A. It is therefore
incorrect to call a failure from the first `mirgud --pattern` attempt a
checkerboard failure unless the logs first contain both:

```text
mirgud: V0.1 initial modeset complete
mirgud: V0.1 update 1 begin
```

Before the first retry, collect the Pi's current and, when retained, previous
boot (`uptime`, `who -b`, `last -x`, `journalctl --list-boots`,
`journalctl -b -1`, `journalctl -k -b -1`, and
`journalctl -u gud-userspace.service -b -1`). Search those records for panic,
Oops, watchdog, OOM, voltage, DWC2/UDC, FunctionFS, endpoint, reset,
disconnect, stall, and timeout messages. A Wi-Fi loss alone is not evidence of
a Pi reboot.

On the recovered Pi, preserve `systemctl status gud-userspace.service`, its
PID and start time, `findmnt -t functionfs`, `ls -l /dev/ffs-usb-gadget0-0`,
and `/sys/class/udc/*/{state,current_speed}` before the phone commits a
framebuffer. On the phone, preserve the `1d50:614d` device and dynamic USB
path, the GUD DRM card/connector/mode, and the descriptor line emitted by
`gud.ko`. `service active` is not endpoint readiness; the first Pi
`FunctionFS bulk OUT endpoint is armed` record is the receive-side boundary.

Use the host driver's bounded trace for the first transaction only. The normal
defaults remain quiet and retain the 3000 ms timeout:

```bash
# On the phone, after loading gud.ko; root required.
echo 1 > /sys/module/gud/parameters/bulk_trace_limit
echo 3000 > /sys/module/gud/parameters/bulk_timeout_ms
```

The matching Pi `RUST_LOG=debug` service trace must show `received and
validated GUD SET_BUFFER`, `FunctionFS bulk OUT endpoint is armed`, the
blocking-read start/completion records, and either `frame_stats` or its exact
error. Correlate `GUD trace=N SET_BUFFER` and `GUD trace=N bulk` with the Pi
payload sequence: advertised `max_buffer_size`, request `length`,
`compressed_length`, submitted `trlen`, expected Pi bytes, and received Pi
bytes must agree.

Only after preserving the 3000 ms trace may a diagnostic retry use 10000 or
30000 ms. A later completion records a slow-but-progressing path, not a fix;
repeated timeouts at all three values are an unresolved receive stall. The
active XDISP driver additionally caps each actual bulk payload at 12,800 bytes,
so inspect the trace rather than assuming the initial 1280x720 modeset is one
full-frame USB transfer.

```bash
# Optional stop-condition probe: prove the virtual source itself stays healthy
# before opening the GUD card. It copies/releases every frame but submits none.
mirgud --no-gud --size 1280 720 --monitor-pid "$(pidof unity8)"

# Stage A: verify the exact GUD call path, no Mir frames involved
mirgud --pattern --size 1280 720 --monitor-pid "$(pidof unity8)"

# Stage B: stop Stage A cleanly, then capture the normal Mir primary at 720p
mirgud --size 1280 720 --monitor-pid "$(pidof unity8)"
```

The first Stage B logs must include source enabled, source format/stride, first
CPU-complete copy (or EGL-readback copy), and GUD enabled. Once a second it
prints received, presented, dropped, conversion-failure, GUD-submit-failure,
and target compositor FD/sync-file counts. Capture these at baseline, virtual
source enabled, first frame, 5, 15, 30, and 60 seconds. Save `fdinfo`, Mir and
Pi journals, and checks for new `binder -12` or `KGSL -24` messages. Stop the
POC and preserve that evidence if either count rises monotonically.

Record the executable SHA-256, exact command, selected GUD mode, actual visual
content, approximate FPS, Pi acknowledgement/journal, and all samples under a
new ignored evidence directory. The procedure above was completed during V0.1;
the resulting V0-A success is recorded below.

## 2026-07-28 hardware result — V0-B

Evidence is retained under
`gud/backport-4.9/env/local/evidence/xdisp-v0-hardware-2026-07-27T2359COT/`.
The phone gate found the Pi at dynamic path `1-1.3`; the Pi was initially
`active` and `configured`, the normal phone GUD driver advertised connected
`1280x720`, and `mirgud.bin` SHA-256 was
`b7861a00a27b7e3e140759fad32f48dc7ada9e98f2fc641bfeaeb11de8950cdc`.

Stage A did **not** produce a successful GUD submission. The first atomic
commit timed out and every later state request failed with `-110`; phone
kernel evidence records `GUD bulk transfer failed after 0 retries: -110` and
`GUD atomic update failed: -110`. The external monitor therefore was not
claimed to show the checkerboard, and Stage B was not run against the failed
sink. The Pi became unreachable over Wi-Fi immediately after the timeout;
only read-only retries were made and its service was not restarted, rebooted,
or otherwise changed.

The independent source-only probe was launched through Lomiri's app launcher
(direct SSH clients are intentionally rejected by the session authorizer). It
received and released 2,324 CPU-mapped `1280x720` format-1 frames at roughly
28 FPS, with zero conversion failures. Lomiri began at 141 FDs/1 sync file,
plateaued around 154--157 FDs with 0--2 sync files while the probe ran, and
settled at 151 FDs/1 sync file thirty seconds after it exited. There was no
monotonic sync-file trend and no new binder `-12`, KGSL `-24`, `BUG:`, or
`Oops` record. The retained `fdinfo` snapshot documents the small persistent
FD delta for follow-up.

**Classification: V0-B — VIRTUAL SOURCE STABLE, GUD PRESENTATION FAILED.**
The completed-frame source is usable and resource-bounded over the observed
interval, but the existing GUD transport failed before any live Mir pixels
could reach the Pi HDMI monitor. At that point Stage B and performance work
were deferred pending recovery of the `-110` Stage A transport failure and Pi
reachability; the later V0.1 result records that recovery.

## 2026-07-28 hardware result — V0.1 transport recovery and V0-A success

The earlier V0-B transport result was recovered without changing the validated
Mir screencast ownership or RGB565 conversion path. The Pi enumerated as
`1d50:614d`, the phone registered the GUD card as `/dev/dri/card1`, and the
static RGB565 checkerboard ran successfully through the Pi HDMI output.

During the successful static run there was no host `-110`, no submission
failure, and no observed kernel fault. The Pi remained reachable, its UDC
remained configured, and FunctionFS completed exact bulk reads through at
least payload sequence 71 without a short read or receiver error.

Stage B must be launched from a Lomiri-authorized Terminal/application session;
a direct SSH process is rejected by Mir session authorization. When launched
from that authorized session, live Terminal/application pixels were visible on
the external HDMI monitor.

This verifies the complete mirror/capture path:

```text
Lomiri / Mir
    ↓
Mir screencast
    ↓
CPU-mapped frame
    ↓
RGB565 conversion
    ↓
bounded latest-frame presenter
    ↓
GUD DRM
    ↓
USB
    ↓
Pi FunctionFS
    ↓
HDMI
```

**Classification: V0-A — SUCCESS.** This is a live application mirror/capture
result only. It does not establish full Lomiri shell composition capture or an
independent desktop.

## 2026-07-28 hardware result — V0.2 virtual-region feasibility

V0.2 used the standalone `mirgud` client with `--no-gud`; neither the GUD
kernel driver nor the Pi gadget participated in these tests. The client takes
a fresh `MirDisplayConfig` both before and after
`mir_screencast_create_sync()`, so the post-creation topology is not a stale
configuration snapshot.

This result applies to the Lomiri **session** server used by V0.2. It was not
an Aethercast-equivalent request: Aethercast uses the host
`/run/mir_socket`, its fixed client identity, vertical mirror, two buffers,
and a retained extend-producer lifetime. The follow-up audit in
`XDISP-E0-AETHERCAST-EXTEND-AUDIT.md` reproduces that path and activates the
system server's virtual output. Do not use V0.2's disconnected session `virt`
observation to infer that the Aethercast lifecycle is unavailable.

The normal-primary baseline requested `(0,0,1080,2280)`. It delivered live,
changing CPU-mapped frames, but output 3 (`virt`) remained
`connected=0 used=0 top_left=(0,0)` in both fresh topology snapshots.

Two off-primary probes then requested `(1080,0,1280,720)` and
`(1081,0,1280,720)`, respectively. Both produced CPU-mapped `1280x720`
frames, but output 3 still remained disconnected and unused in the fresh
post-screencast configuration. Their sampled fingerprint was constant after
the first frame (`0xdce53c1df8560f83` through at least frame 360), consistent
with an empty/static off-primary region rather than independent desktop
content. Frame ownership remained bounded (`self_fds=14`) and there were no
conversion failures.

Upstream Mir 1.8.3's `CompositingScreencast` creates and enables a virtual
output when the capture region has no intersection with the connected-output
bounding rectangle. The UBports Mir packaging patch series contains no patch
to that screencast path. The active phone session runtime nevertheless did not
expose an enabled virtual output for either completely off-primary request. A
separately rebuilt Android platform module with V0.2 lifecycle tracing entered
a LightDM compositor restart loop, so it was immediately unmounted and the
known-good platform restored; no topology conclusion depends on that failed
deployment.

**Classification: V0.2-C — SESSION-SERVER SCREENCAST PATH DOES NOT PROVIDE AN
INDEPENDENT DESKTOP.** The tested Lomiri session server captures the populated
primary scene but does not expose output 3 (`virt`) as connected/used during
those screencast sessions. Completely off-primary requests return static/empty
content, not independently rendered Lomiri content.

E0 later resolved the apparent contradiction: Aethercast uses the system Mir
server at `/run/mir_socket`, not the Lomiri session server used by V0.2. The
Aethercast-compatible E0 request successfully activates a real, non-overlapping
`Virtual 1280x720+1080+0` output and Lomiri creates a landscape external-screen
layout for it. See `XDISP-E0-AETHERCAST-EXTEND-AUDIT.md` for the active result.

## Post-V0.2 architectural decision — superseded by E0

The decision made immediately after V0.2 to defer extended-display work and
continue with scaled primary mirroring is **superseded**.

E0 established that V0.2 had exercised the wrong Mir server for the Aethercast
virtual-output lifecycle. The active architecture is now:

```text
/run/mir_socket
      ↓
Aethercast-compatible extend source
      ↓
Mir Virtual 1280x720+1080+0
      ↓
Lomiri external 1280x720 landscape scene
      ↓
completed 1280x720 frames
      ↓
RGB565 / Raw GUD presenter
      ↓
USB → Pi → HDMI
```

The V0 mirror/capture path remains a useful fallback and transport diagnostic,
but it is no longer the preferred user-facing architecture. The current phase
sequence and acceptance gates are maintained in
`XDISP-E0-AETHERCAST-EXTEND-AUDIT.md`.

Compressed-video transport, including H.264/OMX, is explicitly **outside the
active roadmap**. Do not start that work until the proper extended-display UI,
Raw GUD presentation, application usability, connect/disconnect and reconnect
lifecycle, monitor-mode behavior, sustained resource stability, and Raw GUD
quality/performance work are all complete. Only then should a separate
post-milestone transport-optimization phase evaluate H.264 or other compressed
transports against the finished Raw GUD baseline.
