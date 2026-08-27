# H.264 USB display physical-hardware evidence

Result: **PASS-A POC** on 2026-08-27.  A changing 1920x1080 Mir external
desktop was captured on the OnePlus 6, hardware-encoded by Qualcomm Venus,
sent over the physical USB2 cable, hardware-decoded by the Pi Zero 2 W
`bcm2835-codec`, and handed to VLC `drm_vout` as DRM_PRIME for 1920x1080 HDMI.

## Revisions and binaries

- Repository branch: `poc/h264-usb-display`
- Phone kernel: `4.9.112-g6b190d86b`, aarch64
- Pi kernel: `6.12.47+rpt-rpi-v8-ffs-xfercompltrace`, aarch64
- Sender SHA-256: `0cb0e4a649f0c573940437fd2d87ee87efc71bbb8a3538d629986851d3eb0d40`
- Receiver shim SHA-256: `4a7eed696aea2eaa7c40ec5770b0e30f2b6eed43b642638c642e99e70af8f84c`
- Both sources build with `-O2 -Wall -Wextra -Werror`.

## Passed gates

| Gate | Hardware result |
| --- | --- |
| Mir capture | Public legacy Mir screencast ABI, ABGR8888, external region `1920x1080+1080+0` |
| Phone encode | `/dev/video33`, V4L2 M2M Qualcomm Venus, RGB32 input, H.264 output, four ION buffers |
| Local sustained encode | 900/900 frames in 30.035 s, 29.97 FPS, 10.99 Mb/s; ffprobe decoded all 900 |
| USB transport | Temporary ECM gadget over the physical USB2 link; 4 MiB in 0.488 s = 68.8 Mb/s at MTU 512 |
| Pi decode capacity | `h264_v4l2m2m`, driver `bcm2835-codec`, all 900 frames at about 64 FPS, throttled `0x0` |
| Decoder input size | Shim requested and driver accepted 2,097,152 bytes; final live run had zero FFmpeg V4L2 overflow |
| DRM/HDMI | `drm_prime:yuv420p`, VLC `drm_vout`, 1920x1080 HDMI, first picture received |
| Final live run | 900/900 access units, 30.120 s, **29.88 FPS**, 6.49 Mb/s, largest AU 1,645,040 bytes, queue depth 4, drained |
| Drop/error result | Zero V4L2 input overflow and zero sustained late-frame-drop warnings in the low-overhead run |
| Production rollback | Temporary gadget removed; `gud-userspace.service=active`, UDC `configured`; phone host found `1d50:614d` at 480 Mb/s and reprobed `/dev/dri/card1` |
| Rollback content test | Production GUD accepted one new RGB565/LZ4 1280x720 update and logged `Presented first valid content frame` |

29.88 FPS is the measured end-to-end sender rate including final encoder drain;
the source cadence and MPEG-TS timestamps are exactly 30/1.  Attempting 31 Hz
proved that Mir itself is capped near 30 Hz and caused timestamp lateness, so
30 Hz is the correct operating point.

## Bottlenecks found

1. The old `mirgud` scalar ABGR-to-XRGB conversion is the dominant legacy
   source-side cost: about 50.6 ms/frame and 17 FPS at 1080p.  Direct ABGR copy
   into Venus averages 3.9-5.1 ms and avoids that conversion.
2. Mir screencast/swap is capped at approximately 30 Hz (about 5.3-5.9 ms CPU
   wait/call cost plus compositor cadence).  It cannot produce a genuine 31
   Hz stream on this image.
3. Venus produces content-dependent IDRs up to 1.79 MB.  The available kernel
   exposes bitrate and B-frame controls but rejects the desired rate-control,
   GOP, and I-QP controls; lowering nominal bitrate did not shrink IDRs.
4. Pi distro FFmpeg requested only 1,632,256 bytes per compressed V4L2 buffer.
   This was the direct cause of dropped IDRs.  A scoped 2 MiB `sizeimage` shim
   removes the overflow; the hardware decoder itself has ample capacity.
5. USB ECM at MTU 1500 stalls after TCP setup on this DWC2 path.  MTU 512 is
   reliable but caps measured transport at 68.8 Mb/s, still far above video.
6. VLC `-vvv` per-buffer logging reduces the live path to about 26.6 FPS due to
   Pi storage/log overhead.  Normal `-v` operation reaches 29.88 FPS.

## Measurements not claimed

No high-speed-camera input-to-photon measurement and no synchronized phone/Pi
CPU utilization trace were captured, so no latency or CPU percentage is
fabricated.  Short VLC lateness warnings were 19-196 ms during startup/IDR
bursts; those are scheduler lateness, not a calibrated photon-latency result.
Peak USB bitrate per video burst was not instrumented; the largest encoded
access unit and independent USB capacity are reported instead.

## Failure/retry history retained on devices

- Phone: `/tmp/mir-venus-performance-sender.log`
- Pi: `/tmp/mir-venus-performance-vlc.log`
- Pi verbose decode/DRM proof: `/tmp/mir-venus-live30-vlc-rerun.log`
- Pi final no-overflow verbose proof: `/tmp/mir-venus-final-vlc.log`
- Pi production restoration: `journalctl -u gud-userspace.service`

## Next gated course of action

1. Replace the preload shim with a small receiver or FFmpeg patch that requests
   2 MiB natively; gate: 900/900 decoded with no preload and no overflow.
2. Replace ECM/TCP with a FunctionFS bulk function using 64 KiB asynchronous
   transfers; gate: reconnect-safe 30-minute run, bounded queue <=4 frames,
   zero stale-frame growth.
3. Add periodic SPS/PPS plus forced IDR recovery when the kernel exposes a
   working control; gate: unplug/replug recovers visible HDMI within 2 s.
4. Add monotonic capture/receive/present telemetry and a high-speed-camera
   test; gate: report p50/p95 input-to-photon latency separately from FPS.
5. Run controlled static/text/scroll/high-motion A/B captures against
   production GUD, including phone/Pi CPU and visual text inspection; gate:
   select bitrate/quality policy from measured results rather than inference.
