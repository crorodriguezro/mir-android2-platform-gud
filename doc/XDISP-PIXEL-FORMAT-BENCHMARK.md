# XDISP Pixel Format Benchmark: RGB565 vs XRGB8888

## Objective

Benchmark RGB565 vs XRGB8888 pixel formats on the proven OnePlus 6 -> Mir ->
GUD -> USB -> Pi -> HDMI architecture, collecting apples-to-apples measurements
under the strict <=12,800-byte USB payload cap, and determine the recommended
default format.

## Architecture

```
Component                RGB565     XRGB8888
------------------------------------------------
Mir source               No         Yes (runtime evidence confirms a 4-byte-per-pixel source; the exact MirPixelFormat enum is recorded at runtime)
mirgud conversion        Yes        Yes (--pixel-format rgb565|xrgb8888)
GUD DRM framebuffer      Yes        Yes (DRM_FORMAT_XRGB8888 added)
host GUD support         Yes        Yes (GUD_PIXEL_FORMAT_XRGB8888 added)
Pi gadget support        Yes        Yes (GUD_PIXEL_FORMAT_XRGB8888 advertised)
Pi scanout support       Yes        Yes (DrmFourcc::XRGB8888 added)
```

## Key Constraint: 12,800-byte USB Payload Cap

The XDISP LZ4 variant enforces a hard `GUD_XDISP_PAYLOAD_LIMIT` of 12,800
bytes per bulk URB. This cap is format-aware.

At 1280 width and 12,800-byte raw cap:

| Format   | Bytes/pixel | Row size @ 1280px | Rows/cap (raw) | Payloads/frame (raw) |
|----------|-------------|-------------------|-----------------|----------------------|
| RGB565   | 2           | 2,560             | 5               | ~144                 |
| XRGB8888 | 4           | 5,120             | 2               | ~360                 |

Compression can change these significantly. The actual compressed payload size
depends on scene content and is measured at runtime via the GUD host driver
planner counters.

## Mir Source Memory Layout Analysis

The Mir screencast buffer format is queried at runtime via
`mir_connection_get_available_surface_formats()`. The format is recorded in
the `SourceFormat` struct and reported via `conversion_path`.

On this little-endian target, the byte-for-byte compatibility between Mir
source formats and `DRM_FORMAT_XRGB8888` is:

| Mir pixel format enum          | Integer value  | LE memory layout | DRM_FORMAT_XRGB8888 compatible? |
|--------------------------------|----------------|-------------------|---------------------------------|
| `mir_pixel_format_xrgb_8888`   | 0x00RRGGBB     | B, G, R, X        | YES (direct memcpy)             |
| `mir_pixel_format_xbgr_8888`   | 0x00BBGGRR     | R, G, B, X        | No (channel reorder)            |
| `mir_pixel_format_argb_8888`   | 0xAARRGGBB     | B, G, R, A        | No (channel reorder)            |
| `mir_pixel_format_abgr_8888`   | 0xAABBGGRR     | R, G, B, A        | No (channel reorder)            |
| `mir_pixel_format_rgb_888`     | N/A            | R, G, B           | No (conversion)                 |
| `mir_pixel_format_bgr_888`     | N/A            | B, G, R           | No (conversion)                 |
| `mir_pixel_format_rgb_565`     | 16-bit         | R:G:B 5:6:5       | No (expand)                     |

`mir_pixel_format_xrgb_8888` is the only Mir format whose little-endian
memory layout is byte-for-byte identical to `DRM_FORMAT_XRGB8888` (both are
integer `0x00RRGGBB`, which on LE is memory `[B, G, R, X]`).

## Conversion Paths

The `conversion_path` instrumentation field reports which code path was used
for each frame:

| Path               | Condition                              | CPU cost |
|--------------------|----------------------------------------|----------|
| `direct-copy`      | Mir source is `xrgb_8888`, transport is XRGB8888 | memcpy only |
| `channel-reorder`  | 4-byte Mir source, transport is XRGB8888 | byte shuffle |
| `rgb565-pack`      | Any source, transport is RGB565        | pack to 16-bit |
| `rgb565-expand`    | RGB565 source, transport is XRGB8888 | expand to 32-bit |
| `rgb888-expand`    | RGB888/BGR888 source, transport is XRGB8888 | expand to 32-bit |

## Benchmark Workloads

### Phase 5: Transport-Isolated Benchmarks

