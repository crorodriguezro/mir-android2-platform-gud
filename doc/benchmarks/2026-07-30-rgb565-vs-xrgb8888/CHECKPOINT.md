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
