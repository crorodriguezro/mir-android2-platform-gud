# XDISP-V0: Mir virtual/screencast to GUD

## Status

**V0-B — VIRTUAL SOURCE STABLE, GUD PRESENTATION FAILED.** The source-only
hardware evidence is recorded below; no HDMI visibility claim is made because
Stage A GUD presentation timed out before a checkerboard could be confirmed.

`mirgud` is a standalone client POC. It deliberately does not alter, enable,
or consume the retired synthetic Android DisplayPort output. The normal Android
primary remains the active phone panel while Mir services the screencast.

## Source audit and ownership

The Android graphics platform implements `Display::create_virtual_output()` in
`src/platforms/android/server/display.cpp`. The caller is Mir server's
screencast/virtual-output machinery (outside this platform module): it owns the
returned `graphics::VirtualOutput`, calls `enable()`, and holds it until the
session ends. `VirtualOutput::enable()` sets the platform's `virt` display
configuration to connected, used, powered on, and the requested size, then
causes `Display::on_hotplug()`.

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

Thus Aethercast-style consumers can receive consecutive completed frames
without OMX: the buffer stream is the source; OMX/H.264 is a later consumer
choice. `mirgud` retains the production path while bypassing OMX completely.
It uses the enabled primary output as its capture region and requests a
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
new ignored evidence directory. Classify the completed run exactly as V0-A,
V0-B, V0-C, or V0-D from the V0 brief; until this is executed the only honest
classification is **pending**.

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
could reach the Pi HDMI monitor. Do not pursue Stage B or performance work
until the `-110` Stage A transport failure and Pi reachability are resolved.
