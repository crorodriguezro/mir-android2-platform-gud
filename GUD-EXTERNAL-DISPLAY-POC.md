# GUD external-display proof of concept

## Scope and current state

This fork explores a software DisplayPort (Alt-Mode-like) shim for the OnePlus
6 Ubuntu Touch stack. It extends Mir's Android-HWC platform so Lomiri sees an
Android panel plus a connected DisplayPort-like output. The physical transport
is not USB-C DisplayPort Alt Mode:

```text
Lomiri external output -> Android gralloc buffer -> CPU RGB565 copy ->
GUD DRM/KMS -> USB -> Raspberry Pi FunctionFS gadget -> Pi HDMI
```

The POC builds against the phone's Mir ABI and was installed temporarily on the
phone. It is now **rolled back**: the original Android graphics platform plugin
has been restored. Do not deploy this branch as a normal phone configuration.

### 2026-07-27 P0.2 hardware attempt

The `3fffb05` async-worker implementation was rebuilt with the phone-matched
Noble/UBports environment in `d47b771`. Its commit-qualified test module
loaded successfully, and Mir exposed the synthetic 1280x720 external output.
It did not, however, reach `GUD POC output enabled` or start a Pi FunctionFS
receive session. During its 52-second run the phone generated sustained binder
`-12` allocation failures and KGSL `-24` file-descriptor exhaustion. The
module was immediately unmounted and LightDM restarted on the packaged plugin;
the Pi service was neither stopped nor restarted. This is a failed compositor
health gate, not a GUD worker, transport, or responsiveness success. P0.2
remains in progress pending root-cause work and a clean hardware retry.

## What the POC proved

- With a live GUD DRM node, Lomiri exposed a connected, used `DisplayPort-2`
  output with geometry `1280x720` beside the internal `1080`-pixel-wide panel.
- The cursor could be moved from the phone onto the Pi monitor. This proves a
  true extended desktop configuration, not a captured or mirrored screen.
- Both Android-HWC configuration paths needed the shim: the usual HWC power
  configuration and the legacy blanking-control configuration.
- The Pi received tiled RGB565 updates and performed page flips before the
  later USB endpoint failure.

This is feasibility evidence only. It does not establish stability, correct
layout for every client, acceptable performance, or reconnect support.

## Observed failures and likely boundaries

1. **Compositor stalls (source repair in progress).** The original
   `HwcDevice::commit()` -> `GudOutput::present_external()` path synchronously
   copied the gralloc buffer and called `drmModeAtomicCommit()`. P0.2 now
   retains only the latest buffer for a worker-owned KMS session, so no GUD
   copy or USB/KMS wait occurs in `commit()`. Component tests cover the queue,
   error, and shutdown behavior; a supported-project build plus retained phone
   evidence are still required before this is treated as verified.
2. **Unreliable first transfer after rebind.** After a Pi rebind/reconnect,
   FunctionFS can accept enumeration but stall on its first bulk payload. The
   OnePlus reports a GUD bulk/atomic timeout (`-110`). This is owned by
   `gud-gadget` and must be gated independently of this plugin.
3. **Unstable DRM node.** P0.2 replaces the POC's fixed node with a bounded
   scan for an accessible DRM driver named `gud`. It deliberately does not
   subscribe to DRM remove/add or reconfigure the Mir output; that complete
   reconnect lifecycle remains P0.3. A symlink is not a durable fix because an
   already-open old device file remains stale.
4. **Geometry/content mismatch.** The external monitor sometimes showed the
   orange phone background at full size and sometimes a tree background using
   full height but roughly one sixth of the width. The source copy uses buffer
   stride, so this is not yet evidence of a simple pitch error. It needs
   output, buffer, and shell-placement tracing at a stable transport layer.
5. **Avoidable scaling cost.** The Pi had a 1920x1080 HDMI mode while the POC
   submitted 1280x720. Pi logs showed roughly 39 ms scaling and 45--51 ms total
   handling for each 25-row tile, around 29 tiles for a full frame. A local Pi
   preference for a 1280x720 HDMI mode exists but is not end-to-end verified.

## Shared priority board

The canonical cross-repository board is `../gud/PROJECT-STATUS.md`. Copy these
IDs unchanged into commits, logs, and issue discussions.
Future cross-repository specifications and plans follow
`../gud/docs/superpowers/CROSS-REPOSITORY-WORKFLOW.md`; this document remains
the POC-specific record rather than a duplicate project plan.

| ID | State | Owner | Next result required |
| --- | --- | --- | --- |
| `XDISP-P0.1` | verified | `gud-gadget` | Verified operating constraint: every actual OnePlus-to-Pi payload is at or below 12,800 bytes. |
| `XDISP-P0.2` | in progress | this repository | Decouple submit/copy from `HwcDevice::commit()` using a bounded worker queue that keeps the newest frame and reports failures without blocking the UI. |
| `XDISP-P0.3` | planned | this repository + `gud` | Discover the active GUD DRM card and process removal/re-add instead of opening `card1` once. |
| `XDISP-P1.1` | planned | this repository + `gud-gadget` | Verify full-width, correctly placed external content through repeated enable/disable and reconnect cycles. |
| `XDISP-P2.1` | planned | all repositories | Measure end-to-end FPS, latency, CPU use, and dropped frames; then add damage-aware updates, matched mode selection, and only then compression if justified. |

Use only these lifecycle states: **planned**, **in progress**, **blocked**,
**verified**, and **rolled back**. A hardware task reaches **verified** only
when its listed acceptance test is retained with logs; one successful visual
frame is a diagnostic observation, not completion.

## Implementation direction

P0.2 is implemented at the source/component-test boundary. Its worker owns
the GUD fd and KMS buffers, keeps one newest pending `shared_ptr` frame, drops
superseded work, logs contained I/O errors, and joins before shutdown releases
KMS state. The absence/startup and retry boundaries are documented in
`doc/XDISP-P0.2-GUD-PRESENTATION-TEST.md`; the formal design and plan are in
the coordination repository. Full dynamic remove/add and output hotplug remain
P0.3. Do not infer hardware verification from source tests or deployment of
this experimental plugin.

The desired product remains an independent Lomiri external display. A separate
screen-capture bridge can still be useful as a mirror-only diagnostics and
performance baseline, but it does not meet the extended-desktop requirement.
