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

The current laptop is native AArch64, so the container emits a native AArch64
module. Before staging, use `readelf -d` on the module and require the phone's
versioned sonames (currently `libmir1platform.so.18`, `libmir1common.so.7`,
`libmir1core.so.1`, and Boost 1.83). A successful build against another
Ubuntu/UBports release is not a deployable result.

Prerequisite: a working Docker daemon. Build and test with:

```bash
./tools/xdisp-p0.2-build.sh
```

The source is mounted read-only and the build tree defaults to
`/tmp/mir-android2-platform-gud-p02-build`; override it with
`XDISP_P02_BUILD_DIR=/absolute/path` if artifacts must be retained elsewhere.
Use a fresh explicit build directory if an earlier container cache refers to a
different source mount; do not delete an evidence-bearing build tree just to
reuse its cache.
The default is one build job for the memory-constrained laptop; set
`XDISP_P02_BUILD_JOBS=<n>` only after confirming the available RAM.
The script builds `wrapper`, the `mirplatformgraphicsandroid` shared module,
and `mir_unit_tests_android2`, then runs the focused
`GudPresentationWorker.*:GudHwcBoundary.*` GTest suite directly. For a full suite, run the
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

Also treat compositor-wide resource failures as a stop condition, even before
the first GUD transfer. If the test interval produces repeated binder
allocation failures, KGSL file-descriptor exhaustion, a compositor crash, or
another phone-health regression, immediately restore the packaged plugin and
restart only LightDM. Preserve the logs and do not mislabel that result as a
contained GUD I/O error or as proof that the worker ran.

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

## 2026-07-27 deployment result

The first Focal artifact was deliberately not retried after LightDM rejected
it before platform initialization: it required unversioned
`libmirplatform.so.18`, while the phone provides versioned Mir 1 sonames. The
Noble rebuild at implementation `3fffb05` and build-environment commit
`d47b771` produced the AArch64 module with SHA-256
`cbcf648f26174413df718e5b5c71e41c6cf338dfe3e17dac32c52ef82581dac0`.
`LD_TRACE_LOADED_OBJECTS` on the phone resolved its Mir 1 and Boost
dependencies before the module was mounted.

After the required host/enumeration gate found `1d50:614d` at its dynamically
assigned path, the commit-qualified module was bind-mounted over the package
path and LightDM started successfully. Mir reported the synthetic 1280x720
DisplayPort output as connected and used. Over the following 52 seconds it
did **not** log `GUD POC output enabled`, and the Pi recorded no new FunctionFS
receive session; no GUD worker/KMS transfer therefore ran. During the same
interval the phone kernel reported sustained binder `-12` allocation failures
and KGSL `-24` file-descriptor exhaustion. The temporal association does not
by itself establish a worker cause, but it is a phone-health regression and
fails the hardware gate.

The test bind mount was immediately removed, LightDM was restarted with the
packaged module hash
`cd0ddc0342d19df63798e9bbcf496e3657b543bd9827d53004b997454f00ae74`, and the
stock compositor returned with 90 open descriptors. The normal
`/home/phablet/gud.ko` was never replaced or rebuilt; the Pi stayed `active`
and `configured` and was not restarted. Retained commands and raw logs are in
`gud/backport-4.9/env/local/evidence/xdisp-p0.2-hardware-2026-07-27T1125COT/`.
This is failure/rollback evidence only: slow output, absent startup, I/O
error, reappearance, and shutdown acceptance remain unverified.

## 2026-07-27 follow-up source diagnosis

The deployment log's missing `GUD POC output enabled` line is not evidence
that the P0.2 worker or KMS path failed. The older synchronous POC wrote that
line while lazily constructing its presenter, before it required an external
Android framebuffer. P0.2 constructs its worker only after it obtains an
external `mga::Buffer`; the earlier session therefore established only that no
worker/KMS transfer occurred. It did not establish why the synthetic output
had no frame, and the binder/KGSL failures remain a temporal correlation, not
a worker-cause finding.

The current source makes the boundary explicit: a synthetic GUD external
output is a Mir-rendered sink and is omitted from Android HWC `prepare()` and
`set()` lists. This corrects the former unverified assumption that Android HWC
would ignore that slot, while retaining the normal primary and virtual HWC
paths. It is a defensive P0.2 correctness repair, not a claim that it alone
explains the prior HWC2 phone health failure (that wrapper had no active
physical external display in the retained log). New one-time Mir messages
distinguish an absent external `DisplayContents` frame, a non-Android frame,
and a started presentation worker on the next hardware attempt.

