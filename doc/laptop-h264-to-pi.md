# Laptop extended display over H.264

`src/utils/laptop_h264_sender.py` turns a KDE Wayland virtual monitor into the
input of the Pi direct H.264/DRM receiver. It is intended for the same
1920x1080 display represented by the USB GUD gadget, but transports the pixels
over TCP instead of USB.

The live path is:

```text
KWin extended output -> krfb loopback -> GStreamer x264 -> MH264FRM/TCP
  -> Pi V4L2 H.264 decoder -> DRM PRIME NV12 -> HDMI
```

`krfb-virtualmonitor` is used only as KDE's supported virtual-output provider.
RFB traffic stays on laptop loopback; only framed H.264 crosses Wi-Fi. Closing
the sender removes the virtual monitor.

## Run

Start `h264-direct-drm-receiver` on the Pi on TCP port 5505, then run from the
repository root:

```sh
python3 src/utils/laptop_h264_sender.py
```

For a persistent desktop-session service, link and start the supplied unit:

```sh
systemctl --user link "$PWD/systemd/laptop-h264-to-pi.service"
systemctl --user enable --now laptop-h264-to-pi.service
journalctl --user -fu laptop-h264-to-pi.service
```

Stopping the service also removes the KDE virtual output:

```sh
systemctl --user stop laptop-h264-to-pi.service
```

`systemd/pi-h264-live-receiver.service` is the corresponding persistent Pi
unit. It resolves the current `gud-userspace.service` PID each time it starts,
so the direct receiver can duplicate the correct DRM owner fd after a Pi or GUD
service restart. Its `Restart=always` policy makes it return to listening after
a sender disconnects; only one sender can own a receiver connection at a time.

Defaults are Pi `192.168.1.110:5505`, a `1920x1080` output named `H264-to-Pi`,
30 FPS, and an 8 Mbit/s x264 target. They can be changed with command-line
options; use `--help` for the complete list.

This laptop currently has no usable hardware H.264 encoder exposed by the
Apple GPU, so the sender uses x264's ultrafast, zero-latency mode. The Pi path
remains hardware decoded and direct scanned out. The sender keeps one frame at
each capture boundary and asks the encoder for no B-frames, preventing queues
from turning decoder throughput variation into growing interaction latency.
The x264 VUI is deliberately omitted: the Pi's `/dev/video10` decoder rejected
the otherwise valid timing VUI with `EPIPE`; color interpretation is already
set explicitly to limited-range BT.709 by the direct DRM receiver.
