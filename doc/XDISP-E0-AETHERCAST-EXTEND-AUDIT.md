# XDISP-E0 Aethercast extend audit

## Result

**E0-A lifecycle/source gate passed; visual application-content observation is still pending.** The exact Aethercast-style request activates `Virtual` as a connected, used `1280x720` output at `+1080+0`; Lomiri creates output 3's `PlatformScreen` and enters a landscape `1280x720` external-screen layout. The client also receives completed `1280x720` CPU-mapped frames without using GUD.

The phone was at the Lomiri greeter during the run. The raw-frame fingerprint therefore remained unchanged; this records a stable greeter image, not a claim that a Terminal/application was visibly laid out on the virtual screen. Repeat the same probe from an unlocked Lomiri session and change application content to complete that visual-only observation. Do not involve GUD in that follow-up.

E0 supersedes the post-V0.2 decision to defer independent/extended-display work. V0.2 targeted the Lomiri session Mir server and therefore exercised the wrong server for the Aethercast virtual-output lifecycle. The Aethercast-compatible system-server path is now the active architecture.

## Phone runtime (2026-07-28)

| Component | Installed version |
| --- | --- |
| Mir client/core/server/platform | `1.8.3-0ubports1+0~20260316153853.1+ubports24.04.1~1.gbpbd1c92` |
| Mir Android2 graphics platform | `1.8.0~20241214090501.18~5869d7e+ubports~dev` |
| QtMir | `0.7.2+0~20260224160609.1+ubports24.04.1~1.gbpc4a4f7` |
| Lomiri | `0.5.0+0~20260225173057.3+ubports24.04.1~1.gbp219367` |
| lomiri-system-compositor | `1.0.0+0~20250825193030.10+ubports~dev~1.gbpc02afe` |
| Aethercast | `0.4+0~20250902144210.2+ubports24.04.1~1.gbpd9ec3a` |

The phone kernel is `4.9.112-g6b190d86b`.

## Process and socket architecture

```text
aethercast service (root; normally inactive)
  \-- hard-coded Mir client socket: /run/mir_socket
        \-- lomiri-system-compositor (root, PID 2298 in this run)
              \-- host display configuration, including output 3: Virtual

lomiri (phablet, PID 3664 in this run)
  \-- MIR_SERVER_HOST_SOCKET=/run/mir_socket
  \-- MIR_SERVER_FILE=/run/user/32011/mir_socket
  \-- loads graphics-android2.so.16 and libqt5mir1server.so.1
  \-- presents the user/session server on /run/user/32011/mir_socket
```

V0.2 did not use the Aethercast server: its default client connection was the normal session socket, while Aethercast explicitly uses `/run/mir_socket`. An SSH client is rejected by the session authorizer but accepted by the system socket. `mirout` verified the latter directly.

## Aethercast source trace

```text
ac::mir::SourceMediaManager::Configure()
  -> ExtractRateAndResolution(format)
  -> DisplayOutput{Mode::kExtend, negotiated_width, negotiated_height, fps}
  -> producer->Setup(output)                 // ac::mir::Screencast
  -> mir_screencast_create_sync(spec)
```

`Screencast::Setup()` rejects non-extend mode; connects as `"aethercast screencast client"` to `/run/mir_socket`; finds the first connected/used output; and sets the stream to the negotiated sink size. It sets `capture_region = { primary_mode.width, 0, negotiated_width, negotiated_height }`, selects a Mir pixel format, sets `mir_mirror_mode_vertical`, requests two buffers, and retains the screencast buffer stream. The retained producer owns the virtual-output lifetime: releasing the screencast destroys the server context and disables the output.

UBports change `6447d32` made the region size equal the negotiated stream size. On this phone the exact request is `{1080, 0, 1280, 720}`.

`kExtend` is a lifecycle label, not a separate public Mir API flag or D-Bus request. It is the off-primary request plus the fixed system-server socket, vertical mirror, two-buffer request, and retained producer lifetime. No extra Lomiri/Aethercast D-Bus display signal was found.

## Mir server trace

UBports Mir 1.8.x implements the decision in `src/server/compositor/compositing_screencast.cpp`:

