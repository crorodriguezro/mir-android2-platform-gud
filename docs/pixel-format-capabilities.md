# Deployed Mir Pixel-Format Capabilities

Status: **verified for the deployed OnePlus 6 stack**

This document records the pixel-format capability decision that controls how
`mirgud` should obtain frames. It is deployment-specific: it describes the
Mir/Android2 stack installed on the OnePlus 6, not generic or current upstream
Mir behavior.

The canonical raw and derived proof is retained in
[`gud-gadget/evidence/xdisp-mir-format-capability-20260824T003458Z/`](../../gud-gadget/evidence/xdisp-mir-format-capability-20260824T003458Z/).
That bundle contains the exact commands, package versions, runtime logs,
format matrix, measurements, and checksums.

## Environment

| Component | Deployed value |
| --- | --- |
| Device | OnePlus 6 |
| OS | Ubuntu Touch Noble / Ubuntu 24.04.4 |
| Kernel | `4.9.112-g6b190d86b` |
| Mir | `1.8.3` runtime libraries |
| Lomiri | `0.5.0` |
| Graphics path | Android2 / libhybris / Android HAL gralloc |
| Production Mir socket | `/run/mir_socket` |

## Capability matrix

The requested format is the format passed to the Mir screencast source. “True
packed” means the returned CPU-mappable `MirGraphicsRegion` has the expected
bytes per pixel and stride; it is not merely a 32-bit buffer with a narrower
logical format.

| Format requested from Mir | Mir advertises / accepts | Actual buffer | `mirgud` status |
| --- | --- | --- | --- |
| ABGR8888 | Yes; selected by `auto` | 4 Bpp, stride 5120 at 1280 px | Current production source path; channel-reordered to XRGB8888 transport |
| XRGB8888 | No; request rejected as not advertised | — | Cannot request directly from deployed Mir |
| RGB888 | Yes | True packed 3 Bpp, stride 3840 at 1280 px | Mir works; source-side capture works; current transport plumbing has no direct RGB888 format |
| RGB565 | Yes | True packed 2 Bpp, stride 2560 at 1280 px | Direct path works when RGB565 is selected for transport |

The runtime-advertised Mir list is exactly:

```text
abgr8888, xbgr8888, rgb888, rgb565
```

Therefore the production source is **ABGR8888, not XRGB8888**. The managed
production child requests transport `xrgb8888` but leaves the Mir source format
at `auto`; the deployed backend chooses ABGR8888 (Mir enum 1), and `mirgud`
performs the channel reorder required for the current transport.

## Architecture decision

Current production path:

```text
Mir ABGR8888
    -> mirgud channel reorder
    -> XRGB8888 GUD transport
```

Selected E2 v1 transport candidate:

```text
Mir RGB565 directly
    -> mirgud
    -> RGB565 GUD transport
```

The selected candidate adds LZ4 and the already-qualified large logical-frame
transport:

```text
Lomiri
    -> unmodified Mir 1.8.3
    -> direct packed RGB565 screencast
    -> mirgud, no intermediate XRGB8888 conversion
    -> GUD RGB565
    -> LZ4
    -> USB
    -> Pi Zero 2 W FunctionFS GUD gadget
    -> RGB565 framebuffer / VC4 DRM
    -> HDMI
```

At 1280x720, the full RGB565 logical frame is 1,843,200 bytes. Direct Mir
RGB565 + LZ4 presented 22.62 fps, exceeding the project target of at least
20 presented fps. RGB565 RAW presented 18.16 fps and remains the simpler
fallback without a compression dependency. The managed qualification observed
zero ambiguous accepted I/O, poisoned transitions, timeouts, and ownership
ambiguity.

Direct Mir RGB565 + LZ4 is the selected **E2 v1 transport candidate for
sustained qualification**, not the final production default. E2-T04 is
unpaused and is the next gate; E5-T03 remains responsible for final
apples-to-apples release-default and image-quality qualification.

