# XDISP-E0 Aethercast extend audit

## Result

**E0-A, with visual-content observation pending.** The exact Aethercast-style
request activates `Virtual` as a connected, used `1280x720` output at
`+1080+0`; Lomiri creates output 3's `PlatformScreen` and enters a landscape
`1280x720` external-screen layout. The client also receives completed
`1280x720` CPU-mapped frames without using GUD.

The phone was at the Lomiri greeter during the run. The raw-frame fingerprint
therefore remained unchanged; this records a stable greeter image, not a claim
that a Terminal/application was visibly laid out on the virtual screen. Repeat
the same probe from an unlocked Lomiri session and change application content
to complete that visual-only observation. Do not involve GUD in that follow-up.

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

V0.2 did not use the Aethercast server: its default client connection was the
normal session socket, while Aethercast explicitly uses `/run/mir_socket`. An
SSH client is rejected by the session authorizer but accepted by the system
socket. `mirout` verified the latter directly.

## Aethercast source trace

```text
ac::mir::SourceMediaManager::Configure()
  -> ExtractRateAndResolution(format)
  -> DisplayOutput{Mode::kExtend, negotiated_width, negotiated_height, fps}
  -> producer->Setup(output)                 // ac::mir::Screencast
  -> mir_screencast_create_sync(spec)
```

`Screencast::Setup()` rejects non-extend mode; connects as
`"aethercast screencast client"` to `/run/mir_socket`; finds the first
connected/used output; and sets the stream to the negotiated sink size. It
sets `capture_region = { primary_mode.width, 0, negotiated_width,
negotiated_height }`, selects a Mir pixel format, sets
`mir_mirror_mode_vertical`, requests two buffers, and retains the screencast
buffer stream. The retained producer owns the virtual-output lifetime:
releasing the screencast destroys the server context and disables the output.

UBports change `6447d32` made the region size equal the negotiated stream
size. On this phone the exact request is `{1080, 0, 1280, 720}`.

`kExtend` is a lifecycle label, not a separate public Mir API flag or D-Bus
request. It is the off-primary request plus the fixed system-server socket,
vertical mirror, two-buffer request, and retained producer lifetime. No extra
Lomiri/Aethercast D-Bus display signal was found.

## Mir server trace

UBports Mir 1.8.x implements the decision in
`src/server/compositor/compositing_screencast.cpp`:

```text
capture region
  -> needs_virtual_output(display.configuration(), region)
  -> compare with bounding rectangle of connected outputs
  -> display.create_virtual_output(region.width, region.height)
  -> ScreencastSessionContext owns VirtualOutput
  -> VirtualOutput::enable()
  -> Android display configuration / hotplug propagation
```

`needs_virtual_output()` returns true only when the requested region has no
intersection with the bounding rectangle of connected outputs. For the
`1080x2280+0+0` panel, `{1080,0,1280,720}` is outside the panel rectangle.
`VirtualOutput::enable()` calls `set_virtual_output_to(1280,720)` and hotplug
propagation presents output 3 at `+1080+0`.

The checked UBports source branch is Mir 1.8.1; the installed package is the
later 1.8.3 UBports rebuild. The observed 1.8.3 behavior matches this exact
decision/lifetime: output 3 was connected/used only while the screencast was
retained, and disconnected on release.

## Why V0.2 did not reproduce Aethercast

V0.2 did request the relevant off-primary rectangle, but it targeted the
Lomiri session server rather than Aethercast's `/run/mir_socket` host server.
It also omitted Aethercast's vertical-mirror/two-buffer fields and client
identity. It therefore exercised a different display configuration and could
only report that session server's disconnected `virt` output. Its static
off-primary image was expected for that non-rendered session region.

## Minimal probe

`mirgud` now has an explicit source-only extend mode:

```bash
mirgud --source-mode extend --mir-socket-file /run/mir_socket \
  --size 1280 720 --no-gud --extend-hold
```

It refuses a manual `--capture-region` in extend mode, calculates the region
from the selected primary output, uses Aethercast's client name, vertical
mirror, and two buffers, and logs topology before/after creation.
`--extend-hold` keeps the exact screencast lifetime without consuming buffers;
it is the safe first/second gate. Omitting it reads/releases raw frames.

No Mir server or Android graphics module was rebuilt or deployed. The existing
platform already has virtual-output diagnostics, and changing it again would
reintroduce the prior LightDM restart-loop risk.

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

Lomiri logged `Output 3: Virtual connected, used`, created
`PlatformScreen(output=3, geometry=QRect(1080,0 1280x720))`, and selected a
`root width: 1280 height: 720` external layout. On clean probe termination,
output 3 returned to disconnected.

The raw source run received 127 completed `1280x720`, format-1, stride-5120
frames at about 25 FPS with no conversion failures and a stable 14 client FDs.
Its fingerprint did not change because the greeter content was static. The
initial CPU mapping cache was invalid after virtual-screen reconfiguration;
refreshing `MirGraphicsRegion` per acquired buffer fixed that. A SIGTERM issued
while synchronous buffer release was blocked did not immediately reap the raw
probe, so its known PID was force-terminated after the server had disabled
output 3. The hold mode exits cleanly and is the recommended lifecycle probe.

No GUD driver, Pi gadget, USB transport, H.264/OMX, HWC, synthetic physical
output, or aspect-ratio scaler was touched.
