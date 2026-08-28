# H.264 live-display bottleneck and latency characterization

Date: 2026-08-28  
Path: OnePlus 6 Mir external screencast → Venus H.264 → framed TCP/Wi-Fi → Pi `/dev/video10` → NV12 DMABUF/DRM PRIME → VC4 plane 84/CRTC 95

## Scope and method

Three fixed 3,900-frame live repetitions were run with the existing H.264 path. The runs lasted 151.841–157.145 s, therefore each exceeded the requested 120 s measured duration. The counters below cover the complete bounded run; a separate 10 s warm-up discard was not applied. No GUD restoration, `xdisp.service`, RGB conversion optimization, Venus format change, bitrate change, NEON conversion, DRM redesign, or 1080→1088 crop/orientation change was performed.

The receiver ran with `H264_RECEIVER_NO_RESTORE=1`, leaving the direct H.264 plane selected. Pi presentation completion/vblank was not instrumented, so “presented” means DRM `SETPLANE` submitted, not physically scanned out.

## Results

| Metric | Run 1 | Run 2 | Run 3 |
|---|---:|---:|---:|
| Phone wall time / source FPS | 157.145 s / 24.82 | 155.679 s / 25.05 | 151.841 s / 25.68 |
| Mir source / encoder submit / encoder output / transport send FPS | 24.82 / 24.82 / 24.82 / 24.82 | 25.05 / 25.05 / 25.05 / 25.05 | 25.68 / 25.68 / 25.68 / 25.68 |
| Pi receive / decoder submit / decoder output FPS | 24.82 / 24.82 / 24.81 | 25.05 / 25.05 / 25.04 | 25.68 / 25.68 / 25.68 |
| Pi presentation-submit FPS | 18.70 | 18.65 | 22.05 |
| Pi presentation-complete/vblank FPS | unavailable | unavailable | unavailable |
| Phone encoded / sent | 3,900 / 3,900 | 3,900 / 3,900 | 3,900 / 3,900 |
| Phone RGB→NV12 p50 / p95 | 32.692 / 33.932 ms | 32.566 / 33.883 ms | 32.163 / 32.506 ms |
| Phone Venus encode p50 / p95 | 85.879 / 90.109 ms | 85.140 / 89.562 ms | 83.337 / 85.166 ms |
| Phone transport send p50 / p95 | 0.254 / 0.381 ms | 0.243 / 0.363 ms | 0.249 / 0.298 ms |
| Pi received / decoder completed | 3,900 / 3,900 | 3,900 / 3,900 | 3,900 / 3,900 |
| Pi DRM plane submits | 2,938 | 2,904 | 3,348 |
| Pi decoded outputs replaced | 962 | 996 | 552 |
| Pi decode p50 / p95 | 46.925 / 163.564 ms | 47.762 / 149.517 ms | 41.204 / 85.026 ms |
| Pi DRM submit p50 / p95 | 8.744 / 16.322 ms | 8.877 / 16.423 ms | 8.346 / 16.031 ms |
| Pi receive→plane-submit p50 / p95 | 51.814 / 121.454 ms | 52.055 / 128.826 ms | 49.211 / 83.252 ms |
| Relative age p50 / p95 | 34.635 / 304.537 ms | 124.840 / 355.273 ms | 52.810 / 106.125 ms |

## Full timed-stage statistics

Values are `count / mean / p50 / p95 / max`; phone and Pi durations are milliseconds. Frame-age values are milliseconds.

