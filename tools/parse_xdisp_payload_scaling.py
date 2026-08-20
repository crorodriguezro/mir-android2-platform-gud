#!/usr/bin/env python3
"""Parse marker-bounded XDISP frame summaries without fabricating telemetry."""

import argparse
import re
from pathlib import Path


FIELDS = (
    "source payload rectangles compressed raw max_payload planner_us copy_us "
    "set_buffer_us bulk_wait_us transfer_us"
).split()


def fields(line):
    return {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("kernel_log", type=Path)
    parser.add_argument("--marker-bounded", action="store_true",
                        help="include only records between XDISP_MEASURE_START/END")
    args = parser.parse_args()

    frames = []
    active = not args.marker_bounded
    for line in args.kernel_log.read_text().splitlines():
        if "XDISP_MEASURE_START" in line:
            active = True
            continue
        if "XDISP_MEASURE_END" in line:
            active = False
            continue
        if not active:
            continue
        if "XDISP frame " not in line:
            continue
        record = fields(line)
        if not all(key in record for key in FIELDS):
            continue
        record["frame_seq"] = len(frames) + 1
        record["payload_count"] = record["rectangles"]
        record["min_payload_bytes"] = "unavailable"
        record["avg_payload_bytes"] = record["payload"] / record["payload_count"]
        record["usb_bytes"] = record["payload"]
        record["compression_us"] = record["planner_us"]
        record["pi_receive_ms"] = "unavailable"
        record["pi_processing_total_ms"] = "unavailable"
        record["present_result"] = "unavailable"
        record["transfer_us_per_payload"] = (
            record["transfer_us"] / record["payload_count"]
        )
        record["transfer_us_per_rectangle"] = (
            record["transfer_us"] / record["rectangles"]
        )
        record["payload_bytes_per_second"] = (
            record["payload"] * 1000000 / record["transfer_us"]
        )
        frames.append(record)

    print("frame_seq payload_count rectangle_count represented_bytes compressed_bytes "
          "usb_bytes min_payload_bytes avg_payload_bytes max_payload_bytes "
          "planner_us compression_us copy_us set_buffer_us bulk_wait_us "
          "transfer_us transfer_us_per_payload transfer_us_per_rectangle "
           "payload_bytes_per_second pi_receive_ms pi_processing_total_ms "
           "presented_dropped")
    for record in frames:
        print("{frame_seq} {payload_count} {rectangles} {source} {payload} {usb_bytes} "
              "{min_payload_bytes} {avg_payload_bytes:.2f} {max_payload} {planner_us} "
              "{compression_us} {copy_us} {set_buffer_us} {bulk_wait_us} "
              "{transfer_us} {transfer_us_per_payload:.2f} "
               "{transfer_us_per_rectangle:.2f} {payload_bytes_per_second:.2f} "
               "{pi_receive_ms} {pi_processing_total_ms} "
              "{present_result}".format(**record))


if __name__ == "__main__":
    main()