These workloads isolate the GUD transport (USB + Pi gadget) from the Mir
capture pipeline.

```bash
# Generated transport workload (no Mir, no capture):
# RGB565:
mirgud --pattern --pixel-format rgb565 --no-gud
# XRGB8888:
mirgud --pattern --pixel-format xrgb8888 --no-gud

# Stage A with GUD (measures USB + Pi gadget throughput):
# RGB565:
mirgud --pattern --pixel-format rgb565
# XRGB8888:
mirgud --pattern --pixel-format xrgb8888
```

The `--pattern` flag sends a static checkerboard frame repeatedly. It is a
generated transport workload and does not benchmark Mir capture. With
`--no-gud`, it exercises no GUD transport; without `--no-gud`, it measures
the generated-frame transport path including USB transfer.

### Phase 6: Full Lomiri Extend Benchmarks

These workloads exercise the complete Mir -> GUD -> USB -> Pi -> HDMI pipeline
with real Lomiri content.

```bash
# Full extend pipeline (Mir -> GUD -> USB -> Pi -> HDMI)
# RGB565:
mirgud --source-mode extend --mir-socket-file /run/mir_socket \
       --pixel-format rgb565 --size 1280 720
# XRGB8888:
mirgud --source-mode extend --mir-socket-file /run/mir_socket \
       --pixel-format xrgb8888 --size 1280 720
```

### Phase 7: Responsiveness Measurement

The `--no-gud` flag with `--source-mode extend` measures Mir capture
responsiveness without USB transport:

```bash
mirgud --source-mode extend --mir-socket-file /run/mir_socket \
       --pixel-format rgb565 --no-gud
```

The `sampled_fingerprint()` function reports frame change detection, and
the instrumentation reports `capture_us_avg` and `conversion_us_avg`.

### Quality Reference Generation

The `--quality` flag generates deterministic reference patterns from a single
RGB888 source, converts them to both RGB565 and XRGB8888, and dumps PPM files
for comparison:

```bash
mirgud --quality --dump-frame /tmp/quality

# Compare:
python3 doc/compare_frames.py /tmp/quality.gradient.rgb565.ppm /tmp/quality.gradient.xrgb8888.ppm
python3 doc/compare_frames.py /tmp/quality.ramps.rgb565.ppm /tmp/quality.ramps.xrgb8888.ppm
python3 doc/compare_frames.py /tmp/quality.hfreq.rgb565.ppm /tmp/quality.hfreq.xrgb8888.ppm
python3 doc/compare_frames.py /tmp/quality.photo.rgb565.ppm /tmp/quality.photo.xrgb8888.ppm
```

## Instrumentation

The mirgud client reports per-second statistics:

```
mirgud: frames_received=N frames_submitted=N frames_presented=N frames_dropped=N
        drop_percent=X.X conversion_failures=N gud_submit_failures=N
        capture_us[min=N avg=N max=N p50=N p95=N]
        conversion_us[min=N avg=N max=N p50=N p95=N]
        submit_us[min=N avg=N max=N p50=N p95=N]
        submit_us_avg=N capture_us_avg=N conversion_us_avg=N
        conversion_path=direct-copy:N conversion_path=channel-reorder:N
        self_fds=N
```

Denominator rules:
- `capture_us_avg` and `conversion_us_avg` are divided by `frames_received`
- `submit_us_avg` is divided by `frames_presented` (not `frames_received`)

Timing fields:
- `capture_us`: Time to read a frame from Mir (CPU mapping or EGL readback)
- `conversion_us`: Time to convert pixels to the transport format
- `submit_us`: Time to submit a frame to GUD (USB transfer)

Each timing field reports min, average, max, approximate p50, and approximate
p95 from a bounded 16-bucket histogram. No per-frame vectors are allocated.

The GUD host driver reports per-frame XDISP planner counters when
`xdisp_frame_stats=1`:

```
XDISP frame policy=... source=N payload=N rectangles=N compressed=N raw=N
       max_payload=N cap=12800 target=N compress_attempts=N
       compress_rejected=N compress_source_bytes=N
       planner_us=N copy_us=N set_buffer_us=N bulk_wait_us=N transfer_us=N
```

## Visual Quality Metrics

The `--dump-frame` and `--dump-frame-interval` options save PPM frames for
visual quality comparison. The `--quality` mode generates deterministic
reference patterns for apples-to-apples comparison.

