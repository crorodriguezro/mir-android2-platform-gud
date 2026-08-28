# Mir/Venus H.264 USB display POC

This POC captures the 1920x1080 Mir external-output region as ABGR8888,
converts it to BT.709 limited-range NV12 with 2x2 chroma averaging, copies it
into four bounded ION-backed V4L2 buffers, and submits the buffers
asynchronously to the OnePlus 6 Qualcomm Venus H.264 encoder. Output is either
Annex-B or a minimal MPEG-TS stream with PAT, PMT, PCR, and PTS.

The exercised physical path is:

```
Mir screencast -> ABGR/ION -> /dev/video33 Venus H.264
  -> MPEG-TS/TCP over USB ECM -> Pi bcm2835-codec V4L2 M2M
  -> DRM_PRIME YUV -> VLC drm_vout -> VC4 HDMI
```

Build the phone sender:

```
aarch64-redhat-linux-gcc -O2 -Wall -Wextra -Werror \
  -o mir-venus-h264 src/utils/mir_venus_h264.c -ldl
```

Arguments are:

```
mir-venus-h264 [output|-] [frames] [width] [height] \
  [capture-left] [fps] [annexb|mpegts] [bitrate] [capture-width] [capture-height]
```

The capture dimensions default to the encoded dimensions. `MIR_VENUS_TEST_PATTERN=1`
replaces the Mir pixels with a deterministic color/edge pattern for numeric
color-path checks; it is not a desktop workload.

The hardware acceptance invocation was:

```
mir-venus-h264 - 900 1920 1080 1080 30 mpegts 10000000 | nc 10.55.0.2 5504
```

The Pi distro FFmpeg V4L2 backend requests 1,632,256-byte H.264 input buffers,
while observed Venus IDRs reach 1,794,528 bytes.  Build and preload the scoped
receiver shim:

```
aarch64-redhat-linux-gcc -shared -fPIC -O2 -Wall -Wextra -Werror \
  -o v4l2-h264-sizeimage.so src/utils/v4l2_h264_sizeimage_preload.c -ldl

nc -l -p 5504 | env LD_PRELOAD=./v4l2-h264-sizeimage.so \
  cvlc -v -I dummy --no-audio --avcodec-hw=drm --codec=avcodec \
  --vout=drm_vout --network-caching=0 --live-caching=0 \
  --file-caching=0 --disc-caching=0 --drop-late-frames fd://0 vlc://quit
```

The shim intercepts only `VIDIOC_S_FMT` for H.264
`VIDEO_OUTPUT_MPLANE` and requests a 2 MiB `sizeimage`; the Pi driver accepted
the exact request.  No other codec, device, or ioctl is modified.

USB ECM must use MTU 512 on this hardware.  MTU 1500 handshakes but bulk data
stalls in the DWC2/ECM path.  MTU 512 sustained a measured 4 MiB transfer at
68.8 Mb/s, more than ten times the final encoded stream rate.

The sender queue is fixed at four ION buffers. The receiver must retain the
decoder's native YUV/DRM-PRIME surfaces. A raw RGB565 pipe is diagnostic only:
the Pi FFmpeg build has no accelerated yuv420p-to-RGB565 conversion and falls
to about 10 FPS at 1080p, creating stale frames. The H.264 sender does not emit
VUI color metadata even when Venus accepts the BT.709 format fields, so a
receiver must apply BT.709 limited-range interpretation explicitly (for
example with FFmpeg's `h264_metadata` bitstream filter).

See the timestamped evidence report for measurements and remaining work.
