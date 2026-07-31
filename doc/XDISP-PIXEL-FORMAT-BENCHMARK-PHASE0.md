# XDISP Phase 0 — Pixel Format Source and Format Audit

## 1. Mir source format

**File:** `src/utils/gud_screencast.cpp`

The `DirectCapture` class reads from `MirBufferStream` via `graphics_region()`,
which returns a `MirGraphicsRegion` containing `pixel_format`, `vaddr`,
`stride`, `width`, and `height`.

The Mir pixel format is whatever Mir provides, queried at runtime via
`mir_connection_get_available_surface_formats()`. The code logs the format and
stride:

```
mirgud: virtual frame source is CPU mapped 1280x720 mir_format=<N> stride=<S>
        row_order=top-down transport=xrgb8888 conversion_path=direct-copy
```

**Runtime evidence:** stride of ~5120 at width 1280 -> 5120 / 1280 = 4
bytes/pixel -> confirms a 4-byte-per-pixel source. The exact MirPixelFormat
enum value is recorded at runtime in the `SourceFormat` struct.

The exact MirPixelFormat enum value is recorded at runtime and used to select
the conversion path. Do NOT assume the source is XRGB8888 from stride alone.

### Mir pixel format memory layouts (little-endian)

Per `mir_toolkit/common.h`:

> 32-bit pixel formats (8888): The order of components in the enum matches the
> order of the components as they would be written in an integer representing
> a pixel value of that format. For example; abgr_8888 should be coded as
> 0xAABBGGRR, which will end up as R,G,B,A in memory on a little endian
> system.

| Mir pixel format enum              | Integer value  | LE memory layout | bytes/pixel |
|------------------------------------|----------------|-------------------|-------------|
| `mir_pixel_format_abgr_8888`       | 0xAABBGGRR     | R, G, B, A        | 4           |
| `mir_pixel_format_xbgr_8888`       | 0x00BBGGRR     | R, G, B, X        | 4           |
| `mir_pixel_format_argb_8888`       | 0xAARRGGBB     | B, G, R, A        | 4           |
| `mir_pixel_format_xrgb_8888`       | 0x00RRGGBB     | B, G, R, X        | 4           |
| `mir_pixel_format_bgr_888`         | N/A            | B, G, R           | 3           |
| `mir_pixel_format_rgb_888`         | N/A            | R, G, B           | 3           |
| `mir_pixel_format_rgb_565`         | 16-bit         | R:G:B 5:6:5       | 2           |

### DRM_FORMAT_XRGB8888 memory layout (little-endian)

From `<drm/drm_fourcc.h>`:

```c
#define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4')
/* [31:0] x:R:G:B 8:8:8:8 little endian */
```

DRM format comments specify the layout for a little-endian system:
- Integer value: 0x00RRGGBB
- LE memory: [B, G, R, X]

### Byte-for-byte compatibility

`mir_pixel_format_xrgb_8888` (integer 0x00RRGGBB, LE memory [B, G, R, X])
is the **only** Mir format whose little-endian memory representation is
byte-for-byte identical to `DRM_FORMAT_XRGB8888` (also [B, G, R, X]).

All other 4-byte Mir formats require channel reordering:
- `mir_pixel_format_abgr_8888` → [R, G, B, A] → reorder to [B, G, R, X]
- `mir_pixel_format_xbgr_8888` → [R, G, B, X] → reorder to [B, G, R, X]
- `mir_pixel_format_argb_8888` → [B, G, R, A] → zero alpha byte

The EGL fallback path (`EglCapture`) uses `GL_BGRA_EXT` or `GL_RGBA` and
converts to `mir_pixel_format_argb_8888` or `mir_pixel_format_abgr_8888`.

## 2. GUD DRM framebuffer format

**Files:** `src/utils/gud_screencast.cpp`, `gud/backport-4.9/gud_pipe.c`

`GudKms::allocate()` creates dumb buffers with `bpp = bytes_per_pixel(format) * 8`
and uses `mirgud::drm_format(pixel_format)` for the framebuffer format:

```cpp
frame.dumb.bpp = mirgud::bytes_per_pixel(pixel_format) * 8;
drmModeAddFB2(fd, width, height, mirgud::drm_format(pixel_format), ...)
```

`gud_fb_create()` in `gud_pipe.c` accepts both `DRM_FORMAT_RGB565` and
`DRM_FORMAT_XRGB8888`.

`gud_pipe_check()` validates framebuffer pitch against `bytes_per_pixel(format)`.