At `mir-android2-platform-gud` commit `01d1f23`, the Noble build
produced `graphics-android2.so.16` SHA-256
`2ce05a584bcea36b5138e2b53e4849e511671a79a91f03a212881bba3fba2d40` with
the phone's versioned Mir 1 sonames and Boost 1.83. The direct focused filter
ran six passing checks: the five worker lifecycle checks plus the synthetic
HWC-boundary policy. This is retained offline evidence only; the artifact is
commit-qualified but has not yet passed a hardware acceptance case.

## 2026-07-27 guarded hardware retries

Commit `01d1f23` started the worker and made the synthetic output connected
without the earlier binder/KGSL failure, but did not reach KMS setup or Pi
traffic. Commit `0b09f77` added KMS-stage messages; its plugin SHA-256 was
`c7f8659faeb3646462248c0bc492328ab6fa42d4a844cdccddb56e74af1ae843` and its
six focused tests passed. After the mandatory dynamic `1d50:614d` gate, the
worker processed an external frame and opened GUD DRM, then repeatedly failed
during atomic KMS resource setup before allocation, modeset, or USB traffic.
The Pi stayed active/configured and both sessions restored the packaged plugin.
This is contained pre-transfer evidence only; P0.2 remains in progress and
all slow/I/O-error/reappearance/shutdown/manual-input acceptance rows remain
unverified. Raw evidence is under
`gud/backport-4.9/env/local/evidence/xdisp-p0.2-hardware-2026-07-27T1215COT/`.

The final `7185800` exception-logging retry identified the exact pre-transfer
boundary: GUD was connected but advertised only `1920x1080`, while this P0.2
POC intentionally requires `1280x720`. The module hash was
`2065484f746e766b245d828a0f40465996d4fcaeb6099a7162a9b355d00ccb39`, and the
same six focused tests passed. Mode/geometry alignment is P1/P2 work, so this
procedure does not change the Mir or Pi mode merely to force P0.2 acceptance.

## 2026-07-27 advertised-startup-mode retry

Commit `406b464` removes the POC's fixed 1280x720 assumption without changing
the Pi's advertised modes or physical configuration. At server startup it
scans the accessible GUD DRM card, chooses its preferred advertised connected
mode (or the first usable one), and uses that same cached geometry for the
synthetic Mir output. The worker makes the corresponding selection when it
opens KMS; a later card/mode change remains a contained present failure and
P0.3 hotplug work, not an implicit mode-management feature. The compatible
module SHA-256 was
`c3c80df697180a513c3943999b2e6fa88b568efd479fca01360b5c8eaa1461d2`; all
eight focused tests, including two mode-selection tests, passed.

After the mandatory gate found `1d50:614d` at `1-1.3`, the qualified module
made Mir expose a `1920x1080` DisplayPort output, and the Pi recorded fresh
1920x1080 direct-exact FunctionFS frames. The largest retained actual payload
was 12,157 bytes and every shown receive returned to Idle. This proves the
former fixed-mode KMS boundary is resolved without changing either kernel,
the normal module, payload cap, or Pi service.

The interval immediately reproduced repeated phone binder `-12` and KGSL
`-24` failures. That is a compositor-health stop condition, not a successful
slow-output or I/O-error acceptance case. The plugin was removed and the
packaged hash restored; the normal GUD module and Pi service were untouched.
The ordinary unmount was busy after LightDM stopped, so rollback kept LightDM
down until an idle-mount inspection confirmed no holder and a lazy unmount
detached it; LightDM then returned active on the packaged plugin. P0.2 remains
**in progress**. Full commands and results are retained at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-mode-startup-2026-07-27T1249COT/`.

## 2026-07-27 compositor-health comparison gate

Before another qualified-plugin deployment, the packaged plugin was sampled
after the documented dynamic host gate returned `FOUND: /sys/bus/usb/devices/1-1.3`.
The packaged hash remained
`cd0ddc0342d19df63798e9bbcf496e3657b543bd9827d53004b997454f00ae74`, but it
was not a clean comparison baseline: repeated binder `-12` and KGSL `-24`
errors continued across a LightDM-only restart. The replacement compositor had
89 FDs, while the Android HWC2 service named by the binder log had 22 FDs; the
source retains only one duplicated present fence per Android display. This does
not prove a Mir, worker, or HWC leak. It does establish that a new experimental
interval would be confounded by pre-existing phone health, so no test plugin was
mounted and no Pi payload was sent.

Commit `20e54b0` adds bounded worker counters for submitted/coalesced/active/
completed/failed frames and compositor FD count, with focused assertions. Its
phone-matched artifact SHA-256 is
`72648f4b5d9c00abcbbc3201f14182c262ed6c512987587374edca58eee366ea`; the six
`GudPresentationWorker.*:GudHwcBoundary.*` checks pass. It is staged evidence
only. Retained commands, counts, and the next clean-baseline gate are at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-health-comparison-2026-07-27T1259COT/`.
Keep P0.2 **in progress** and the packaged plugin active until a clean packaged
baseline permits the bounded comparison.