```text
capture region
  -> needs_virtual_output(display.configuration(), region)
  -> compare with bounding rectangle of connected outputs
  -> display.create_virtual_output(region.width, region.height)
  -> ScreencastSessionContext owns VirtualOutput
  -> VirtualOutput::enable()
  -> Android display configuration / hotplug propagation
```

`needs_virtual_output()` returns true only when the requested region has no intersection with the bounding rectangle of connected outputs. For the `1080x2280+0+0` panel, `{1080,0,1280,720}` is outside the panel rectangle. `VirtualOutput::enable()` calls `set_virtual_output_to(1280,720)` and hotplug propagation presents output 3 at `+1080+0`.

The checked UBports source branch is Mir 1.8.1; the installed package is the later 1.8.3 UBports rebuild. The observed 1.8.3 behavior matches this exact decision/lifetime: output 3 was connected/used only while the screencast was retained, and disconnected on release.

## Why V0.2 did not reproduce Aethercast

V0.2 did request the relevant off-primary rectangle, but it targeted the Lomiri session server rather than Aethercast's `/run/mir_socket` host server. It also omitted Aethercast's vertical-mirror/two-buffer fields and client identity. It therefore exercised a different display configuration and could only report that session server's disconnected `virt` output. Its static off-primary image was expected for that non-rendered session region.

## Minimal probe

`mirgud` now has an explicit source-only extend mode:

```bash
mirgud --source-mode extend --mir-socket-file /run/mir_socket \
  --size 1280 720 --no-gud --extend-hold
```

It refuses a manual `--capture-region` in extend mode, calculates the region from the selected primary output, uses Aethercast's client name, vertical mirror, and two buffers, and logs topology before/after creation. `--extend-hold` keeps the exact screencast lifetime without consuming buffers; it is the safe first/second gate. Omitting it reads/releases raw frames.

No Mir server or Android graphics module was rebuilt or deployed. The existing platform already has virtual-output diagnostics, and changing it again would reintroduce the prior LightDM restart-loop risk.

## Hardware evidence

Baseline `/run/mir_socket` topology:

```text
Output 1: LVDS, connected, enabled, 1080x2280+0+0
Output 2: DisplayPort, disconnected
Output 3: Virtual, disconnected, 1920x1080
```

During the five-second `--extend-hold` run:

```text
Output 3: Virtual, connected, 1280x720+1080+0, enabled, on,
          scale=1.00, form_factor=monitor
```

Lomiri logged `Output 3: Virtual connected, used`, created `PlatformScreen(output=3, geometry=QRect(1080,0 1280x720))`, and selected a `root width: 1280 height: 720` external layout. On clean probe termination, output 3 returned to disconnected.

The raw source run received 127 completed `1280x720`, format-1, stride-5120 frames at about 25 FPS with no conversion failures and a stable 14 client FDs. Its fingerprint did not change because the greeter content was static. The initial CPU mapping cache was invalid after virtual-screen reconfiguration; refreshing `MirGraphicsRegion` per acquired buffer fixed that. A SIGTERM issued while synchronous buffer release was blocked did not immediately reap the raw probe, so its known PID was force-terminated after the server had disabled output 3. The hold mode exits cleanly and is the recommended lifecycle probe.

No GUD driver, Pi gadget, USB transport, H.264/OMX, HWC, synthetic physical output, or aspect-ratio scaler was touched.

## Current development direction

The active goal is now the **proper Aethercast-style extended display**, not scaled primary mirroring.

The immediate architecture is:

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
existing RGB565 conversion
      ↓
existing bounded Raw GUD presenter
      ↓