| Stage | Run 1 | Run 2 | Run 3 |
|---|---:|---:|---:|
| RGB→NV12 | 3900 / 32.882 / 32.692 / 33.932 / 91.801 | 3900 / 32.755 / 32.566 / 33.883 / 88.618 | 3900 / 32.243 / 32.163 / 32.506 / 87.877 |
| Venus encode | 3900 / 81.962 / 85.879 / 90.109 / 112.335 | 3900 / 81.720 / 85.140 / 89.562 / 195.274 | 3900 / 82.490 / 83.337 / 85.166 / 91.076 |
| Transport send | 3900 / 0.333 / 0.254 / 0.381 / 4.534 | 3900 / 0.352 / 0.243 / 0.363 / 149.776 | 3900 / 0.240 / 0.249 / 0.298 / 0.509 |
| Phone total | 3900 / 115.184 / 119.033 / 123.363 / 191.050 | 3900 / 114.835 / 118.203 / 123.097 / 266.035 | 3900 / 114.981 / 115.834 / 117.685 / 123.357 |
| Receive→decode submit | 2937 / 0.001 / 0.0004 / 0.0008 / 0.049 | 2903 / 0.001 / 0.0004 / 0.0007 / 0.190 | 3347 / 0.0004 / 0.0004 / 0.0007 / 0.023 |
| Pi decode | 3899 / 65.917 / 46.925 / 163.564 / 520.816 | 3899 / 66.045 / 47.762 / 149.517 / 446.974 | 3899 / 48.339 / 41.204 / 85.026 / 287.659 |
| Decode→DRM | 2937 / 0.020 / 0.006 / 0.079 / 0.242 | 2903 / 0.020 / 0.006 / 0.079 / 0.177 | 3347 / 0.014 / 0.006 / 0.077 / 0.144 |
| DRM submit | 2937 / 8.784 / 8.744 / 16.322 / 47.757 | 2903 / 8.965 / 8.877 / 16.423 / 51.435 | 3347 / 8.478 / 8.346 / 16.031 / 42.130 |
| Pi receive→presentation submit | 2937 / 62.846 / 51.814 / 121.454 / 524.866 | 2903 / 63.177 / 52.055 / 128.826 / 382.126 | 3347 / 53.199 / 49.211 / 83.252 / 298.419 |
| Relative frame age | 128 / n/a / 34.635 / 304.537 / 393.122 | 128 / n/a / 124.840 / 355.273 / 432.360 | 128 / n/a / 52.810 / 106.125 / 228.579 |

Run 3 produced only 0.04 Mb/s because the screen was comparatively static; this changes compressed byte volume, not the all-frame delivery result. The first Run 3 launch was excluded after a DRM-owner permission failure; the retry above is the valid repetition.

## Bottleneck attribution

1. The phone is the first hard rate limiter. It produces 24.82–25.68 FPS rather than the requested 30 FPS. Mir capture and Venus completion are serialized by bounded in-flight work (`max_in_flight=3`). RGB→NV12 alone consumes about 32.2–32.7 ms p50 per frame, while Venus submit→output is about 83.3–85.9 ms p50. These overlap asynchronously, so the sum is not a latency calculation, but they explain the source-side backpressure.
2. This is a single-worker bottleneck despite aggregate phone CPU headroom: the measured Run 2 sender process used about 87.6% of one logical CPU while aggregate phone busy was about 17.1%. The current measurement does not split that process into separate conversion and encoder CPU threads.
3. Transport is not the bottleneck. Every run sent and received all 3,900 framed access units; p50 TCP send time was 0.243–0.254 ms. There was no observed network queue/drop/error symptom.
4. Pi decoding is not losing frames: all 3,900 submitted access units completed in every run. The measured decoder interval from output `QBUF` to capture `DQBUF` was the largest Pi timing stage (p50 41.2–47.8 ms; p95 85.0–163.6 ms), with burst/jitter.
5. The receiver intentionally keeps only the newest decoded buffer before each DRM submit. Consequently 552–996 decoded outputs were replaced, and the plane was submitted at approximately 18.7, 18.7, and 22.0 updates/s. This is the direct cause of much of the visible mouse choppiness: decoded frames are available, but intermediate frames are coalesced and DRM submits have roughly 8.3–8.9 ms p50 with 16.0–16.4 ms p95.
6. The measured 30→25 FPS loss is therefore phone-side production/backpressure, not transport loss. The further 25→19–22 presentation-update rate is receiver freshness coalescing plus decoder/DRM scheduling, not decoder frame loss.

## Answers to the requested questions

