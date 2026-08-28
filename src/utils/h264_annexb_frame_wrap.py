#!/usr/bin/env python3
"""Wrap an Annex-B H.264 stream into the direct receiver's test framing."""
import struct
import sys
import time
import os

def iter_nals():
    """Yield complete Annex-B NAL units without buffering the whole stream."""
    data = bytearray()
    eof = False
    while True:
        chunk = sys.stdin.buffer.read(65536)
        if chunk:
            data.extend(chunk)
        else:
            eof = True
        while True:
            start = -1
            prefix_length = 0
            for pos in range(max(0, len(data) - 3)):
                if data[pos:pos + 4] == b"\0\0\0\1":
                    start, prefix_length = pos, 4
                    break
                if data[pos:pos + 3] == b"\0\0\1":
                    start, prefix_length = pos, 3
                    break
            if start < 0:
                if eof:
                    return
                if len(data) > 3:
                    del data[:-3]
                break
            if start:
                del data[:start]
            next_start = -1
            for pos in range(prefix_length, len(data) - 2):
                if data[pos:pos + 4] == b"\0\0\0\1":
                    next_start = pos
                    break
                if data[pos:pos + 3] == b"\0\0\1":
                    next_start = pos
                    break
            if next_start < 0:
                if eof:
                    if len(data) > prefix_length:
                        yield bytes(data)
                    return
                break
            yield bytes(data[:next_start])
            del data[:next_start]
        if eof:
            return

sequence = 0
current = bytearray()
has_vcl = False
source_start = time.monotonic_ns()

def emit(access_unit):
    global sequence
    header = (b"MH264FRM" + b"\1\0\0\0" +
              struct.pack(">QQI", sequence, source_start + sequence * 33333333,
                          len(access_unit)))
    sys.stdout.buffer.write(header + access_unit)
    sys.stdout.buffer.flush()
    if os.environ.get("H264_WRAP_PACE_FPS"):
        time.sleep(1.0 / float(os.environ["H264_WRAP_PACE_FPS"]))
    sequence += 1

for nal in iter_nals():
    prefix_length = 4 if nal[:4] == b"\0\0\0\1" else 3
    nal_type = nal[prefix_length] & 0x1f
    if nal_type in (1, 5) and has_vcl:
        emit(current)
        current = bytearray()
        has_vcl = False
    current.extend(nal)
    if nal_type in (1, 5):
        has_vcl = True
if current and has_vcl:
    emit(current)