USB → Pi → HDMI
```

The mirror/capture path remains useful as a fallback and transport diagnostic, but it is no longer the preferred user-facing architecture because it renders the phone's portrait scene and scales it into a landscape mode.

### Active phases

1. **E0.1 — visual-content gate, source only.** Run extend mode from an unlocked Lomiri session, create changing application content on output 3, and prove that the captured `1280x720` frames contain a correctly laid-out landscape Terminal/application rather than a stretched portrait scene.
2. **E1 — connect the proven extend source to Raw GUD.** Feed those native `1280x720` completed frames into the existing RGB565/GUD presenter and verify the full system-Mir → Lomiri external scene → GUD → Pi → HDMI path.
3. **E2 — extended-display lifecycle and UX hardening.** Validate clean connect/disconnect, repeated reconnects, shell/application behavior, phone-side Virtual Touchpad or equivalent supported behavior, pointer/window behavior where applicable, monitor mode selection, and 60-second resource stability.
4. **E3 — Raw GUD quality/performance hardening.** Improve the working raw path as needed for a usable product: frame pacing, stale-frame dropping, damage/update behavior, transport recovery, mode handling, and visual correctness. Do not replace the source architecture while these fundamentals are still being validated.

The **active roadmap ends with E3 and external-display milestone acceptance**. The milestone is complete only after the extended landscape UI is visually correct on HDMI, applications are usable, lifecycle/reconnect behavior is reliable, monitor modes behave correctly, and the Raw GUD path is resource-bounded and sufficiently stable for sustained operation.

## Post-milestone transport research — not part of the active roadmap

H.264/OMX is **not part of E0.1, E1, E2, E3, or the external-display milestone**. Do not start encoder, decoder, zero-copy, compressed-video USB, or related H.264 work while any extended-display source, Lomiri behavior, Raw GUD presentation, lifecycle, monitor-mode, resource-stability, visual-quality, or usability work remains unresolved.

Only after the complete Aethercast-style extended-display + Raw GUD path has passed the milestone above should a separate future project evaluate whether replacing or supplementing Raw GUD with a hardware-compressed video transport is worthwhile.

That later decision should compare the finished Raw GUD implementation against candidate compressed transports using measured bandwidth, latency, text quality, motion behavior, CPU/GPU cost, reliability, and implementation complexity. H.264 is one possible optimization candidate, not a prerequisite and not the next phase.

## E0.1 live Lomiri application-content result (2026-07-28)

**Classification: E0.1-A — real landscape Lomiri application content is
captured correctly.** The validated source remains the system Mir server at
`/run/mir_socket`, using the Aethercast-compatible client identity, vertical
mirror, two requested buffers, and the retained off-primary screencast. No
GUD card was opened for this gate.

The source was run as:

```bash
/home/phablet/mirgud-e01-orient.bin \
  --source-mode extend --mir-socket-file /run/mir_socket \
  --size 1280 720 --no-gud \
  --dump-frame /home/phablet/xdisp-e01-unlocked-morph.ppm \
  --dump-frame-after 1500 --monitor-pid 2298
