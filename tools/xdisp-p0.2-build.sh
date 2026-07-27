#!/bin/sh
# Build the Android2 platform against the checked Ubuntu Touch Noble/Mir1 ABI.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${XDISP_P02_BUILD_DIR:-/tmp/mir-android2-platform-gud-p02-build}
build_jobs=${XDISP_P02_BUILD_JOBS:-1}
image_tag=mir-android2-platform-gud-p02-build:ubuntu24.04-noble

mkdir -p "$build_dir"

docker build \
    --quiet \
    --tag "$image_tag" \
    --file "$repo_root/tools/xdisp-p0.2-build/Dockerfile" \
    "$repo_root/tools/xdisp-p0.2-build"

docker run --rm \
    --network none \
    --cap-drop ALL \
    --security-opt label=disable \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --volume "$repo_root:/src:ro" \
    --volume "$build_dir:/build" \
    --workdir /src \
    "$image_tag" \
    cmake -S /src -B /build -G Ninja \
        -DMIR_ENABLE_TESTS=ON \
        -DMIR_BUILD_UNIT_TESTS=ON \
        -DMIR_RUN_UNIT_TESTS=ON

docker run --rm \
    --network none \
    --cap-drop ALL \
    --security-opt label=disable \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --volume "$repo_root:/src:ro" \
    --volume "$build_dir:/build" \
    --workdir /src \
    "$image_tag" \
    cmake --build /build --target wrapper mirplatformgraphicsandroid mir_unit_tests_android2 --parallel "$build_jobs"

docker run --rm \
    --network none \
    --cap-drop ALL \
    --security-opt label=disable \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --volume "$repo_root:/src:ro" \
    --volume "$build_dir:/build" \
    --workdir /build \
    "$image_tag" \
    /build/bin/mir_unit_tests_android2.bin --gtest_filter='GudPresentationWorker.*:GudHwcBoundary.*'
