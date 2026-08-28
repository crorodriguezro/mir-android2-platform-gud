# 1080p Mir -> Venus H.264 -> Pi optimization report

Date: 2026-08-28 UTC

Base commit: `ffe40e2cc593e1d2afcec28163d9d22196f2bf8a`

Production path: public Mir ABGR8888 -> optimized BT.709 limited NV12 -> Venus H.264 -> framed TCP -> Pi V4L2 decode -> bounded latest-frame VC4 presenter

## Outcome

Three working optimizations were retained:

1. AArch64 NEON ABGR-to-NV12 with an exact scalar fallback.
2. Independent phone Venus-completion/TCP drain with four preallocated bounded buffers.
3. Event-driven Pi receive/decode plus a one-in-flight/one-newest-pending presentation worker.

Direct Mir RGB to Venus was fully rejected: RGB4 accepts configuration and produces H.264, but live Mir and all four plausible 32-bit byte layouts decode corruptly. Production therefore uses optimized NV12.

The combined triplicate sustained 29.975-29.982 source/encode/decode FPS and 29.039-29.821 presentation submits/s. Every sent frame arrived and decoded. Post-decode replacement fell to 0.524-3.143% from 14.154-25.538%, with bounded queues and no throttling.

## Benchmark interpretation

Baseline rows are the preserved three 3900-frame, 151.8-157.1 s runs. Combined rows are three 2100-frame, about 70 s runs; the first seconds include natural startup rather than a separately discarded warmup. Converter figures are deterministic 120-frame microbenchmarks, which the task permits for converter exploration. Direct-RGB and presenter isolation runs were short qualification experiments; only baseline and combined-best are used for sustained production claims.

The NEON microbenchmark improved converter p50 from 12.758 to 3.957 ms (3.22x) and p95 from 17.622 to 8.730 ms (2.02x). The older baseline converter was 32.163-32.692 ms p50. In the live public-Mir path, mapped-buffer reads and scheduling dominate, so combined conversion is 26.911-28.335 ms p50 even with the faster arithmetic. Sender throughput still rises from a 25.05 FPS baseline median to 29.978 FPS.

The phone standalone CPU run measured 71.70% of one logical CPU for the process, 21.37% aggregate SoC busy, 69.36% main thread, and 0.69% completion thread. Baseline's correctly sampled run was 87.6% of one logical CPU and 17.1% aggregate; the differing aggregate sample means only the process reduction should be treated as a direct improvement. Phone temperature was 44.6 C. Pi receiver CPU was 2.15-2.27% of one core, temperature 45.1-46.2 C, and `get_throttled=0x0`.

## Direct answers