## 2026-07-27 rebooted baseline and plugin-load control

After an authorized OnePlus-only reboot, the packaged hash was verified and a
30-second control stayed at 90 compositor FDs with zero binder `-12` and KGSL
`-24` errors. The mandatory dynamic gate found `1d50:614d` at `1-1.3`; read-only
Pi preflight reported `active`, `configured`, and no poisoned/in-flight receive.
The commit-qualified `20e54b0` artifact was then mounted for 20 seconds. Its
compositor stayed bounded at 89--90 FDs and five--six sync fences with zero new
binder/KGSL errors. The packaged plugin was restored and verified immediately.

This is a clean plugin-load control only: after the OnePlus reboot, the exposed
DRM topology had only `card0` and Mir recorded DisplayPort disconnected. No GUD
card was available to synthesize the output, so no worker marker, KMS setup,
Pi receive, or payload occurred. The temporary missing GUD card was expected:
the OnePlus reboot had unloaded the out-of-tree normal GUD module. It was later
restored by loading the unchanged `/home/phablet/gud.ko`, which recreated a
connected GUD card and connector. This is not P0.3 reappearance evidence, not a
P0.2 worker success, and not a reason to use a fixed card path. Full
commands and state are retained at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-reboot-baseline-2026-07-27T1313COT/`.
P0.2 remains **in progress**.

## 2026-07-27 plain-FBO and output-activation boundary

Commit `9af1768` replaced the synthetic external Android window surface with a
pbuffer EGL surface and standalone GLES texture/FBO. It allocates no Android
gralloc framebuffer for the offscreen target and returns no Android framebuffer
from `last_rendered_buffer()`. Its focused worker, HWC-boundary, render-only,
and offscreen-target checks passed. On a fresh OnePlus boot, with the worker
disabled, it nevertheless reached 808 FDs/715 sync files; no binder or KGSL
error appeared during that bounded run. Thus the plain texture-FBO replacement
does not solve the render-only retention.

The next controls eliminated progressively more of the platform path. With the
synthetic output disabled entirely, the compositor remained at 90 FDs/zero
additional sync files while the primary continued at 60 commits/s. With the
synthetic output configured and used but all synthetic `HwcDevice` bookkeeping
and `GudOutput` submission bypassed, it reached 899 FDs/806 sync files. With
that same output configured/used but omitted from `DisplayGroup` compositor
buffers, it still reached 702 FDs/609 sync files. These controls establish that
the leakage is neither the external EGL/FBO producer, the GUD worker/KMS/USB
path, nor Android HWC bookkeeping; a connected/used second Mir output is
sufficient.

Commit `f4d8262` added the final discriminator: a test-only configuration that
keeps the synthetic DisplayPort connected but marks it unused. Its artifact
SHA-256 was
`9de11f5b531d4264620516fcc2b5ce6a734941a9db6c2f317b5736fcebeda470`; all ten
focused tests passed. Following a fresh phone reboot, the mandatory dynamic
host gate found `1d50:614d` at `1-1.3`; Pi preflight was `active`,
`configured`, and idle. The unchanged normal `gud.ko` was loaded only for
discovery. During the bounded run, Mir logged Output 2 as DisplayPort
disconnected (the public configuration maps unused outputs that way) and the
compositor remained at 89 FDs/5 sync files, with no new binder/KGSL regression.
The packaged plugin SHA-256
`cd0ddc0342d19df63798e9bbcf496e3657b543bd9827d53004b997454f00ae74` was
immediately restored and LightDM returned active.

This identifies the remaining boundary precisely: activating the second Mir
output (`used=true`) drives the retention even when it has no offered display
buffer and performs no synthetic HWC work. Do not re-enable the worker or run
slow-output acceptance on this architecture. A future implementation must make
GUD a non-Mir secondary presentation path (or repair the relevant upstream
multi-output lifecycle) before it can safely activate an external desktop.
P0.2 remains **in progress**; no payload was sent in these controls.

## 2026-07-27 external-only active-output control

Commit `bdbfc5b` made the test-only policy explicit: with a discovered
synthetic GUD output, the primary is reported connected/unused/off and the
synthetic DisplayPort output connected/used/on. It retains the existing
diagnostic bypasses: no synthetic compositor buffer, Android HWC bookkeeping,
worker, KMS, USB, or Pi payload work. The compatible artifact SHA-256 was
`6912e50de8d14527e30a127586e15093d0d691968556aa56ad97673c86cd2d8c`; the
dedicated output-policy test and the existing six worker/HWC-boundary checks
passed.

After a fresh OnePlus boot, the packaged baseline was 90 FDs/6 sync files with
zero binder `-12` and KGSL `-24` errors. The mandatory dynamic gate found
`1d50:614d` at `1-1.3`, and Pi preflight was active/configured with no unsafe
receive state. The unchanged normal `gud.ko` was loaded only to recreate the
post-reboot GUD card for mode discovery.

Mir 1.8.2 reported the requested runtime configuration exactly: Output 1 LVDS
was connected, unused, and powered off; Output 2 DisplayPort was connected,
used, powered on, and selected the advertised 1280x720 mode. Lomiri then
exited repeatedly before a measurement interval began; LightDM logged `Unity
System Compositor: Failed to write to compositor: Broken pipe` and reached its
restart limit. Consequently there are no 5/15/30/60-second FD samples and no
fdinfo classification: this is not evidence for either the stable or leaking
external-only cases.

This is **Result C**: on this Mir 1/Lomiri stack the primary-unused,
external-used configuration reaches Mir's reported runtime state but does not
survive Lomiri startup. The test plugin was immediately unmounted. The
packaged hash `cd0ddc0342d19df63798e9bbcf496e3657b543bd9827d53004b997454f00ae74`
is restored, LightDM is active, and the restored compositor is 90 FDs/6 sync
files with zero binder/KGSL errors. Full raw evidence is retained at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-external-only-2026-07-27T2158COT/`.

