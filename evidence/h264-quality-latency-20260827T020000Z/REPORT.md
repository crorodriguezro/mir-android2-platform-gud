OUTCOME: partial — native 1080p and color path corrected; low-latency display path not yet acceptable

FINAL PIPELINE TESTED

Mir ABGR8888 1920x1080
→ CPU BT.709 limited-range NV12 conversion (2x2 chroma averaging)
→ Qualcomm Venus H.264 1920x1080 at 30 FPS target, no B-frames
→ MPEG-TS over USB2 ECM/TCP
→ Pi bcm2835-codec H.264 V4L2 M2M
→ FFmpeg raw yuv420p→RGB565 conversion
→ VC4 DRM dumb framebuffer
→ 1920x1080 HDMI

The sender and decoder were exercised on the physical OnePlus 6 → Pi Zero 2 W
link. The raw-RGB565 receiver is diagnostic only: Pi FFmpeg reports that no
accelerated yuv420p→rgb565le conversion is available, so it is not a viable
final receiver architecture.

MEASURED RESULTS

| Item | Result |
| --- | --- |
| Resolution | 1920x1080 end-to-end; no 720p scaler |
| Source / encoder input | Mir ABGR8888 → Venus NV12 |
| Venus formats | NV12, Q128, RGB4, NV21, Q12A, QP10 advertised; NV12 accepted |
| Accepted color controls | colorspace=REC709 (3), ycbcr_enc=709 (2), quantization=limited (2), xfer_func=709 (1) |
| H.264 metadata | FFprobe reports color range/space/primaries/transfer unknown; Venus did not emit VUI from the accepted V4L2 format fields |
| Sender queue | four ION-backed USERPTR buffers; maximum in-flight observed 4 |
| Sender cadence | 23.9–24.7 FPS in corrected NV12 live runs; conversion 34–35 ms/frame, Mir swap 4–5 ms/frame |
| Receiver decode/presentation | 90/90 frames reached the native 1080p sink, but raw RGB565 path ran at 9.8 FPS |
| Receiver errors | zero in the corrected MPEG-TS/probe path; earlier failures were FFmpeg `nobuffer`/probe configuration defects |
| Bitrate controls | 10, 15, 25, and 40 Mb/s controls accepted; static desktop output stayed approximately 0.03–0.05 Mb/s |
| 40 Mb/s pattern trial | stalled at frame 23; no completed result, and the exact stall point was not isolated (the known 2 MiB Venus capture ceiling remains a candidate) |
| Physical USB | high-speed 480 Mb/s; prior validated ECM payload capacity far exceeds these streams |
| Displayed-frame age | not measured; no sequence/timestamp telemetry exists in this receiver |
| Input-to-photon latency | not measured; no high-speed-camera/HID rig used |

COLOR FINDINGS

The deterministic pattern was encoded through the same RGB→NV12 path and
decoded at 1920x1080. With explicit BT.709 limited→RGB conversion, representative
decoded pixels were black `(0,0,0)`, white `(255,255,253)`, gray `(128,128,128)`,
red `(255,0,0)`, green `(0,254,0)`, blue `(1,0,255)`, cyan `(0,254,253)`,
magenta `(255,0,255)`, and yellow `(254,255,0)`.

Using FFmpeg’s default matrix on the same decoded samples shifted red to about
`(233,0,2)`, green to `(19,255,6)`, cyan to `(21,255,250)`, magenta to
`(236,0,246)`, and yellow to `(252,255,8)`. The prior washed/mixed colors are
therefore a fixable BT.601-vs-BT.709 receiver mismatch, compounded by missing
H.264 VUI metadata. The receiver should use the `h264_metadata` bitstream filter
or equivalent decoder configuration: BT.709, limited range, progressive.

The pattern’s one-pixel alternating red/blue section remains chroma-averaged by
4:2:0. That loss is intrinsic to the selected H.264 format; it cannot be
recovered by raising bitrate. Tiny colored desktop text is therefore expected
to remain softer than RGB565 even after the implementation mismatch is fixed.

LATENCY FINDINGS

The previous 500 ms–1 s delay cannot be assigned to USB: the physical link is
fast enough and the tested sender queue is bounded at four frames. A separate
receiver defect was found: FFmpeg’s `-fflags nobuffer`/`-avioflags direct`
combination prevents live MPEG-TS pipe startup, while `probesize=2048` can
misidentify the stream. The compatible diagnostic settings are `-flags
low_delay -probesize 32768 -analyzeduration 0 -map 0:v:0`.

Even with that fixed, the raw RGB565 receiver introduces an unaccelerated CPU
colorspace conversion and falls to 9.8 FPS, so its pipe necessarily accumulates
old frames. This is a fixable prototype bottleneck, not evidence that Venus or
the Pi decoder is too slow. The final receiver must preserve decoder NV12/DRM
PRIME surfaces (as the prior VLC `drm_prime:yuv420p` run did) or use a direct
V4L2-to-DRM PRIME implementation; it must not convert every frame on the ARM
CPU.

GUD COMPARISON

GUD is better at lossless RGB565 desktop text, exact color, and the already
validated bounded display path. H.264 is better at bandwidth for photographic
or high-entropy content, but that advantage was not enough to offset the
unaccelerated receiver conversion in this run. Existing GUD measurements on
the same hardware report approximately 20 FPS at native 1080p with exact
RGB565/LZ4 presentation.

DECISION

For the current project, focus on GUD. H.264 remains a credible optional path
only after a direct DRM PRIME receiver, VUI/color handling, frame-age telemetry,
and a real interactive latency measurement are implemented. The evidence here
classifies the original color issue as fixable, 720p scaling as fixable, 4:2:0
colored-edge loss as codec-intrinsic, and the observed receiver backlog as a
fixable prototype architecture issue. It does not justify PASS-QL or PASS-L.