```

At five seconds, `mirout /run/mir_socket` reported `Virtual` as connected,
enabled and on at `1280x720+1080+0`, `form_factor=monitor`. It returned to
disconnected after the source was terminated; both `lomiri-system-compositor`
and `lomiri` remained alive. No new `binder -12`, `KGSL -24`, `BUG`, or
`Oops` record was found in the accessible phone kernel tail.

### Visual evidence

The first settled shell dump was
`/home/phablet/xdisp-e01-shell-late.ppm` (local copy
`/tmp/xdisp-e01-shell-late.ppm`, SHA-256
`cfab273a23f87b34b300641753a14d4d62a92d77fccd6d6ab5d701c50244ad02`).
It is an upright `1280x720` landscape Lomiri external shell with a panel,
left launcher, landscape background, and status/lock area. The initial CPU
copy was vertically inverted. For this phone's tested Aethercast-compatible
extend source, the CPU `MirGraphicsRegion` must be copied top-down to produce
an upright frame. `0618735dba9817ce48cdd7c5ac1abf1c2efff044` corrected the
owned RGB565 row order; the settled frame thereafter is upright, with no
scaling or topology change.

This is specifically E0.1 extend-path evidence. The historical/default CPU
screencast path is treated as bottom-up by the existing Mir screencast
reference implementation, and the primary capture path retains that row order.
The extend source also requests `mir_mirror_mode_vertical`; that is a strong
architectural correlation with the observed top-down order, not proof that it
is the cause. E0.1 does not establish that every `MirGraphicsRegion` is
universally top-down or universally bottom-up.

The application-content frame used for the gate was
`/home/phablet/xdisp-e01-unlocked-morph.ppm` (local copy
`/tmp/xdisp-e01-unlocked-morph.ppm`, SHA-256
`7e8155cb2332e225373cbed05ecfb65172c906b16f73388d1efa98a7fbaebb7b`).
It shows the Lomiri Gallery application in a decorated landscape external
window: natural text and controls, a wide application area, the external
launcher/panel, and uncovered landscape wallpaper at either side. It is not a
scaled or horizontally stretched `1080x2280` phone image.

The second-app switch dump was
`/home/phablet/xdisp-e01-settings.ppm` (local copy
`/tmp/xdisp-e01-settings.ppm`, SHA-256
`01598d26795ea73324e120bcf917491c58b43b41650c8779ba70a0429b1f1609`).
It shows `lomiri-system-settings` replacing Gallery in the same external
landscape scene, with its natural two-column System Settings layout. The
source fingerprints changed during the Gallery/Settings transitions
(`0x82fb6fa70d05ac25` -> `0xb6f1ecccea33f407` ->
`0xf942696fa18a1655` in the Settings run), then stabilized once the selected
application was rendered.

No Terminal desktop entry/package was present on this phone; the installed
normal Lomiri applications used for the visual matrix were Gallery and System
Settings. `lomiri-app-launch lomiri-system-settings` reported a later registry
disconnect after starting, but the captured external frame itself proves the
settings application was rendered successfully.

The physical phone-side state is not observable through SSH and was not
asserted from the frame dumps. Record a direct observation of whether Lomiri
showed Virtual Touchpad or another supported phone-side state with the E1
hardware observation.

### Source stability samples

| Sample | Completed frames | Client FDs | Conversion failures | Notes |
| --- | ---: | ---: | ---: | --- |
| baseline | 0 | n/a | 0 | Virtual disconnected before source creation |
| first frame | 1 | 14 | 0 | CPU-mapped format 1, stride 5120 |
| 5 s | 106 | 14 | 0 | Virtual connected/used at 1280x720+1080+0 |
| 15 s | 360 | 14 | 0 | application layout settled |
| 30 s | 720 | 14 | 0 | stable frame ownership |
| 60 s | 1500 | 14 | 0 | stable at about 25 FPS |
| teardown | 0 | n/a | 0 | Virtual disconnected; both compositors alive |

`monitor_fds` and `monitor_sync_files` were unavailable to the unprivileged
client (`0` from the protected root compositor `/proc` view); the client FD
count remained flat, with no sign of the former monotonic resource failure.

E0.1 therefore proves that the Aethercast-style system-Mir virtual output
does produce a real, correctly oriented, native `1280x720` Lomiri external
application scene. E1 may proceed only after the already-established GUD/Pi
preflight is again healthy.

## Phase 1 revalidation and E1 preflight (2026-07-28)

**Phase 1 passed; E1 has not started.** The scoped row-order implementation
is commit `702773815e4d0230f424f0ef8b2898837ca58996`, following the original
extend-only orientation correction
`0618735dba9817ce48cdd7c5ac1abf1c2efff044`. The matching documentation
correction is `a30b68f`. A phone-matched container build completed and the
focused filter
`GudScreencastRgb565.*:GudPresentationWorker.*:GudHwcBoundary.*` passed all
10 tests. This includes RGB565 channel conversion plus separate top-down and
bottom-up, padded-stride row-copy tests.

The fresh payload was deployed without replacing earlier evidence artifacts
as `/home/phablet/mirgud-e1-7027738.bin` (SHA-256
`6c7a78bdbf0d161cb7160bb8075286488b293dac0ecbcaa6e7c0df31e83ef5a6`).
With GUD explicitly disabled, the system-Mir source checks produced these
new, retained local copies:

| Source mode | Explicit row order | Result |
| --- | --- | --- |
| `extend` | top-down | `/tmp/xdisp-e1-phase1-extend.ppm`, SHA-256 `0f2f5fbc3e076bb0405bae28eec7eb38a3d94f1f28b63c2087ec3f57a1ef42a3`: upright `1280x720` landscape external shell, panel and launcher visible. 175 frames in eight seconds, 14 client FDs, zero conversion failures. |
| `primary` | bottom-up | `/tmp/xdisp-e1-phase1-primary.ppm`, SHA-256 `3f42fe3cb8c8b2e337a3074aebc1ccf909644d488d190e56fbc4b3d528db2869`: readable, non-inverted primary UI. 168 frames in eight seconds, 14 client FDs, zero conversion failures. |

The default greeter-session socket rejected the primary probe with a broken
pipe before it created a stream, so that probe was repeated against the
healthy system socket `/run/mir_socket`; it captured the primary `(0,0,
1080,2280)` region and logged `row_order=bottom-up`. The extend probe used
the fixed Aethercast-compatible system socket and requested `(1080,0,
1280,720)`, logging `row_order=top-down`. After both probes, `mirout
/run/mir_socket` reported `Virtual, disconnected`; no `mirgud` process
remained and both `lomiri-system-compositor` and `lomiri` were alive.

### GUD preflight and transport result

The user has granted standing authorization to use the existing Pi SSH
credentials for this work. The Pi receiver preflight passed: `gud-userspace`
was active, FunctionFS was mounted at `/dev/ffs-usb-gadget0-0`, its bulk OUT
endpoint was open, and the `3f980000.usb` UDC was `configured`.

The OnePlus 6 saw the receiver at the dynamic USB path
`1-1.2:1.0` (`1d50:614d`). The unchanged
`/home/phablet/gud.ko` (SHA-256
`bd15c2c1bc4cd941bcac88bb13276b67620d9e2eec515973ff815add68f3630c`)
was then loaded as the smallest recovery action. It created `/dev/dri/card1`
while MSM remained `/dev/dri/card0`; `card1` resolved to the GUD USB interface
and its `Virtual-2` connector became connected. The receiver advertised and
accepted the exact `1280x720` timing during the modeset.

**Phase 2 transport gate failed, so E1 has not started.** The established
`/home/phablet/gud-kms-fill /dev/dri/card1` static RGB565 test began the
`1280x720` modeset, but its first `64000`-byte payload failed. The phone
reported `GUD bulk transfer failed after 0 retries: -110`, followed by
`GUD atomic update failed: -110`; its connector query then also timed out.
The Pi received the mode-check/commit and display-enable controls, then
recorded an invalid FunctionFS read for payload sequence 1 and entered its
terminal `Poisoned` state. The Pi service and UDC remained active/configured,
but the receiver explicitly directs that a poisoned instance must not be
stopped, restarted, rebooted, or retried.

#### 64 KB versus 12.8 KB transport boundary

The failed `64000`-byte transfer identifies a transport configuration mismatch,
not an E0.1 or `mirgud` regression. The normal module used in this run
advertised `max_buffer_size=8294400`; its uncompressed `gud-kms-fill` update
therefore selected a `1280x25` RGB565 rectangle (`1280 * 25 * 2 = 64000`).
That size is legal for the normal module's descriptor, but it is not the
qualified OnePlus transport boundary.

The passing `<=12800`-byte evidence belongs to the separately built,
explicitly named `xdisp-lz4-12800` diagnostic module, not to the normal
`/home/phablet/gud.ko`. That variant adaptively plans complete-row rectangles
and rejects every actual submission larger than 12,800 bytes; because both
modules have the internal name `gud`, it must replace—not coexist with—the
normal module during its isolated diagnostic run.

After physical recovery of the poisoned receiver, the next task is
**transport-only** and must not alter E0.1, E1 source code, or `mirgud`:

1. Preserve the failed phone/Pi logs and identify the normal-module SHA,
   descriptor `max_buffer_size`, `SET_BUFFER` length/compressed length, host
   `trlen`, and Pi received byte count as the 64 KB baseline.
2. Stage and identify the existing named `xdisp-lz4-12800` diagnostic artifact
   by its module version/description and SHA; never overwrite the normal
   module.
3. Run only the static GUD gate after fresh enumeration, requiring every
   actual submitted payload to be `<=12800` and every Pi receive sequence to
   return from `InFlight` to `Idle` with matching byte counts.
4. Treat any larger payload, `-110`, short/invalid read, poisoned session, or
   receiver fault as another transport stop condition. E1 remains blocked
   until this static gate passes.

This is a Phase 2 stop condition, not an E1 result. No E1 source-to-presenter
command, HDMI visual claim, phone-side UX claim, or 60-second test was run.
Preserve the phone/Pi logs and recover the receiver only through the documented
physical/hardware path before repeating the known-good static transport gate.
The intended E1 command after that gate passes is:

```bash
env MIR_CLIENT_PLATFORM_PATH=/usr/lib/aarch64-linux-gnu/mir1/client-platform \
    MIR_SERVER_PLATFORM_PATH=/usr/lib/aarch64-linux-gnu/mir1/server-platform \
    /home/phablet/mirgud-e1-7027738.bin \
    --source-mode extend --mir-socket-file /run/mir_socket --size 1280 720
