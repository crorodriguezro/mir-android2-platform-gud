# E4-T01 physical mode-contract capture

Captured on 2026-08-25 from the connected OnePlus 6 host and Raspberry Pi GUD
gadget during one managed direct-RGB565 activation.

- Phone commit: `11fa003de3265b3955f6aea1cd0bafa3d3f2474d`
- Phone deployed MirGUD SHA-256:
  `0051a0b304c6bcfa842f4c1396024f5a0600dcb6ba3add46ab8beef94c437748`
- Pi commit: `86c884e457f905b34ddd7c52d3635593a3486b5c`
- Pi deployed gud-drm SHA-256:
  `b64febf93b65766100ab809b209f30b85b5ddd2034a161859142ea59a4de9637`
- Pi service PID: `2426`
- Managed MirGUD child PID: `88805`
- USB identity/path/speed: `1d50:614d`, `1-1.3`, 480 Mbit/s
- Common contract ID: `e4c1-2c5242af7c0e3ebe`
- Operator visual confirmation: **PASS**; the external desktop remained visibly
  correct after the E4 binaries were deployed and activated.
- Final sampled frame: payload sequence 1020, RGB565, LZ4 compression, no
  scaling, 1,062,783 transfer bytes to 1,843,200 scanout bytes.
- Final sampled lifecycle counters: 1,025 accepted/completed AIO transactions,
  zero processing failures, zero poisoned transactions, zero timeouts, receiver
  and exact AIO both Idle between frames.

The correlated records prove the following single contract end to end:

- selected host timing: 1280x720 at a 74250 kHz pixel clock, totals 1650x750,
  flags 0x5;
- Mir source: 1280x720, stride 2560, format 7, top-down;
- transport: direct-copy RGB565 (GUD format 0x40);
- Pi committed GUD state: the identical timing, format, connector, and flags;
- physical route: fixed 1280x720 scanout, pitch 2560, two 1,843,200-byte
  mappings.

Verifier command:

```sh
python3 tools/xdisp-e4-mode-contract-verify.py \
  evidence/xdisp-e4-t01-mode-contract-20260825T052302Z/phone.log \
  evidence/xdisp-e4-t01-mode-contract-20260825T052302Z/pi.log \
  --contract-id e4c1-2c5242af7c0e3ebe
```

Verifier result:

```text
PASS mode_contract_id=e4c1-2c5242af7c0e3ebe physical_event=e4_mode_contract_physical_route
```

## Verdict

**E4-T01 PASS.** The host timing, Mir source raster, GUD wire format and timing,
and physical Pi scanout were correlated under one deterministic contract ID,
the live transport remained healthy, and the operator confirmed the resulting
external desktop.
