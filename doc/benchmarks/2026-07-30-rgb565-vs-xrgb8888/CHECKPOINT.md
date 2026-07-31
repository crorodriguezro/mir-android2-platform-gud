# XDISP Pixel-Format Benchmark Checkpoint

## 2026-07-31 Transport Interpretation Correction

The prior transport-performance interpretation is superseded for these
preserved runs:

- `raw/xdisp-xrgb8888-drop-classification-20260730-230005/`
- `raw/xdisp-rgb565-native-e2e-20260730-234255/`

They are functionally valid but performance contaminated by synchronous Pi
diagnostic dumping. After every payload, the Pi FunctionFS event loop wrote a
PPM and raw framebuffer dump before accepting the next `SET_BUFFER`. The phone
therefore blocked in `usb_control_msg(GUD_REQ_SET_BUFFER)` while that event loop
was unavailable. The apparent roughly 500-ms per-payload delay was diagnostic
interference, not inherent GUD or USB latency.

The preserved evidence remains authoritative for:

- Mir source-format negotiation and conversion-path contracts.
- Visual correctness.
- Actual payload cap compliance at or below 12,800 bytes.
- Absence of short reads and poisoned receiver state.
- Identification of synchronous diagnostic-dump interference.

Exclude submitted/presented rate, drop rate, `set_buffer` latency, transfer
latency, and effective throughput from format-performance aggregates for these
runs. Do not delete or rewrite their original logs.

## 2026-07-31 Native-Mode Requalification

Do not use performance values from runs that had either synchronous per-payload
framebuffer dumps or a missing `GUD_TEST_OUTPUT_MODE=1280x720`. The latter
caused Pi fallback to 1920x1080 and approximately 208--215 ms of `scale_ms` per
rectangle. Those runs remain evidence for root-cause investigation, protocol
behavior, and recovery procedures.

The following native-1280x720, dump-disabled retries are authoritative for the
corresponding 1-FPS gate. They retain `scale_ms=0`, actual payloads at or below
12,800 bytes, no short reads, no poison, and no marker-bounded `-110`.

- `raw/xdisp-rgb565-native-clean-live-retry-20260731-054315/`: Mir RGB565 to
  GUD RGB565 direct copy; 63 submitted and presented with zero drops.
- `raw/xdisp-xrgb8888-abgr-clean-live-20260731-054611/`: Mir ABGR8888 to GUD
  XRGB8888 channel reorder; 63 submitted and presented with zero drops.
- `raw/xdisp-xrgb8888-native-control-20260731-054820/`: pregenerated XRGB8888
  transport control, not a native Mir XRGB8888 source; 71 submitted and
  presented with zero drops.

The earlier `raw/xdisp-xrgb8888-clean-control-retry-20260731-052656/` and
`raw/xdisp-rgb565-native-clean-live-20260731-052845/` ran after a format switch
that omitted the native output override. Exclude their performance values.