- Sustained source rate: 24.82–25.68 FPS; median across runs is about 25.05 FPS.
- Phone RGB→NV12: 32.2–32.7 ms p50, about 32.5–33.9 ms p95.
- Venus encode interval: 83.3–85.9 ms p50, about 85.2–90.1 ms p95.
- Transport: approximately 0.25 ms p50, and all frames delivered.
- Pi decode interval: 41.2–47.8 ms p50; p95 85.0–163.6 ms.
- Pi DRM submit: 8.3–8.9 ms p50; p95 16.0–16.4 ms.
- Replaced/skipped decoded outputs: 552–996 per 3,900-frame run, intentional latest-frame policy.
- Physical display latency: unavailable because no vblank/page-flip completion timestamp was captured.
- Relative frame age: p50 34.6–124.8 ms and p95 106.1–355.3 ms. This is only the existing relative-start-offset proxy: phone and Pi `CLOCK_MONOTONIC` domains are independent, so it is not absolute input-to-photon latency.
- USB/network bottleneck: no. The measured framed TCP/Wi-Fi path delivered 100% of access units.
- Pi CPU/thermal safety: no throttling (`0x0`); observed temperatures 44.5–47.2°C; GUD service remained active.
- GUD safety: preserved. `xdisp.service` remained inactive, and the benchmark never restored FB 673 or started xdisp.
- Upside-down image: separate orientation bug, not a measured performance bottleneck. It remains intentionally unfixed for this benchmark.
- Single next optimization to test: Venus `RGB4` input to remove the full-frame CPU RGB→NV12 conversion, while validating channel order and visual quality. This is a follow-up experiment, not part of this commit.

## Explicit final answers

1. True sender FPS: 24.82–25.68 FPS, approximately 25 FPS.
2. True Pi receive FPS: the same 24.82–25.68 FPS; all 3,900 frames arrived complete.
3. True decoder-output FPS: the same within the measurement window; 3,900/3,900 completed in every run.
4. True presentation FPS: 18.65–22.05 DRM plane submissions/s; physical displayed FPS is unavailable.
5. Missing frames between ~30 and ~20: first at phone production (30 requested vs ~25 produced), then at receiver presentation (decoded frames are coalesced before DRM).
6. Failure or stale drops: transport/decoder losses were failures-free; the 552–996 post-decode replacements were intentional stale-frame coalescing.
7. Largest timed latency stage: Venus encode submit→completion by p50/p95; Pi decode has the largest receiver-side jitter and contributes to coalescing.
8. Aggregate phone CPU saturation: NO; the measured Run 2 aggregate busy sample was 17.1%.
9. One phone thread/core saturated: YES; the Run 2 sender process used about 87.6% of one logical CPU.
10. RGB→NV12 bottleneck: YES as a major phone-side contributor (about 32 ms p50), but not proven to be the sole limiter.
11. Venus encoding bottleneck: YES as part of the phone-side throughput/latency bottleneck (about 83–86 ms p50); it overlaps with conversion.
12. Transport bottleneck: NO; p50 send time was about 0.25 ms and retention was 100%.
13. Pi decoding bottleneck: YES for receiver-side timing/jitter and latency, but NO for frame loss; all submitted frames completed.
14. Pi DRM presentation bottleneck: YES as a secondary visible rate limiter; submit calls were about 8.3–8.9 ms p50 and latest-frame policy replaced many decoded outputs.
15. Mouse feel: presentation updates fall to roughly 19–22/s, giving 45–54 ms between visible updates versus 33 ms at 30 FPS; encode/decode buffering and jitter add delay on top.
16. Next optimization: test Venus `RGB4` input to eliminate CPU RGB→NV12, after validating channel order/quality.

## Instrumentation limitations

Phone “Mir interval” is measured at the instrumented dequeue/copy boundary and includes the effect of downstream backpressure; it is not an independent display-producer clock. Phone encode timing is the V4L2 submit-to-completion interval. Pi decode timing is the decoder output `QBUF` to capture `DQBUF` interval. No physical scanout, vblank, synchronized clocks, or absolute end-to-end timestamp was added.

CPU sampling limitation: the correctly interpreted aggregate/per-process sample was captured during Run 2; Run 1 and Run 3 did not retain a full-run CPU time series. The reported Run 2 sample is sufficient to distinguish aggregate idle from one-worker saturation, but not to claim per-thread attribution inside the sender.
