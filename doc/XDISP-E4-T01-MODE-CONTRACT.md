# E4-T01 end-to-end mode contract

E4-T01 correlates the selected host timing, Mir source raster, committed GUD
state, and Pi physical scanout without changing the GUD protocol. Both processes
derive `mode_contract_id=e4c1-<16 hex digits>` using 64-bit FNV-1a over this
canonical little-endian schema:

1. schema version `u8` (`1`)
2. GUD connector index `u8`
3. GUD pixel format `u8`
4. `clock` as `u32`
5. `hdisplay`, `hsync_start`, `hsync_end`, `htotal` as `u16`
6. `vdisplay`, `vsync_start`, `vsync_end`, `vtotal` as `u16`
7. GUD user-visible mode flags (`flags & 0x000033ff`) as `u32`

The identifier is a mode-contract fingerprint, not a transport transaction ID.
The Pi's state generation and the process/journal timestamps distinguish repeated
commits of the same contract.

## Required records

The managed phone child emits:

- `host_mode_selected`: exact timing, connector, and wire pixel format;
- `mir_source_ready`: logical/source size, stride, Mir format, row order,
  transport format, and conversion path.

The Pi emits:

- `e4_mode_contract_gud_commit`: exact state accepted at STATE_COMMIT;
- `e4_mode_contract_physical_route` for fixed matching, or
  `mode_commit_decision` for dynamic matching, including physical route, size,
  pitch, and allocation data.

Validate one activation's captured logs with:

```sh
python3 tools/xdisp-e4-mode-contract-verify.py PHONE.log PI.log \
    --contract-id e4c1-0123456789abcdef
```

The verifier rejects missing stages, missing required fields, and ambiguous IDs.
The shared test vector is `e4c1-2c5242af7c0e3ebe` for a 1280x720 RGB565 timing;
both the C++ and Rust suites assert it independently.

## Qualification status

**E4-T01 PASS.** On 2026-08-25 one connected activation produced matching phone
and Pi records for `mode_contract_id=e4c1-2c5242af7c0e3ebe`; the verifier passed,
live RGB565+LZ4 frames completed without receiver failures, and the operator
confirmed the external desktop was visibly correct. Evidence is in
`evidence/xdisp-e4-t01-mode-contract-20260825T052302Z/`.

This result does not override E3-B01's strict reconnect failure: the independent
OnePlus host-controller issue required the documented role cycle before this E4
session. E4-T01 does not by itself qualify E4-T02 or E4-T05.