The `compare_frames.py` script reports:
- Mean Absolute Error (MAE) per channel and overall
- Max Absolute Error per channel
- Root Mean Square Error (RMSE)
- Peak Signal-to-Noise Ratio (PSNR) in dB
- Gradient continuity diagnostic (fully-identical rows/columns, labeled as
  diagnostic only, not as "banding severity")

## Runtime Format Recording

At startup, the `DirectCapture` class records the runtime Mir source format:

```
mirgud: virtual frame source is CPU mapped 1280x720 mir_format=<N> stride=<S>
        row_order=top-down transport=xrgb8888 conversion_path=direct-copy
```

Fields recorded:
- `mir_format`: The `MirPixelFormat` enum value from the Mir graphics region
- `width`, `height`: The source dimensions
- `stride`: The source row stride in bytes
- `row_order`: top-down or bottom-up
- `transport`: The selected transport format (rgb565 or xrgb8888)
- `conversion_path`: The conversion path used for this source format

## Default Format Decision

**Default format decision: PENDING HARDWARE BENCHMARK**

The benchmark has not run yet. Do not assume XRGB8888 is the default.

## Safety Rules

1. The USB payload cap (`GUD_XDISP_PAYLOAD_LIMIT`) is never exceeded, regardless
   of format. The host driver validates every chunk against the cap.
2. The `gud_pipe_check()` function validates framebuffer pitch matches
   `width * bytes_per_pixel(format)`.
3. The `gud_fb_create()` function validates the GEM object is large enough for
   the framebuffer dimensions and format.
4. The `gud_xdisp_buffers_init()` function allocates buffers for the worst-case
   format (XRGB8888, 4 bytes/pixel) to ensure all formats are supported.
5. The Pi gadget validates buffer request length against `bytes_per_pixel(format)`.
6. The mirgud client validates the `--pixel-format` option and passes it through
   to the GUD KMS driver for framebuffer allocation.

## Files Changed

### mir-android2-platform-gud
- `src/utils/gud_screencast_format.h` (modified): Fixed DRM format values,
  added ConversionPath, TimingSummary, SourceFormat, parse_pixel_format,
  direct-copy fast path, deterministic test pattern generators,
  rgb888_to_rgb565/rgb888_to_xrgb8888/frame_to_rgb888 conversion helpers,
  write_ppm_rgb888
- `src/utils/gud_screencast.cpp` (modified): Fixed timing denominators,
  added TimingSummary-based Stats, conversion_path instrumentation,
  runtime Mir format recording, --quality mode, --pattern/--no-gud descriptions
- `doc/compare_frames.py` (modified): Deterministic quality workflow,
  MAE/RMSE/PSNR, removed invalid SSIM/banding claims
- `tests/unit-tests/xdisp/test_pixel_format.cpp` (new): Focused unit tests
- `tests/unit-tests/xdisp/CMakeLists.txt` (modified): Added test target

### gud (host driver)
- `backport-4.9/gud_protocol.h`: Added `GUD_PIXEL_FORMAT_XRGB8888`
- `backport-4.9/gud_internal.h`: Added `current_format` field
- `backport-4.9/gud_pipe.c`: Format-aware `gud_formats[]`, `gud_fb_create()`,
  `gud_pipe_check()`, `gud_pipe_state_check()`, `gud_pipe_transfer()`,
  `gud_pipe_transfer_xdisp()`, `gud_xdisp_buffers_init()`
- `backport-4.9/gud_gem_4_9.c`: `gud_gem_dumb_create()` supports 16/32 bpp

### gud-gadget (Pi)
- `drm/src/main.rs`: Added XRGB8888 to `TransferFormat` enum, updated `DrmScanoutBackend::create_buffer()` and `add_framebuffer()` to use format-aware DRM fourcc and bpp
- `drm/src/scanout.rs`: Updated `logical_memory_estimate()` to accept `bytes_per_pixel` parameter, renamed `checked_rgb565_bytes()` to `checked_bytes()`, removed `RGB565_BYTES_PER_PIXEL` constant
- `drm/src/main.rs`: Updated `ShadowFramebuffer::activate_scaled()` to use format-aware bytes per pixel
- `drm/src/main.rs`: Updated buffer copy paths to use direct `copy_buffer_to_framebuffer_with_stats()` for all formats (removed `copy_rgb888_to_rgb565_framebuffer` and `rgb888_to_rgb565`)
- `gadget/src/lib.rs`: Made `bytes_per_pixel()` public
