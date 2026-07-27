# XDISP-P0.2 GUD Presentation Test Procedure

This is the owning-component procedure for the canonical P0.2 design and
plan in `../gud/docs/superpowers/`. The experimental platform remains rolled
back from the normal phone configuration until this procedure has retained its
acceptance evidence.

## Offline gate

Build a commit-qualified plugin in a supported Mir/Android2 build environment,
then run the focused worker test:

```bash
cmake -S . -B build-p0.2 -DMIR_ENABLE_TESTS=ON -DMIR_BUILD_UNIT_TESTS=ON
cmake --build build-p0.2 --target mir_unit_tests_android2
ctest --test-dir build-p0.2 --output-on-failure -R GudPresentationWorker
```

Record the exact commit, build command, compiler, test output, and deployed
plugin hash. Do not deploy an uncommitted build. A source-level worker check is
not a substitute for this gate.

## Hardware session gate

Do not start Lomiri, load a test module, or diagnose KMS until the mandatory
OnePlus gate in
`../gud/docs/oneplus6-usb-host-gud-troubleshooting.md` prints `FOUND:` for
`1d50:614d`. It forces the controller to `host` and polls every dynamic USB
topology; run it verbatim immediately before the test.

Use the existing normal `/home/phablet/gud.ko` unchanged. If a separately
named diagnostic module is needed for the already-verified 12,800-byte payload
ceiling, use a commit-qualified artifact and record its hash; never overwrite
the normal module. Do not modify either kernel or perform P2 work.

Before a transfer, inspect the Pi service journal and UDC state. If a receive
session is `Poisoned`, incomplete, or otherwise in the documented unsafe
state, do **not** stop, restart, reboot, shut down, or retry the Pi service.
Preserve the logs and use only the physical recovery path from the P0.1
procedure.

## Evidence matrix

Create an ignored evidence directory under
`gud/backport-4.9/env/local/evidence/xdisp-p0.2-<timestamp>/` and retain:

| Case | Action | Required observation |
| --- | --- | --- |
| Slow output | Run a changing external desktop while the known bounded GUD path drains frames slowly. | Phone touch/input and internal display remain responsive; worker logs show coalescing/drop behavior rather than a compositor stall. |
| Absent at startup | Launch with no accessible GUD card. | No synthetic GUD output/worker is started and the internal display stays usable. |
| I/O error | From a clean, non-poisoned session, physically detach the Pi during an active external update. | Mir logs a contained GUD frame failure; phone UI remains responsive; preserve dmesg, Pi journal, and UDC state without an unsafe service action. |
| Reappearance | Reattach only after the hardware is in a known-safe state. | Record whether the still-existing synthetic output reacquires GUD; a missing output remove/add notification is P0.3 evidence, not a P0.2 failure workaround. |
| Shutdown | Stop the experimental Mir session only after the active transfer has returned. | Pending frame is dropped, worker joins cleanly, and no use-after-free/deadlock record appears. |

Use dynamic DRM discovery in the logs; do not assert `card1`. Keep P0.2
**in progress** until every row that applies has the required evidence.