Result C correction: the previous external-only test advertised the synthetic
DisplayPort as connected/used/on while
`should_offer_synthetic_output_to_compositor()` was still false, so the
diagnostic gate excluded that `DisplayBuffer`. Because the primary was also
powered off, `DisplayGroup::for_each_display_buffer()` enumerated no display
buffers to the compositor. The Lomiri/LightDM failure is real evidence for
that inconsistent zero-target diagnostic state, but it cannot distinguish
stable from leaking valid external-only operation and does not establish that
such a topology is unsupported. Test D must restore synthetic external
compositor participation while preserving the existing GUD/HWC/transport
bypasses.

## 2026-07-27 Test D one-target compositor attempt

Commit `b793099` restored synthetic external compositor participation and
added change-only target-set logging in `DisplayGroup`. Its compatible
`graphics-android2.so.16` artifact SHA-256 was
`e75aaa12061d6f10b8c676209bea91ceea817e41754eb300b487c150e57a2dfc`.
The focused Android2 test filter passed seven checks: the five presentation
worker checks, the synthetic HWC-boundary check, and the new DisplayGroup
control. The latter explicitly configured the primary off and the synthetic
external on, and logged `primary=0 external=1 total=1`. The module requires
the phone's versioned `libmir1platform.so.18`, `libmir1common.so.7`, and
`libmir1core.so.1` sonames.

Following a fresh OnePlus session, the mandatory dynamic host gate found
`1d50:614d` at `1-1.3`; the Pi service was active and its UDC configured with
no unsafe/in-flight receive. The unchanged normal `/home/phablet/gud.ko` was
loaded only to recreate GUD discovery. The packaged baseline was 90 FDs / 6
sync files with zero binder `-12` and KGSL `-24` errors.

The Test D artifact reported the desired public configuration (LVDS
connected/unused and DisplayPort connected/used), but its mandatory
compositor-target instrumentation instead logged
`xdisp compositor targets: primary=1 external=1 total=2`. Lomiri then failed
startup repeatedly and LightDM reached its restart limit. This is **D-invalid
— intended one-target topology was not achieved**: the FD behavior and startup
failure must not be interpreted as an external-only result. In particular, the
primary remained offered to the compositor despite its public unused state;
the diagnostic policy must be corrected before another Test D run.