`gud_pipe_state_check()` sends the format-aware GUD pixel format.

## 3. GUD host driver supported formats

**File:** `gud/backport-4.9/gud_protocol.h`

The GUD protocol defines:

```c
#define GUD_PIXEL_FORMAT_RGB565   0x40
#define GUD_PIXEL_FORMAT_XRGB8888 0x80
```

The host driver supports both formats in `gud_formats[]`, `gud_fb_create()`,
and `gud_pipe_check()`.

## 4. Pi gadget advertised formats

**File:** `gud-gadget/gadget/src/lib.rs`

The gud-gadget library defines three pixel formats:

```rust
pub const GUD_PIXEL_FORMAT_RGB565: u8 = 0x40;
pub const GUD_PIXEL_FORMAT_RGB888: u8 = 0x50;
pub const GUD_PIXEL_FORMAT_XRGB8888: u8 = 0x80;
```

The `bytes_per_pixel()` function supports all three (2, 3, 4 respectively).

The `validate_state_check_payload()` validates that the format is in the
configured formats list.

The `validate_buffer_request()` validates buffer length against
`bytes_per_pixel(state.format)`.

The `dump_pixel_buffer_ppm()` function handles bpp 2, 3, and 4.

The `copy_buffer_to_framebuffer()` function copies pixels with the given bpp.

## 5. DRM scanout on Pi

**File:** `gud-gadget/drm/src/main.rs`

`DrmScanoutBackend::create_buffer()` creates format-aware dumb buffers using
`DrmFourcc::Rgb565` or `DrmFourcc::Xrgb8888`.

`DrmScanoutBackend::add_framebuffer()` adds format-aware framebuffers.

The Pi DRM scanout supports both RGB565 and XRGB8888.

The `TransferFormat` enum in `main.rs` has `Rgb565`, `Rgb888`, and `Xrgb8888`
options (selected via `GUD_TRANSFER_FORMAT` env var).

## 6. XRGB8888 support end-to-end

| Component           | RGB565 | XRGB8888 |
|---------------------|--------|----------|
| Mir source          | No     | Yes (runtime evidence confirms 4 bytes/pixel; exact enum recorded) |
| mirgud conversion   | Yes    | Yes (direct-copy when mir_pixel_format_xrgb_8888, otherwise channel-reorder) |
| GUD DRM framebuffer | Yes    | Yes (DRM_FORMAT_XRGB8888) |
| GUD protocol        | Yes    | Yes (GUD_PIXEL_FORMAT_XRGB8888) |
| Pi gadget protocol  | Yes    | Yes (GUD_PIXEL_FORMAT_XRGB8888 advertised) |
| Pi DRM scanout      | Yes    | Yes (DrmFourcc::Xrgb8888) |

## Architecture Table

```
Component                RGB565     XRGB8888
------------------------------------------------
Mir source               No         Yes (runtime evidence confirms 4 bytes/pixel; exact enum recorded)
mirgud conversion        Yes        Yes (direct-copy when mir_pixel_format_xrgb_8888, otherwise channel-reorder)
GUD DRM framebuffer      Yes        Yes (DRM_FORMAT_XRGB8888)
host GUD support         Yes        Yes (GUD_PIXEL_FORMAT_XRGB8888)
Pi gadget support        Yes        Yes (GUD_PIXEL_FORMAT_XRGB8888 advertised)
Pi scanout support       Yes        Yes (DrmFourcc::Xrgb8888)
```

## Conclusion

Runtime evidence confirms a 4-byte-per-pixel source. The exact MirPixelFormat
enum value is recorded at runtime and used to select the conversion path.

When the Mir source is `mir_pixel_format_xrgb_8888`, the XRGB8888 transport
uses a direct row memcpy because the source memory is byte-for-byte compatible
with `DRM_FORMAT_XRGB8888` on this little-endian target.

The entire pipeline from Mir through the host GUD driver to the Pi DRM scanout
supports both RGB565 and XRGB8888.

## Prerequisites Before Hardware Benchmark

1. Verify the runtime MirPixelFormat enum value matches `mir_pixel_format_xrgb_8888`
   (or another 4-byte format) by inspecting the `conversion_path` log output.
2. Confirm the GUD host driver accepts `DRM_FORMAT_XRGB8888` in `gud_fb_create()`.
3. Confirm the Pi gadget advertises `GUD_PIXEL_FORMAT_XRGB8888` in its format list.
4. Run the deterministic quality comparison (`--quality --dump-frame`) to establish
   the quantization baseline before measuring CPU/perf metrics.