```

It must first follow the Phase 2 static-transport gate, then run the full
visual and 60-second stability matrix. No E1 classification, HDMI visual
claim, phone-side UX claim, or Raw-GUD presentation result is made here.

## Capped transport recovery and E1 integration (2026-07-28)

The Pi was physically rebooted after the poisoned normal-module run. Its new
receiver instance was active with FunctionFS mounted and the UDC configured.
The normal phone module was then unloaded and replaced with the separately
staged diagnostic artifact
`/home/phablet/gud.xdisp-p0.1-adaptive-12800-2a8f59b.ko` (SHA-256
`2369eccc5cf3afc7ff8364921b8ce94fec8363518466f727b71d2d009c6c5999`,
version `xdisp-p0.1-adaptive-12800-v1`). Its unique probe record confirms the
`gud_xdisp_lz4_12800` driver and an actual bulk-payload cap of `12800` bytes.
MSM remained card0 and the diagnostic GUD interface re-enumerated as card1.

The static `gud-kms-fill /dev/dri/card1` gate then passed: the phone logged a
maximum submitted payload of `12761`, and the Pi received its four complete
payloads (`12761`, `12618`, `12149`, and `3325` bytes) without an error or
poisoned receive session.

With that gate passed, the unchanged E0.1 source was connected to the bounded
Raw-GUD presenter for the planned 68-second E1 run:

```bash
env MIR_CLIENT_PLATFORM_PATH=/usr/lib/aarch64-linux-gnu/mir1/client-platform \
    MIR_SERVER_PLATFORM_PATH=/usr/lib/aarch64-linux-gnu/mir1/server-platform \
    /home/phablet/mirgud-e1-7027738.bin \
    --source-mode extend --mir-socket-file /run/mir_socket \
    --size 1280 720 --monitor-pid 2298
