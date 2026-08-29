# Building Mir Android2 packages in a container

This repository targets the Ubuntu/UBports Mir 1 ABI. A Fedora or other host
installation may fail during CMake configuration even when the source is
correct, because it does not provide the matching Mir development packages,
Boost configuration, or `libandroid-properties` metadata.

Use the ARM64 Ubuntu 24.04 build image below for a reproducible build:

```text
mir-android2-platform-gud-p02-build:ubuntu24.04-noble
```

The image must contain the Mir 1 development packages, Android headers,
libhybris, libdrm, udev, GoogleTest, and Debian packaging tools. Check that it
is available before starting:

```sh
docker image inspect mir-android2-platform-gud-p02-build:ubuntu24.04-noble
```

The commands below mount the repository read-only. Build output is written to
a disposable directory under `/tmp`, so the container cannot modify the
working tree. This is the supported build path when the workstation lacks the
matching Mir/Android2 development packages; the host does not need to be an
ARM64 Ubuntu installation.

## Quick CMake build

This is useful for checking compilation of the current source without creating
Debian packages:

```sh
MIR_GUD_BUILD_DIR=/tmp/mir-android2-platform-gud-cmake
mkdir -p "$MIR_GUD_BUILD_DIR"

docker run --rm \
  -v "$PWD":/src:ro,z \
  -v "$MIR_GUD_BUILD_DIR":/build:z \
  -w /build \
  mir-android2-platform-gud-p02-build:ubuntu24.04-noble \
  bash -lc '
    cmake -S /src -B . \
      -DMIR_PLATFORM=android \
      -DMIR_ENABLE_TESTS=OFF \
      -DMIR_BUILD_UNIT_TESTS=OFF \
      -DCMAKE_BUILD_TYPE=Debug &&
    cmake --build . --parallel 2
  '
```

Successful output includes the `xdispd`, `mirgud`, Android2 client-platform,
and Android2 graphics-platform targets.

## Debian package build

Use a fresh source copy inside the container. The package build needs
`debhelper-compat`, which may not be installed in the base image:

```sh
MIR_GUD_PACKAGE_DIR=/tmp/mir-android2-platform-gud-packages
mkdir -p "$MIR_GUD_PACKAGE_DIR"

docker run --rm \
  -v "$PWD":/src:ro,z \
  -v "$MIR_GUD_PACKAGE_DIR":/build:z \
  -w /build \
  mir-android2-platform-gud-p02-build:ubuntu24.04-noble \
  bash -lc '
    set -o pipefail
    apt-get update -qq
    apt-get install -y --no-install-recommends debhelper-compat
    rm -rf source
    cp -a /src source
    cd source
    dpkg-checkbuilddeps -B
    make -f debian/rules \
      COMMON_CONFIGURE_OPTIONS="-DCMAKE_INSTALL_LIBEXECDIR=lib/aarch64-linux-gnu/mir1 -DMIR_LINK_TIME_OPTIMIZATION=ON -DCMAKE_CXX_FLAGS=-Wno-error=stringop-overflow" \
      clean build binary 2>&1 | tee /build/package-build.log
  '
```

The `COMMON_CONFIGURE_OPTIONS` override is intentional:

* It supplies the package's ARM64 Mir libexec directory and enables LTO.
* GCC 13 in the container diagnoses an existing Mir Android2 test as
  `-Wstringop-overflow`; the project builds with `-Werror`, so the isolated
  `-Wno-error=stringop-overflow` flag is needed to compile the legacy test.
* Tests remain enabled and are executed by `debian/rules`. Do not replace this
  with `MIR_ENABLE_TESTS=OFF` for a package build: the
  `mir1-android2-tests` package expects `mir_unit_tests_android2`.
* `DEB_CMAKE_EXTRA_FLAGS` is not consumed by this repository's Debian rules;
  pass the options through the `COMMON_CONFIGURE_OPTIONS` make variable as
  shown above.

The packages are written directly into `MIR_GUD_PACKAGE_DIR`. The expected
runtime packages are:

```text
lomiri-xdisp_*_arm64.deb
mir1-platform-graphics-android2-16_*_arm64.deb
mir1-client-platform-android2-5_*_arm64.deb
mir1-graphics-drivers-android2_*_arm64.deb
mir1-android2-tests_*_arm64.deb
```

Debug symbol `.ddeb` files may also be produced.

The 2026-08-29 qualification used this exact package command and completed all
five Debian test targets with zero failures, including 53 xdisp unit tests.
The resulting `lomiri-xdisp_1.8.0_arm64.deb` SHA256 was
`46568f59caad9cff73b02e6ea253dfc2720b29787f427f92c6f6c9f562edaf24`.

## Verify the artifacts

Use the same image to inspect package contents when the host does not have
`dpkg-deb`:

```sh
docker run --rm \
  -v "$MIR_GUD_PACKAGE_DIR":/artifacts:ro,z \
  mir-android2-platform-gud-p02-build:ubuntu24.04-noble \
  bash -lc '
    for package in /artifacts/*.deb; do
      echo "--- $(basename "$package")"
      dpkg-deb -c "$package" | grep -E \
        "xdispd|mirgud|xdisp-status|xdisp.service|org.lomiri.XDisp|graphics-android2|android2.so|mir_unit_tests"
    done
  '
```

The `lomiri-xdisp` package must contain:

```text
usr/libexec/lomiri-xdisp/xdispd
usr/libexec/lomiri-xdisp/mirgud
usr/bin/xdisp-status
usr/lib/systemd/system/xdisp.service
usr/share/dbus-1/system-services/org.lomiri.XDisp.service
etc/dbus-1/system.d/org.lomiri.XDisp.conf
```

Record checksums for handoff or later installation testing:

```sh
sha256sum "$MIR_GUD_PACKAGE_DIR"/*.deb
```

## Troubleshooting

* `Unable to find Boost header files`, missing `libandroid-properties`, or
  missing `mir1client`/`mir1core` pkg-config modules on the host means the
  host is not the target build environment. Run the container build.
* `dpkg-checkbuilddeps` reporting `debhelper-compat (= 13)` means the
  disposable container needs the `apt-get install` step above.
* `dh_missing` reporting `usr/bin/xdisp-status` means the package manifest is
  stale. `debian/lomiri-xdisp.install` must include `usr/bin/xdisp-status`.
* With SELinux-enabled Docker, keep the `:z` suffix on both bind mounts. Do
  not relabel-mount the `/tmp` root itself; mount a dedicated output directory.
* A successful container build proves compilation, unit tests, and package
  staging only. It does not prove the OnePlus 6 USB GUD gate. Follow
  [`gud/docs/oneplus6-usb-host-gud-troubleshooting.md`](../../gud/docs/oneplus6-usb-host-gud-troubleshooting.md)
  and require dynamic enumeration of `1d50:614d` before diagnosing KMS or
  claiming hardware qualification.
* The phone image used for the hardware run has a read-only dpkg database.
  Do not install the `.deb` there with `dpkg -i`; stage the rebuilt binaries
  and service override in a recoverable `/home/phablet/` location, or use a
  writable development image. The qualification report records both the
  package build and the staged-runtime paths.