The test bind mount was removed before recovery. LightDM was reset and
restarted on the packaged module SHA-256
`cd0ddc0342d19df63798e9bbcf496e3657b543bd9827d53004b997454f00ae74`; after
settling, Lomiri returned to 90 FDs / 6 sync files with zero binder/KGSL
errors. Retained evidence summary:
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-test-d-invalid-2026-07-27T2302COT/`.

## 2026-07-27 Test D2 configure-path power-policy attempt

Commit `3961ae1` added the narrow external-only effective-power policy to
`Display::configure_locked()`: when the synthetic output is active and the
primary is submitted unused, requested primary power is retained for public
configuration but applied to the platform DisplayBuffer as off. It also added
change-only `xdisp configure` logging of used, requested power, and effective
power. The compatible artifact SHA-256 was
`2a872f332dbcc5604addf34f0b7909695fda7348892e403b3f93cf20cfe330a2`; nine
focused checks passed, including the configuration-path control that logs
primary `used=0`, requested on, effective off, external effective on, and a
single offered external DisplayBuffer.

The fresh-session packaged baseline was 95 FDs / 6 sync files with zero binder
`-12` and KGSL `-24` errors. The dynamic GUD gate found `1d50:614d` at
`1-1.3`; the Pi service was active/configured with no receive in flight. The
unchanged normal GUD module was present only for discovery.

Hardware result: **D2-invalid**. The diagnostic plugin again reported LVDS
connected/unused and DisplayPort connected/used, but before any `xdisp
configure` trace appeared, `DisplayGroup` repeatedly logged
`primary=1 external=1 total=2`. Lomiri then exited with the LightDM broken-pipe
failure. The required `primary=0 external=1 total=1` topology was therefore
not achieved, so this crash and the startup FD samples are not interpreted.

Source inspection explains the missing configure trace: `DisplayBuffer` is
constructed with `power_mode_` initialized to on, while the D2 override runs
only later in `configure_locked()`. The target enumeration happened before
that configuration application. This identifies the next discriminator, but
no further power/buffer changes were made in this test.

The bind mount was removed and LightDM restarted on the packaged module
`cd0ddc0342d19df63798e9bbcf496e3657b543bd9827d53004b997454f00ae74`.
Restoration settled at 90 FDs / 6 sync files with zero binder/KGSL errors.
Evidence summary:
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-test-d2-invalid-2026-07-27T2326COT/`.

## 2026-07-27 synthetic offscreen target gate

Commit `eae00c7` introduced the first synthetic-only pbuffer/FBO render target
to remove `eglCreateWindowSurface`/`MirNativeWindow` from the external producer.
Its compatible artifact SHA-256 was
`0525a1d2d0d77d50d61970154e212963d52a6ef444db648592bb2c7a8abc1487`; 21
focused checks passed. The render-only phone gate failed at startup with
`cannot bind synthetic GUD offscreen framebuffer`, before any worker, KMS, USB,
or Pi receive. It was immediately rolled back to the packaged plugin and normal
GUD module. A plain GLES texture-FBO refinement compiled but exhausted local
disk while linking the unit-test binary; it was not committed or deployed.
This is contained startup evidence only. See
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-offscreen-gate-2026-07-27T1659COT/`.
P0.2 remains **in progress**.

## 2026-07-27 synthetic render-only control

Commit `a0fb290` is a render-only control: it retains the synthetic Android EGL
window surface but drops its completed external frame before worker creation.
The compatible artifact SHA-256 is
`84d67e85f52760ad474436d28e9fa1dac82485fe374e79e216278427711d1531`; 20
focused control, worker, HWC-boundary, and server-window checks pass. After a
fresh OnePlus reboot, clean packaged 90-FD control, Pi active/configured/Idle
preflight, host gate, and unchanged normal GUD discovery, it logged the explicit
render-only drop and no worker/KMS/USB/Pi payload activity.

Synthetic Android EGL rendering alone reached a stable 757--758 FDs and
658--659 sync files, with zero binder/KGSL errors in the bounded interval. This
isolates the remaining retention to the synthetic
`eglCreateWindowSurface`/`MirNativeWindow` path, not the worker, KMS, USB, Pi,
or later GUD lease handoff. Stop adding fence exceptions; the next implementation
must use an explicit offscreen gralloc/EGL target for the synthetic output and
transfer its lease directly to the worker. Packaged Mir and normal GUD were
restored. Evidence is at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-render-only-2026-07-27T1633COT/`.
P0.2 remains **in progress**.