1. **Which formats can Mir actually produce?** ABGR8888, XBGR8888, ARGB8888, XRGB8888, RGB888, and RGB565 at 1920x1080. BGR888 is rejected. ARGB/XRGB work although not advertised; no accepted request was substituted.
2. **Which formats can Venus actually encode?** NV12, Q128/NV12-UBWC, RGB4, NV21, Q12A/TP10-UBWC, and QP10/P10-Venus. Relevant alternate packed RGB/RGBA, RGB888/BGR888, RGB565, and NV12M requests are rejected.
3. **Is there a direct Mir->Venus compatible format?** No qualified one. RGB4 is the only plausible overlap, but every exact Mir layout decoded corruptly.
4. **Which exact format was selected?** Mir ABGR8888, real stride, NEON/scalar BT.709-limited conversion, Venus single-plane NV12.
5. **Can RGB->NV12 be eliminated?** **NO.**
6. **Direct-RGB sender FPS/CPU/latency?** Not applicable as a production result; the path failed correctness and was not promoted to a sustained benchmark.
7. **Why not?** Venus RGB4's advertised name did not define a usable byte contract. ABGR, XBGR, ARGB, and XRGB trials produced the same corrupt decoded result, while Venus rejects Mir's 24-bit and RGB565 alternatives.
8. **Scalar RGB->NV12 p50/p95?** 12.758/17.622 ms in the controlled 1080p converter microbenchmark.
9. **Optimized RGB->NV12 p50/p95?** 3.957/8.730 ms in the same microbenchmark; live public-Mir combined runs were 26.911-28.335/42.771-44.252 ms because source-buffer access/scheduling dominates.
10. **Converter speedup?** 3.22x at p50 and 2.02x at p95; output is byte-exact to scalar.
11. **Baseline sender FPS?** 24.82, 25.05, 25.68; median 25.05 FPS.
12. **Optimized sender FPS?** 29.975-29.982; median 29.978 FPS in combined production.
13. **What caused the previous ~25-FPS phone ceiling?** A near-one-core serialized capture/conversion path, about 32 ms scalar conversion, and waiting in the same path for Venus completions. Venus's ~85 ms per-frame latency was pipelined and was not a 12-FPS throughput ceiling.
14. **Baseline decode FPS?** 24.81-25.68 FPS with 100% decoder retention.
15. **Baseline DRM presentation FPS?** 18.65-22.05 submits/s.
16. **New DRM presentation FPS?** 29.039, 29.821, 29.461; median 29.461 submits/s.
17. **Baseline decoded replacement percent?** 24.667%, 25.538%, 14.154%; median 24.667%.
18. **New decoded replacement percent?** 3.143%, 0.524%, 1.714%; median 1.714%.
19. **Did the new presenter improve mouse temporal smoothness without increasing latency?** **YES for temporal update cadence and queue behavior:** updates rose to about 29.5/s and no FIFO can accumulate. Physical latency was not measured; the unsynchronized relative-age proxy varied with decoder jitter, so an input-to-photon improvement is not claimed.
20. **Did sender pipeline decoupling help?** **YES.** Venus completion and TCP no longer block capture/conversion/QBUF; combined runs had zero encoder-buffer waits and 29.98 FPS, while Venus latency fell to about 46.5-46.8 ms p50.
21. **Best Venus in-flight depth?** Four allocated slots. Observed occupancy was 3-4 with zero waits; four is the smallest configuration directly proven across all retained full runs.
22. **Combined-best source FPS?** 29.978 FPS median (29.975-29.982).
23. **Combined-best decode FPS?** 29.978 FPS median; 2100/2100 completed in every run.
24. **Combined-best presentation FPS?** 29.461 FPS median (29.039-29.821).
25. **Combined-best phone CPU?** 71.70% of one logical CPU for the sender process; 21.37% aggregate SoC busy in the standalone full-pipeline CPU run.
26. **Combined-best frame-age p50/p95?** Median-of-runs 98.078/422.364 ms; ranges 44.152-204.971/174.367-452.886 ms. This is an independent-clock relative-start proxy, not absolute input-to-photon latency.
27. **Current remaining dominant bottleneck?** Public-Mir mapped RGB buffer access plus conversion/scheduling on the phone; Pi decoder completion jitter is the main receiver-side variability. Transport retains every frame.
28. **Next optimization?** Preserve color quality while reducing source-memory traffic: prototype a parallel/prefetched ABGR converter or a GPU/hardware color-conversion path into Venus-compatible NV12, then qualify it against the scalar reference. Mir RGB565 could reduce bandwidth but is not preferred because of quality loss.

## Counters and timing detail

Each combined run dequeued, converted, Venus-QBUF'd, Venus-DQBUF'd, and sent 2100 frames. Each Pi run received, decoder-submitted, and decoder-completed 2100. Presented/replaced were 2034/66, 2089/11, and 2064/36. Source-pending replacements and encoder-buffer waits were zero. Maximum phone in-flight was 4, 3, and 4; maximum Pi state was always one in-flight and one pending.

Combined Venus p50/p95 was 46.489-46.799/65.317-68.895 ms; TCP send was 0.066-0.067/0.460-0.582 ms. Pi decode was 25.968-29.731/73.613-124.761 ms and DRM submit was 10.439-12.106/16.599-16.611 ms. No page-flip/vblank completion timestamp is available: receiver completion means its dedicated SETPLANE call returned.

## Qualification and limitations

The normal live Lomiri scene remained visually functional with the same known crop/orientation defect documented before this optimization. `xdisp.service` remained inactive. The receiver does not restore a GUD framebuffer and now disables/removes/closes only its exact H.264 DRM resources on teardown; the 30-frame regression left no H.264-owned framebuffer and 204124 kB CMA free.

The implementation meets the no-backlog invariants, but it does not add a phone source-pending worker because the capture/conversion path already stays at target rate and adding another full-frame staging copy regressed conversion timing. Presentation uses a dedicated blocking SETPLANE worker because this legacy plane path exposes no page-flip completion event in the current implementation.