```

It completed the initial modeset and sustained the exact system-Mir
off-primary request. During the active run, `Virtual` was connected/used at
`1280x720+1080+0`; the source logged top-down CPU mapping and bounded frame
replacement with no conversion or GUD-submission failure in the captured
metrics. The source client held 15 FDs in its sampled reports; protected root
compositor FD/sync counts remained unavailable to that client.

The Pi recorded `5864` complete capped payload receives during the post-reboot
window, with an observed maximum transfer size of `12794` bytes, all below the
`12800` invariant. It remained active/configured with no new `Poisoned`,
short-FunctionFS-read, or transport-timeout record. After timed termination,
no `mirgud` process remained; `mirout /run/mir_socket` reported `Virtual,
disconnected`, and both `lomiri-system-compositor` and `lomiri` remained alive.
There was no new diagnostic-module-era GUD `-110`, atomic-update failure,
`BUG`, `Oops`, binder `-12`, or KGSL `-24` record.

**Classification at the end of the remote run: E1 transport/lifecycle
integration passed; final visual classification was pending direct HDMI
observation.** The phone was at
the Lomiri greeter during the remote run, and SSH cannot observe the monitor
or physical-phone external-display UX. Therefore this record does not yet
claim upright HDMI pixels, colors, application switching, a native landscape
application window, or Virtual Touchpad behavior. Those direct observations
must be made from an unlocked session before calling the result E1-A.

## E1 direct visual result (2026-07-29)

**Classification: E1-A — SUCCESS.** Direct observation of the proven E1 path
confirmed that the HDMI image was upright, its colors and proportions looked
correct, and application content was usable. The phone operated as Lomiri's
Virtual Touchpad while the native landscape external desktop was active.

This completes the visual-only gate left open by the remote run. The measured
result remains 5,864 complete Pi payload receives over 68 seconds, a maximum
payload of 12,794 bytes, no new GUD `-110`, no short FunctionFS read, no
poisoned receiver state, no conversion or GUD-submission failure, clean
Virtual-output teardown, and both compositors alive. The successful transport
used the separately staged `gud_xdisp_lz4_12800` diagnostic module; making that
capped variant reproducible remains E2 work.
