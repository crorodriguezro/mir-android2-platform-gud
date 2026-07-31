#!/usr/bin/env python3
"""Validate machine-readable XDISP benchmark records."""
import sys


FORMATS = {
    "rgb565": ("0x40", "2"),
    "xrgb8888": ("0x80", "4"),
    "rgb888": ("0x50", "3"),
}


def fields(line):
    result = {}
    for token in line.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        if key in result:
            raise ValueError("duplicate key: " + key)
        result[key] = value
    return result


def validate(kind, line):
    record = fields(line)
    if kind == "mirgud":
        required = ("transport_format", "transport_bpp", "source_mir_format", "conversion_path_current")
        name, bpp = record.get("transport_format"), record.get("transport_bpp")
    elif kind == "gud":
        required = ("format", "bpp", "source", "payload", "rectangles", "max_payload", "cap")
        name, bpp = record.get("format", "").lower(), record.get("bpp")
        if int(record["max_payload"]) > 12800 or int(record["cap"]) > 12800:
            raise ValueError("payload cap exceeds 12800")
    elif kind == "pi":
        required = ("transfer_format", "gud_format", "bytes_per_pixel", "payload_seq", "rect", "source")
        name, bpp = record.get("transfer_format"), record.get("bytes_per_pixel")
        expected_gud = record.get("gud_format", "").lower()
        if name in FORMATS and expected_gud != FORMATS[name][0]:
            raise ValueError("GUD format disagrees with transfer format")
    else:
        raise ValueError("unknown record kind")
    missing = [key for key in required if key not in record]
    if missing:
        raise ValueError("missing required fields: " + ",".join(missing))
    if name not in FORMATS or bpp != FORMATS[name][1]:
        raise ValueError("format and bpp disagree")


SAMPLES = {
    "mirgud-rgb565": ("mirgud", "transport_format=rgb565 transport_bpp=2 source_mir_format=8 conversion_path_current=rgb565-pack"),
    "mirgud-xrgb8888": ("mirgud", "transport_format=xrgb8888 transport_bpp=4 source_mir_format=9 conversion_path_current=direct-copy"),
    "gud-rgb565": ("gud", "format=RGB565 bpp=2 source=2560 payload=12800 rectangles=5 max_payload=12800 cap=12800"),
    "gud-xrgb8888": ("gud", "format=XRGB8888 bpp=4 source=5120 payload=10240 rectangles=2 max_payload=12800 cap=12800"),
    "pi-rgb565": ("pi", "payload_seq=1 transfer_format=rgb565 gud_format=0x40 bytes_per_pixel=2 rect=1280x5+0,0 source=1280x720"),
    "pi-xrgb8888": ("pi", "payload_seq=1 transfer_format=xrgb8888 gud_format=0x80 bytes_per_pixel=4 rect=1280x2+0,0 source=1280x720"),
}


def main():
    for label, (kind, line) in SAMPLES.items():
        validate(kind, line)
        print("PASS " + label)
    for kind, line in (("mirgud", "transport_format=rgb565 transport_format=rgb565 transport_bpp=2 source_mir_format=1 conversion_path_current=direct-copy"),
                       ("gud", "format=RGB565 bpp=4 source=1 payload=1 rectangles=1 max_payload=1 cap=12800"),
                       ("pi", "transfer_format=xrgb8888 gud_format=0x80 bytes_per_pixel=2 payload_seq=1 rect=1x1+0,0 source=1x1")):
        try:
            validate(kind, line)
        except ValueError:
            continue
        raise AssertionError("invalid record accepted")


if __name__ == "__main__":
    main()
