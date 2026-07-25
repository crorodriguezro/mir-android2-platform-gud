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

1. **Compositor stalls.** `GudOutput::present_external()` runs during
   `HwcDevice::commit()`. It performs synchronous GUD/KMS work, so a full
   frame transfer, slow Pi, or failed transfer blocks the same path that
   services the phone's UI. This explains the frozen display and very slow
   refresh observed during testing.
2. **Unreliable first transfer after rebind.** After a Pi rebind/reconnect,
   FunctionFS can accept enumeration but stall on its first bulk payload. The
   OnePlus reports a GUD bulk/atomic timeout (`-110`). This is owned by
   `gud-gadget` and must be gated independently of this plugin.
3. **Unstable DRM node.** The POC hard-codes `/dev/dri/card1`; GUD may become
   another card number after reconnect. A symlink is not a durable fix because
   an already-open old device file remains stale.
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

| ID | State | Owner | Next result required |
| --- | --- | --- | --- |
| `XDISP-P0.1` | blocked | `gud-gadget` | Ten post-rebind/reconnect cycles complete their first 64 KiB payload, without `-110`. |
| `XDISP-P0.2` | planned | this repository | Decouple submit/copy from `HwcDevice::commit()` using a bounded worker queue that keeps the newest frame and reports failures without blocking the UI. |
| `XDISP-P0.3` | planned | this repository + `gud` | Discover the active GUD DRM card and process removal/re-add instead of opening `card1` once. |
| `XDISP-P1.1` | planned | this repository + `gud-gadget` | Verify full-width, correctly placed external content through repeated enable/disable and reconnect cycles. |
| `XDISP-P2.1` | planned | all repositories | Measure end-to-end FPS, latency, CPU use, and dropped frames; then add damage-aware updates, matched mode selection, and only then compression if justified. |

Use only these lifecycle states: **planned**, **in progress**, **blocked**,
**verified**, and **rolled back**. A hardware task reaches **verified** only
when its listed acceptance test is retained with logs; one successful visual
frame is a diagnostic observation, not completion.

## Implementation direction

After `XDISP-P0.1` gives a reliable transport gate, implement `XDISP-P0.2`
before further visual work. The worker should own its GUD file descriptor,
bound memory and queue depth, drop superseded frames, and make device errors
visible to output/hotplug logic. Then implement dynamic discovery and proper
hotplug (`XDISP-P0.3`). Only with those protections should geometry and
throughput experiments continue.

The desired product remains an independent Lomiri external display. A separate
screen-capture bridge can still be useful as a mirror-only diagnostics and
performance baseline, but it does not meet the extended-desktop requirement.
