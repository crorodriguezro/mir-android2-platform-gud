#!/usr/bin/env python3
"""Stream a KDE Wayland virtual monitor to the direct Pi H.264 receiver.

krfb-virtualmonitor asks KWin for a real extended output and exposes that output
over RFB on loopback. GStreamer consumes the lossless local feed, performs a
low-latency software H.264 encode, and this process adds the MH264FRM header the
Pi direct DRM receiver expects to every complete access unit.
"""

from __future__ import annotations

import argparse
import os
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import NoReturn

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import GLib, Gst  # noqa: E402

from h264_frame_protocol import make_header  # noqa: E402


@dataclass(frozen=True)
class Settings:
    pi_host: str
    pi_port: int
    resolution: str
    width: int
    height: int
    fps: int
    bitrate_kbps: int
    rfb_port: int
    monitor_name: str
    encoder_threads: int


class LaptopH264Sender:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.password = secrets.token_hex(4)
        self.krfb: subprocess.Popen[bytes] | None = None
        self.pipeline: Gst.Pipeline | None = None
        self.connection: socket.socket | None = None
        self.loop = GLib.MainLoop()
        self.sequence = 0
        self.started_ns = time.monotonic_ns()
        self.last_report_ns = self.started_ns
        self.last_report_sequence = 0
        self.failure: str | None = None
        self.stopping = False

    def fail(self, message: str) -> None:
        if not self.failure:
            self.failure = message
            print(f"laptop-h264: {message}", file=sys.stderr, flush=True)
        self.loop.quit()

    def launch_virtual_monitor(self) -> None:
        command = [
            "krfb-virtualmonitor",
            "--resolution",
            self.settings.resolution,
            "--scale",
            "1",
            "--name",
            self.settings.monitor_name,
            "--password",
            self.password,
            "--port",
            str(self.settings.rfb_port),
        ]
        self.krfb = subprocess.Popen(command)
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            result = self.krfb.poll()
            if result is not None:
                raise RuntimeError(f"krfb-virtualmonitor exited with status {result}")
            try:
                with socket.create_connection(
                    ("127.0.0.1", self.settings.rfb_port), timeout=0.25
                ):
                    return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("timed out waiting for the KDE virtual monitor")

    def connect_receiver(self) -> None:
        print(
            f"laptop-h264: connecting to {self.settings.pi_host}:"
            f"{self.settings.pi_port}",
            file=sys.stderr,
            flush=True,
        )
        connection = socket.create_connection(
            (self.settings.pi_host, self.settings.pi_port), timeout=10.0
        )
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connection.settimeout(None)
        self.connection = connection

    def build_pipeline(self) -> None:
        s = self.settings
        description = f"""
            rfbsrc name=capture host=127.0.0.1 port={s.rfb_port}
                view-only=true shared=true do-timestamp=true incremental=true
            ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream
            ! videorate drop-only=true
            ! video/x-raw,width={s.width},height={s.height},framerate={s.fps}/1
            ! videoconvert n-threads={s.encoder_threads}
            ! video/x-raw,format=I420
            ! x264enc name=encoder tune=zerolatency speed-preset=ultrafast
                bitrate={s.bitrate_kbps} key-int-max={s.fps} bframes=0
                byte-stream=true aud=true insert-vui=false threads={s.encoder_threads}
            ! h264parse config-interval=-1
            ! video/x-h264,stream-format=byte-stream,alignment=au
            ! appsink name=frames emit-signals=true sync=false max-buffers=1 drop=true
        """
        pipeline = Gst.parse_launch(description)
        if not isinstance(pipeline, Gst.Pipeline):
            raise RuntimeError("GStreamer did not create a pipeline")
        capture = pipeline.get_by_name("capture")
        capture.set_property("password", self.password)
        sink = pipeline.get_by_name("frames")
        sink.connect("new-sample", self.on_sample)
        bus = pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_bus_message)
        self.pipeline = pipeline

    def on_sample(self, sink: Gst.Element) -> Gst.FlowReturn:
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.ERROR
        buffer = sample.get_buffer()
        success, mapped = buffer.map(Gst.MapFlags.READ)
        if not success:
            self.fail("could not map an encoded access unit")
            return Gst.FlowReturn.ERROR
        try:
            access_unit = bytes(mapped.data)
            timestamp_ns = time.monotonic_ns()
            header = make_header(self.sequence, timestamp_ns, len(access_unit))
            assert self.connection is not None
            self.connection.sendall(header)
            self.connection.sendall(access_unit)
            self.sequence += 1
            if timestamp_ns - self.last_report_ns >= 5_000_000_000:
                elapsed = (timestamp_ns - self.last_report_ns) / 1_000_000_000
                frames = self.sequence - self.last_report_sequence
                print(
                    f"laptop-h264: sent={self.sequence} rate={frames / elapsed:.1f}fps "
                    f"last_au={len(access_unit)}B",
                    file=sys.stderr,
                    flush=True,
                )
                self.last_report_ns = timestamp_ns
                self.last_report_sequence = self.sequence
        except (BrokenPipeError, ConnectionError, OSError, ValueError) as error:
            self.fail(f"receiver connection failed: {error}")
            return Gst.FlowReturn.ERROR
        finally:
            buffer.unmap(mapped)
        return Gst.FlowReturn.OK

    def on_bus_message(self, _bus: Gst.Bus, message: Gst.Message) -> None:
        if message.type == Gst.MessageType.ERROR:
            error, debug = message.parse_error()
            self.fail(f"GStreamer error: {error.message}; {debug or 'no details'}")
        elif message.type == Gst.MessageType.EOS:
            self.fail("capture stream ended")

    def run(self) -> int:
        try:
            self.launch_virtual_monitor()
            print(
                f"laptop-h264: KDE output '{self.settings.monitor_name}' is ready "
                f"at {self.settings.resolution}",
                file=sys.stderr,
                flush=True,
            )
            self.connect_receiver()
            self.build_pipeline()
            assert self.pipeline is not None
            if self.pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
                raise RuntimeError("GStreamer pipeline refused PLAYING state")
            print(
                f"laptop-h264: streaming {self.settings.resolution}@{self.settings.fps} "
                f"H.264 ({self.settings.bitrate_kbps} kb/s target)",
                file=sys.stderr,
                flush=True,
            )
            self.loop.run()
        except (OSError, RuntimeError, GLib.Error) as error:
            self.failure = str(error)
            print(f"laptop-h264: {error}", file=sys.stderr, flush=True)
        finally:
            self.stop()
        return 1 if self.failure else 0

    def stop(self) -> None:
        if self.stopping:
            return
        self.stopping = True
        if self.pipeline is not None:
            self.pipeline.set_state(Gst.State.NULL)
        if self.connection is not None:
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
        if self.krfb is not None and self.krfb.poll() is None:
            self.krfb.terminate()
            try:
                self.krfb.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.krfb.kill()
                self.krfb.wait()
        print(
            f"laptop-h264: stopped after {self.sequence} frames; virtual output removed",
            file=sys.stderr,
            flush=True,
        )