No intermediate XRGB8888-to-RGB565 conversion is required. The deployed Mir
screencast path accepted RGB565 and returned CPU-mappable 2-byte pixels; the
existing `mirgud` RGB565 transport path direct-copied all acquired frames.

RGB888 is also real packed delivery at the Mir buffer boundary, but the current
`mirgud` transport format enum and host/GUD path support RGB565 and XRGB8888,
not RGB888. It therefore remains a source-capability result rather than the
next end-to-end transport experiment.

## Mir API and backend boundary

The relevant capture sequence is:

```text
mir_connection_get_available_surface_formats()
    -> mir_screencast_spec_set_pixel_format()
    -> mir_screencast_create_sync()
    -> mir_screencast_get_buffer_stream()
    -> mir_buffer_stream_get_graphics_region()
```

The Android2 allocator advertises `abgr8888`, `xbgr8888`, `rgb888`, and
`rgb565`, mapping them to the corresponding Android HAL formats and returning
the native gralloc stride. Lomiri owns the Virtual output and compositor
integration, but the tested format acceptance is Mir API/backend behavior; no
Lomiri format restriction was found.

Relevant implementation references:

- [`src/utils/gud_screencast.cpp`](../src/utils/gud_screencast.cpp)
- [`src/utils/gud_screencast_format.h`](../src/utils/gud_screencast_format.h)
- [`src/platforms/android/server/graphic_buffer_allocator.cpp`](../src/platforms/android/server/graphic_buffer_allocator.cpp)
- [`src/platforms/android/include/android_format_conversion-inl.h`](../src/platforms/android/include/android_format_conversion-inl.h)
- [`src/platforms/android/server/gralloc_module.cpp`](../src/platforms/android/server/gralloc_module.cpp)
- [`src/platforms/android/server/buffer.cpp`](../src/platforms/android/server/buffer.cpp)
- [`../gud/backport-4.9/gud_pipe.c`](../../gud/backport-4.9/gud_pipe.c)
- [`../../gud-gadget/gadget/src/lib.rs`](../../gud-gadget/gadget/src/lib.rs)

## Qualified performance and evidence

| Path | Logical bytes/frame | Presented fps | Result |
| --- | ---: | ---: | --- |
| XRGB8888 RAW | 3,686,400 | ~9 | Existing baseline |
| XRGB8888 + LZ4 | 3,686,400 | 13.27 | Existing baseline |
| Direct Mir RGB565 RAW | 1,843,200 | 18.16 | Simpler fallback |
| Direct Mir RGB565 + LZ4 | 1,843,200 | **22.62** | Selected candidate; exceeds target |

Canonical evidence bundles:

- [Mir pixel-format capability audit](../../gud-gadget/evidence/xdisp-mir-format-capability-20260824T003458Z/)
- [full-frame RAW qualification](../../gud-gadget/evidence/xdisp-e1-fullframe-managed-scaling-20260823T223143Z/)
- [large-frame LZ4 qualification](../../gud-gadget/evidence/xdisp-e2-bandwidth-damage-20260823T232018Z/)
- [direct Mir RGB565 qualification](../../gud-gadget/evidence/xdisp-e2-direct-mir-rgb565-20260824T010040Z/)

## Scope and follow-up

- This audit and the transport qualification select the E2 v1 candidate but do
  not select the final release default; sustained resource qualification and
  final image-quality/release comparison remain roadmap gates.
- Do not add XRGB8888-to-RGB565 conversion unless new evidence shows that
  direct RGB565 delivery is unusable.
- The audit did not instrument whether Mir internally renders or converts into
  the returned buffer. It proves the format, stride, packed size, and changing
  content of the buffers delivered across the Mir screencast boundary.
- The evidence bundles are immutable canonical proof; update this document only
  when new deployment-specific evidence changes the capability or transport
  decision.
