#!/usr/bin/env python3
"""Helpers for the direct Pi receiver's framed H.264 wire protocol."""

from __future__ import annotations

import struct


MAGIC = b"MH264FRM"
VERSION = 1
HEADER_SIZE = 32
MAX_PAYLOAD_SIZE = 16 * 1024 * 1024


def make_header(sequence: int, source_monotonic_ns: int, payload_size: int) -> bytes:
    """Return one version-1 MH264FRM header in network byte order."""
    if not 0 <= sequence <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("sequence does not fit in uint64")
    if not 0 <= source_monotonic_ns <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("source timestamp does not fit in uint64")
    if not 0 < payload_size <= MAX_PAYLOAD_SIZE:
        raise ValueError(f"payload size must be between 1 and {MAX_PAYLOAD_SIZE}")

    return MAGIC + bytes((VERSION, 0, 0, 0)) + struct.pack(
        ">QQI", sequence, source_monotonic_ns, payload_size
    )


def frame_access_unit(
    sequence: int, source_monotonic_ns: int, access_unit: bytes
) -> bytes:
    """Prefix one complete Annex-B access unit with its receiver header."""
    return make_header(sequence, source_monotonic_ns, len(access_unit)) + access_unit
