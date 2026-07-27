# XDISP-P0.2 GUD Presentation Test Procedure

This is the owning-component procedure for the canonical P0.2 design and
plan in `../gud/docs/superpowers/`. The experimental platform remains rolled
back from the normal phone configuration until this procedure has retained its
acceptance evidence.

## Offline gate

On this Fedora/Asahi laptop, use the tracked Ubuntu 24.04 container rather
than trying to mix the phone's Ubuntu Touch Mir 1/libhybris ABI with Fedora
libraries. Before building or deploying, record `/etc/os-release` and the
installed `libmir1*`, `mir1-*`, and Boost package versions from the phone. The
image adds the public `24.04-1.x` UBports archive with its signed
`keyring.gpg`, then installs the matching versioned development packages
(`libmir1platform-dev`, `libmir1core-dev`, and `mir1test-dev`, among others).
Do not substitute the unversioned Noble Mir development packages: they emit
different sonames and cannot be loaded by the phone's Mir 1 compositor.

Prerequisite: a working Docker daemon. Build and test with:

```bash
./tools/xdisp-p0.2-build.sh
```

The source is mounted read-only and the build tree defaults to
`/tmp/mir-android2-platform-gud-p02-build`; override it with
`XDISP_P02_BUILD_DIR=/absolute/path` if artifacts must be retained elsewhere.
The default is one build job for the memory-constrained laptop; set
`XDISP_P02_BUILD_JOBS=<n>` only after confirming the available RAM.
The script builds `wrapper`, the `mirplatformgraphicsandroid` shared module,
and `mir_unit_tests_android2`, then runs the focused
`GudPresentationWorker.*` GTest suite directly. For a full suite, run the
same image/mount setup with `cd "$XDISP_P02_BUILD_DIR" && ctest
--output-on-failure`.
On Fedora with SELinux enforcing, the run containers use
`--security-opt label=disable` rather than relabeling the checkout; they run
as the invoking user with no network or Linux capabilities, keep `/src`
read-only, and write only the selected build directory.

Record the exact commit, image tag, build command, compiler, test output, and
deployed plugin hash. Do not deploy an uncommitted build. A source-level worker
check is not a substitute for this gate.

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
