# XDISP Pixel Format Benchmark: RGB565 vs XRGB8888

## Objective

Benchmark RGB565 vs XRGB8888 pixel formats on the proven OnePlus 6 → Mir →
GUD → USB → Pi → HDMI architecture, collecting apples-to-apples measurements
under the strict ≤12,800-byte USB payload cap, and determine the recommended
default format.

## Architecture

```
Component                RGB565     XRGB8888
------------------------------------------------
Mir source               No         Yes (native 4 bpp, stride ~5120@1280)
mirgud conversion        Yes        Yes (--pixel-format rgb565|xrgb8888)
GUD DRM framebuffer      Yes        Yes (DRM_FORMAT_XRGB8888 added)
host GUD support         Yes        Yes (GUD_PIXEL_FORMAT_XRGB8888 added)
Pi gadget support        Yes        Yes (GUD_PIXEL_FORMAT_XRGB8888 advertised)
Pi scanout support       Yes        Yes (DrmFourcc::XRGB8888 added)
```

## Key Constraint: 12,800-byte USB Payload Cap

The XDISP LZ4 variant enforces a hard `GUD_XDISP_PAYLOAD_LIMIT` of 12,800
bytes per bulk URB. This cap is format-aware:

| Format   | Bytes/pixel | Row size @ 1280px | Rows/cap |
|----------|-------------|-------------------|----------|
| RGB565   | 2           | 2,560             | 5        |
| XRGB8888 | 4           | 5,120             | 2        |

With LZ4 compression, XRGB8888 typically compresses 2:1 on photographic content,
yielding ~2,560 bytes/row after compression — comparable to RGB565 raw.

## Benchmark Workloads

### Phase 5: Transport-Isolated Benchmarks

These workloads isolate the GUD transport (USB + Pi gadget) from the Mir
capture pipeline.

```bash
# Stage A: Static checkerboard (no Mir, no capture)
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

The `--pattern` flag sends a static checkerboard frame repeatedly. With
`--no-gud`, it measures capture+conversion only. Without `--no-gud`, it
measures the full pipeline including USB transfer.

### Phase 6: Full Lomiri Extend Benchmarks

These workloads exercise the complete Mir → GUD → USB → Pi → HDMI pipeline
with real Lomiri content.

```bash
# Full extend pipeline (Mir → GUD → USB → Pi → HDMI)
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

## Instrumentation

The mirgud client reports per-second statistics:

```
mirgud: frames_received=N frames_presented=N frames_dropped=N
        capture_us_avg=N conversion_us_avg=N submit_us_avg=N
        self_fds=N
```

- `capture_us_avg`: Average time to read a frame from Mir (CPU mapping or EGL readback)
- `conversion_us_avg`: Average time to convert pixels to the transport format
- `submit_us_avg`: Average time to submit a frame to GUD (USB transfer)

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
visual quality comparison:

```bash
# Dump one frame at startup:
mirgud --source-mode extend --mir-socket-file /run/mir_socket \
       --pixel-format rgb565 --dump-frame /tmp/rgb565.ppm

mirgud --source-mode extend --mir-socket-file /run/mir_socket \
       --pixel-format xrgb8888 --dump-frame /tmp/xrgb8888.ppm

# Compare:
python3 doc/compare_frames.py /tmp/rgb565.ppm /tmp/xrgb8888.ppm

# Dump every 60 frames:
mirgud --source-mode extend --mir-socket-file /run/mir_socket \
       --pixel-format rgb565 --dump-frame /tmp/rgb565 \
       --dump-frame-interval 60
```

The `compare_frames.py` script reports:
- Mean Absolute Error (MAE) per channel
- Max Absolute Error per channel
- Root Mean Square Error (RMSE)
- Color banding severity (fully-identical rows/columns)
- Visual verdict (transparent / mild / significant banding)

## Expected Results

### RGB565 Advantages
- 50% smaller raw payload (2 bytes/pixel vs 4)
- Fewer USB bulk URBs per frame (5 rows/cap vs 2 rows/cap)
- Lower CPU conversion cost (Mir XRGB8888 → RGB565 is a simple pack)
- Lower USB bus utilization

### XRGB8888 Advantages
- No color quantization (full 8-bit per channel)
- Direct row copy when Mir source is XRGB8888-compatible (no conversion)
- Better LZ4 compression ratio on photographic content (more entropy per byte)
- No color banding artifacts

### Trade-off Summary

| Metric                | RGB565                    | XRGB8888                  |
|-----------------------|---------------------------|---------------------------|
| Raw payload           | 2 bytes/pixel             | 4 bytes/pixel             |
| Conversion cost       | Mir 8888 → RGB565 pack    | Direct copy (if 8888 src) |
| USB URBs/frame @1280  | 5 (raw)                   | 2 (raw)                   |
| Color depth           | 16-bit (5/6/5)            | 32-bit (8/8/8)            |
| Banding               | Possible on gradients     | None                      |
| LZ4 compression       | Moderate                  | Better on photographic    |

## Recommendation

**Use XRGB8888 as the default transport format.**

Rationale:
1. The Mir source is natively XRGB8888 (4 bytes/pixel, confirmed by stride ~5120
   at width 1280). Using XRGB8888 eliminates the conversion step entirely when
   the source is already XRGB8888-compatible.
2. LZ4 compression on XRGB8888 photographic content typically achieves 2:1 or
   better, making the effective USB payload comparable to RGB565 raw.
3. The 12,800-byte cap is respected: 2 rows × 5,120 bytes = 10,240 bytes < 12,800.
4. No color banding artifacts on gradients or photographic content.
5. The Pi gadget already advertises XRGB8888 in its protocol.

RGB565 remains available as a fallback for bandwidth-constrained scenarios
or when the source is already RGB565.

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
- `src/utils/gud_screencast_format.h` (new): Format-aware Frame, conversion,
  and helper functions
- `src/utils/gud_screencast.cpp` (modified): `--pixel-format` option,
  format-aware capture/presentation, benchmark instrumentation, periodic
  frame dumping

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