def parse_resolution(value: str) -> tuple[int, int]:
    try:
        width_text, height_text = value.lower().split("x", 1)
        width, height = int(width_text), int(height_text)
    except (ValueError, AttributeError) as error:
        raise argparse.ArgumentTypeError("resolution must look like 1920x1080") from error
    if width < 16 or height < 16 or width % 2 or height % 2:
        raise argparse.ArgumentTypeError("resolution dimensions must be even and at least 16")
    return width, height


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pi-host", default=os.getenv("H264_PI_HOST", "192.168.1.110"))
    parser.add_argument("--pi-port", type=positive_int, default=5505)
    parser.add_argument("--resolution", default="1920x1080")
    parser.add_argument("--fps", type=positive_int, default=30)
    parser.add_argument("--bitrate-kbps", type=positive_int, default=8000)
    parser.add_argument("--rfb-port", type=positive_int, default=5905)
    parser.add_argument("--monitor-name", default="H264-to-Pi")
    parser.add_argument(
        "--encoder-threads", type=positive_int, default=min(4, os.cpu_count() or 1)
    )
    return parser


def main() -> NoReturn:
    args = argument_parser().parse_args()
    width, height = parse_resolution(args.resolution)
    settings = Settings(
        pi_host=args.pi_host,
        pi_port=args.pi_port,
        resolution=f"{width}x{height}",
        width=width,
        height=height,
        fps=args.fps,
        bitrate_kbps=args.bitrate_kbps,
        rfb_port=args.rfb_port,
        monitor_name=args.monitor_name,
        encoder_threads=args.encoder_threads,
    )
    Gst.init(None)
    sender = LaptopH264Sender(settings)

    def request_stop(_signum: int, _frame: object) -> None:
        sender.loop.quit()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    raise SystemExit(sender.run())


if __name__ == "__main__":
    main()
