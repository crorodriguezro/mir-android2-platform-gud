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
mirgud: virtual frame source is CPU mapped 1280x720 format=<N> stride=<S>
```

**E0.1 observation:** stride of ~5120 at width 1280 → 5120 / 1280 = 4
bytes/pixel → confirms a 4-byte-per-pixel source (XRGB8888 or equivalent).

The `convert_row_to_rgb565()` function in `gud_screencast_rgb565.h` handles
multiple Mir pixel formats:

| Mir pixel format enum              | LE memory layout | bytes/pixel |
|------------------------------------|------------------|-------------|
| `mir_pixel_format_abgr_8888`       | RGBA             | 4           |
| `mir_pixel_format_xbgr_8888`       | RGBX             | 4           |
| `mir_pixel_format_argb_8888`       | BGRA             | 4           |
| `mir_pixel_format_xrgb_8888`       | BGRX             | 4           |
| `mir_pixel_format_rgb_888`         | RGB              | 3           |
| `mir_pixel_format_bgr_888`         | BGR              | 3           |
| `mir_pixel_format_rgb_565`         | RGB565           | 2           |

The EGL fallback path (`EglCapture`) uses `GL_BGRA_EXT` or `GL_RGBA` and
converts to `mir_pixel_format_argb_8888` or `mir_pixel_format_abgr_8888`.

## 2. GUD DRM framebuffer format

**Files:** `src/utils/gud_screencast.cpp`, `gud/backport-4.9/gud_pipe.c`

`GudKms::allocate()` creates dumb buffers with `bpp = 16` and
`DRM_FORMAT_RGB565`:

```cpp
frame.dumb.bpp = 16;
drmModeAddFB2(fd, width, height, DRM_FORMAT_RGB565, ...)
```

`gud_fb_create()` in `gud_pipe.c` rejects any format != `DRM_FORMAT_RGB565`:

```c
if (mode_cmd->pixel_format != DRM_FORMAT_RGB565)
    return ERR_PTR(-EINVAL);
```

`gud_pipe_check()` also rejects non-RGB565:

```c
if (plane_state->fb->pixel_format != DRM_FORMAT_RGB565)
    return -EINVAL;
```

`gud_formats[]` array contains only `DRM_FORMAT_RGB565`.

`gud_pipe_state_check()` sends `GUD_PIXEL_FORMAT_RGB565` in the state check
request.

## 3. GUD host driver supported formats

**File:** `gud/backport-4.9/gud_protocol.h`

The GUD protocol only defines:

```c
#define GUD_PIXEL_FORMAT_RGB565 0x40
```

The host driver hardcodes RGB565 everywhere.

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

`DrmScanoutBackend::create_buffer()` creates `DrmFourcc::Rgb565` dumb buffers.

`DrmScanoutBackend::add_framebuffer()` adds RGB565 framebuffers.

The Pi DRM scanout is hardcoded to RGB565.

The `TransferFormat` enum in `main.rs` has `Rgb565` and `Rgb888` options
(selected via `GUD_TRANSFER_FORMAT` env var), but not XRGB8888.

## 6. XRGB8888 support end-to-end

| Component           | RGB565 | XRGB8888 |
|---------------------|--------|----------|
| Mir source          | No     | Yes (native, 4 bpp) |
| mirgud conversion   | Yes    | No (only RGB565 output) |
| GUD DRM framebuffer | Yes    | No (hardcoded RGB565) |
| GUD protocol        | Yes    | No (only RGB565 defined) |
| Pi gadget protocol  | Yes    | Yes (advertised) |
| Pi DRM scanout      | Yes    | No (hardcoded RGB565) |

## Architecture Table

```
Component                RGB565     XRGB8888
------------------------------------------------
Mir source               No         Yes (native 4 bpp, stride ~5120@1280)
mirgud conversion        Yes        No (output hardcoded to RGB565)
GUD DRM framebuffer      Yes        No (DRM_FORMAT_RGB565 only)
host GUD support         Yes        No (GUD_PIXEL_FORMAT_RGB565 only)
Pi gadget support        Yes        Yes (GUD_PIXEL_FORMAT_XRGB8888 advertised)
Pi scanout support       Yes        No (DrmFourcc::Rgb565 only)
```

## Conclusion

The Mir source is already XRGB8888 (4 bytes/pixel, confirmed by stride ~5120
at width 1280). The entire pipeline from Mir through the host GUD driver to
the Pi DRM scanout is hardcoded to RGB565. The Pi gadget protocol layer
already advertises XRGB8888 but the DRM scanout backend does not use it.

To support XRGB8888 end-to-end, the following changes are needed:

1. **mirgud:** Add `--pixel-format xrgb8888` option, change `Frame` to use a
   format-aware representation, add direct XRGB8888 capture path.
2. **GUD host driver:** Add `GUD_PIXEL_FORMAT_XRGB8888` to protocol, add
   XRGB8888 to `gud_formats[]`, `gud_fb_create()`, `gud_pipe_check()`, make
   the 12.8KB cap format-aware.
3. **gud-gadget:** Add XRGB8888 to `TransferFormat` enum, add XRGB8888 DRM
   dumb buffer creation, add XRGB8888 framebuffer copy path.
