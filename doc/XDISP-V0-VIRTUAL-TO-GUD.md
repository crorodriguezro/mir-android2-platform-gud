# XDISP-V0: Mir virtual/screencast to GUD

## Status

**V0 implementation staged; hardware result pending.** No claim of HDMI
visibility, 60-second stability, or resource boundedness is made by this
document until the evidence procedure below is completed on the phone and Pi.

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
