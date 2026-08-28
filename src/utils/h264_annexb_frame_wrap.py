#!/usr/bin/env python3
"""Wrap an Annex-B H.264 stream into the direct receiver's test framing."""
import struct
import sys
import time
import os

data = sys.stdin.buffer.read()
starts = []
pos = 0
while pos + 3 < len(data):
    if data[pos:pos + 4] == b"\0\0\0\1":
        starts.append((pos, 4))
        pos += 4
    elif data[pos:pos + 3] == b"\0\0\1":
        starts.append((pos, 3))
        pos += 3
    else:
        pos += 1

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

for index, (start, prefix_length) in enumerate(starts):
    end = starts[index + 1][0] if index + 1 < len(starts) else len(data)
    nal = data[start:end]
    nal_type = data[start + prefix_length] & 0x1f
    if nal_type in (1, 5) and has_vcl:
        emit(current)
        current = bytearray()
        has_vcl = False
    current.extend(nal)
    if nal_type in (1, 5):
        has_vcl = True
if current and has_vcl:
    emit(current)