## 2026-07-27 synthetic return-fence retry

The `e9fb3a5` flow diagnostic established that the synthetic output reuses three
Android buffers, receives one returned fence per render, and receives no fence
on the next dequeue. Commit `9c6d772` therefore waited and cleared returned
fences only for the synthetic GUD external window, preserving primary and real
Android-external behavior. Its compatible artifact SHA-256 was
`72829fb6240048bfe7e63e8d3fec65d280635abb742409a0c1c77a2aeed52e98`; all 19
focused server-window, worker, and HWC-boundary tests passed.

After explicit Pi reboot/start authorization, a fresh active/configured/Idle
preflight, host gate, clean 15-second packaged control, and bounded 1920x1080
GUD setup, the fixed artifact still reached 1024 FDs/925 sync files and emitted
binder `-12` and KGSL `-24`. Counters had `copied_fences=0`, so this disproves
the returned-fence wait as the remaining fix, but does not identify the final
remaining handle owner. Pi payloads stayed bounded and shown receives returned
Idle. The packaged plugin and normal GUD module were restored; LightDM needed
only reset/start after failure. Evidence is at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-return-fence-fix-2026-07-27T1431COT/`.
P0.2 remains **in progress**.

## 2026-07-27 external render-fence flow stop

External-only counters at `694d591` proved that the remaining 574-sync-file
plateau is not a large gralloc pool: the synthetic output reused three Android
buffers while returned fences increased one-for-one with external renders. The
likely remaining boundary is the acquire-fence duplicate supplied to Android
EGL on dequeue, but source review does not yet prove its owner. `e9fb3a5` adds
the minimum counter set to distinguish returned, dequeued, and EGL-supplied
fences; its compatible artifact SHA-256 is
`67a5730bafc735491788af8b5cfe3284dc9a5a0c1d8956f29dba19b17da639e7` and its
focused checks pass.

That diagnostic was not mounted. Pi read-only journal evidence exposed an
`InFlight` receive without a safely observed return in the initial window, so
the hardware session stopped under the P0.2/P0.1 containment rule. No Pi action
was taken. Packaged Mir and normal GUD remain restored. Evidence is at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-render-fence-flow-2026-07-27T1358COT/`.
P0.2 remains **in progress**.

## 2026-07-27 normal GUD module recovery

After a fresh read-only Pi safety preflight and mandatory `FOUND:` gate, the
unchanged normal `/home/phablet/gud.ko` was loaded and successfully created
`/dev/dri/card1`. A read-only `modetest -M gud -c` probe reported the GUD
`Virtual-2` connector connected with one preferred 1280x720 mode. This restores
the expected post-reboot host module state; it does not run P0.2 presentation.
The normal module is not qualified for P0.2 payload work because it lacks the
separately verified <=12,800-byte adaptive transfer path. Evidence is at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-normal-gud-recovery-2026-07-27T1322COT/`.

## 2026-07-27 synthetic-output fence boundary

After a clean post-reboot packaged baseline, the 1920x1080 separately named
bounded GUD diagnostic module and `20e54b0` reproduced the phone health failure
while its worker counters still showed bounded active/pending frames and normal
coalescing. Compositor sync-file FDs rose with submitted external frames until
the 1024 descriptor limit, then binder `-12` and KGSL `-24` failures occurred.
The Pi received only bounded payloads (shown maximum 12,157 bytes) and returned
each shown receive to Idle.

Source tracing proved that the synthetic output was excluded from Android HWC
but still armed an Android acquire fence through `LayerList::swap_occurred()`;
no HWC `set()` consumer could close it. Commit `1f9e8db` prevents acquire-fence
arming for that synthetic sink while preserving primary and virtual HWC paths.
Its compatible artifact SHA-256 is
`b259a04b5f891b97d367ff9ca1d37cd076e5b8d277adf77c3981b223a8d009b4` and six
focused tests pass.

The fixed hardware retry no longer exhausted FDs or produced binder/KGSL errors,
but plateaued at 674 FDs/574 sync files, far above the 90-FD control. This is a
separate unproven fence-retention path, likely before HWC layer handling, and
blocks acceptance. The packaged plugin and normal GUD module were restored;
the Pi remained active/configured with Idle receives. Details are retained at
`../gud/backport-4.9/env/local/evidence/xdisp-p0.2-fence-boundary-2026-07-27T1333COT/`.
P0.2 remains **in progress**.
