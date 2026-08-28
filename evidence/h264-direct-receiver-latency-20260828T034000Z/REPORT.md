OUTCOME: PARTIAL

The direct low-copy receive/display route is physically proven, but the
interactive latency gate is not passed. The evidence does not prove that the
hardware cannot meet the target; it proves that this direct receiver
implementation still has a V4L2/DRM lifecycle failure that must be fixed
before choosing H.264.

ACTUAL FINAL PIPELINE:

Native 1920x1080 H.264 access units
→ USB2 ECM/TCP
→ Pi `/dev/video10` bcm2835-codec H.264 V4L2 M2M
→ native 1920x1088 linear NV12 capture DMABUF
→ `VIDIOC_EXPBUF`
→ DRM PRIME import
→ VC4 plane 84, cropped to 1920x1080
→ HDMI 1920x1080

The direct receiver contains no FFmpeg, VLC, libswscale, CPU YUV conversion,
or playback timestamp queue. The sender framing header carries sequence and
source monotonic timestamp fields.

Measured hardware facts:

- `/dev/video10` accepted H.264 input at 1920x1080 with a 2 MiB input buffer.
- Capture accepted 1920x1088 NV12, one plane, 1920-byte pitch,
  3,133,440-byte image size.
- Four decoder input buffers and six capture buffers were allocated.
- Six capture buffers were exported as DMABUFs and imported into DRM; all
  six `DRM_IOCTL_MODE_ADDFB2` calls succeeded.
- VC4 plane 84 accepted linear NV12 and explicit BT.709/limited-range
  properties.
- The earlier direct run presented 60/60 frames without software conversion.
- That run averaged 8.54 ms per DRM plane commit, and relative displayed
  frame age grew to p50 213 ms and p95 430 ms because every decoded frame was
  presented instead of selecting the newest completed frame.
- A latest-frame policy was implemented to drain decoded buffers and present
  only the newest one. A current burst run decoded 30/30 and presented 28;
  its relative-age p50 was -204.857 ms and p95 28.506 ms, which is not a
  physical latency measurement because the source was replayed as a burst.
- A current paced 30-FPS run caused the Pi to reboot while the direct receiver
  owned the V4L2/DRM resources. No valid paced latency distribution was
  collected. This is a receiver stability failure, not evidence that the
  decoder or VC4 cannot perform the route.

QUALITY:

Native H.264 color was independently verified in the preceding quality
evidence. BT.709 limited-range conversion is correct, but Venus emits no
useful VUI color metadata. Small colored text remains subject to 4:2:0 chroma
loss. The direct VC4 plane was explicitly configured for BT.709 limited range.

LATENCY:

No valid input-to-photon p50/p95 was obtained. No physical HID/camera
measurement was available. The pre-latest-frame relative-age result fails the
target, while the latest-frame result was only a burst sanity check. The
latest-frame implementation must be rerun after fixing the decoder drain and
cleanup lifecycle.

DECISION:

KEEP H.264 as an experimental option, not the primary desktop architecture
yet. Do not select GUD merely because the old RGB565 conversion receiver was
slow; select GUD provisionally because H.264 still lacks a passing, stable
direct receiver latency result and has intrinsic colored-text artifacts.

Production restoration was verified after the Pi reboot: the phone reported
host mode, dynamically enumerated GUD `1d50:614d` at 480 Mb/s, and retained
`/dev/dri/card1`; the Pi reported `gud-userspace.service=active`, UDC
`configured`, GUD owner `/home/cristian/gud-drm-e4-t03-cdf83d37`, and VC4
plane 84 on CRTC 95 with framebuffer 673. A bounded `gud-kms-fill` smoke run
also emitted fresh XDISP frame updates on the phone; it was stopped by the
intentional timeout (rc=124).
