# Mir and Venus format qualification

Date: 2026-08-28 UTC

Resolution: 1920x1080

## Public Mir screencast formats

The audit created a real legacy public-Mir screencast for every request and queried the returned buffer. The server-advertised list contained only ABGR8888, XBGR8888, RGB888, and RGB565, but ARGB8888 and XRGB8888 also worked when explicitly requested. Accepted requests were not silently substituted.

| Requested | Accepted | Actual | Stride | Little-endian memory bytes | Public capture usable |
|---|---|---|---:|---|---|
| ABGR8888 (1) | yes | ABGR8888 | 7680 | R,G,B,A | yes; selected source |
| XBGR8888 (2) | yes | XBGR8888 | 7680 | R,G,B,X | yes |
| ARGB8888 (3) | yes | ARGB8888 | 7680 | B,G,R,A | yes, although not advertised |
| XRGB8888 (4) | yes | XRGB8888 | 7680 | B,G,R,X | yes, although not advertised |
| BGR888 (5) | no | none | n/a | n/a | no |
| RGB888 (6) | yes | RGB888 | 5760 | R,G,B | yes; no matching Venus RGB input |
| RGB565 (7) | yes | RGB565 | 3840 | little-endian RGB565 | yes; no matching Venus RGB input |

## Venus encoder OUTPUT-queue formats

`VIDIOC_ENUM_FMT` and authoritative `VIDIOC_S_FMT` were run on the OnePlus 6 encoder. This driver returns `ENOTTY` for every `VIDIOC_TRY_FMT`, so TRY_FMT cannot be used as an acceptance result. All accepted formats returned one plane, `bytesperline=0`, Rec.709 colorspace (3), encoding 2, limited quantization (2), and transfer function 1. The opaque driver alignment is represented by the returned `sizeimage`; it must not be inferred from the visible width.

| Fourcc | Description | S_FMT | Returned dimensions | Planes | bytesperline | sizeimage |
|---|---|---|---|---:|---:|---:|
| NV12 | Y/CbCr 4:2:0 | accepted | 1920x1080 NV12 | 1 | 0 | 3153920 |
| Q128 | NV12 UBWC | accepted | 1920x1080 Q128 | 1 | 0 | 3313664 |
| RGB4 | 32-bit A/XRGB 8-8-8-8 | accepted | 1920x1080 RGB4 | 1 | 0 | 8355840 |
| NV21 | Y/CrCb 4:2:0 | accepted | 1920x1080 NV21 | 1 | 0 | 3153920 |
| Q12A | TP10 UBWC | accepted | 1920x1080 Q12A | 1 | 0 | 4333568 |
| QP10 | P10 Venus | accepted | 1920x1080 QP10 | 1 | 0 | 6303744 |

Rejected by `S_FMT`: NV12M (`NM12`), BGR4, AR24, XR24, AB24, XB24, BA24, BX24, RGB3, BGR3, and RGB565 (`RGBP`).

## Direct compatibility matrix

| Mir memory layout | Venus candidate | Result |
|---|---|---|
| ABGR8888 / R,G,B,A | RGB4 | encoded and decoded, but corrupt colors/geometry |
| XBGR8888 / R,G,B,X | RGB4 | encoded and decoded, but corrupt colors/geometry |
| ARGB8888 / B,G,R,A | RGB4 | encoded and decoded, but corrupt colors/geometry |
| XRGB8888 / B,G,R,X | RGB4 | encoded and decoded, but corrupt colors/geometry |
| RGB888 | RGB3/BGR3 | Venus rejects both |
| RGB565 | RGBP | Venus rejects it |
| any Mir public format | NV12/NV21 | Mir public screencast does not produce either |

All four RGB4 trials decoded to the same incorrect image SHA-256 (`c511da...`) despite different, memory-correct channel layouts. STREAMON/S_FMT success is therefore not treated as compatibility. An earlier live-Mir RGB4 run was likewise green/corrupt.

## Selected production format

Mir ABGR8888, real stride honored, is converted by the optimized BT.709 limited-range ABGR-to-NV12 implementation and submitted to Venus as NV12. The scalar converter remains selectable with `MIR_VENUS_SCALAR=1`. Direct `rgb4` remains an audit mode, not the production default.
