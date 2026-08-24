#!/usr/bin/env python3
"""Verify that phone and Pi logs contain one complete E4-T01 mode contract."""

import argparse
import re
from pathlib import Path


ID = re.compile(r'mode_contract_id[=:]\s*"?(e4c1-[0-9a-f]{16})')
EVENT = re.compile(r'event[=:]\s*"?([a-z0-9_]+)')


def records(path: Path):
    result = {}
    for line in path.read_text(errors="replace").splitlines():
        identity = ID.search(line)
        event = EVENT.search(line)
        if identity and event:
            result.setdefault(identity.group(1), {}).setdefault(event.group(1), []).append(line)
    return result


def require_fields(line: str, fields, description: str):
    missing = [field for field in fields if f"{field}=" not in line]
    if missing:
        raise SystemExit(f"{description} is missing fields: {', '.join(missing)}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phone_log", type=Path)
    parser.add_argument("pi_log", type=Path)
    parser.add_argument("--contract-id")
    args = parser.parse_args()

    phone = records(args.phone_log)
    pi = records(args.pi_log)
    common = sorted(set(phone) & set(pi))
    if args.contract_id:
        common = [identity for identity in common if identity == args.contract_id]
    if len(common) != 1:
        raise SystemExit(
            "expected exactly one common mode contract ID; found " +
            (", ".join(common) if common else "none"))

    identity = common[0]
    phone_events = phone[identity]
    pi_events = pi[identity]
    for event in ("host_mode_selected", "mir_source_ready"):
        if event not in phone_events:
            raise SystemExit(f"phone log lacks {event} for {identity}")
    if "e4_mode_contract_gud_commit" not in pi_events:
        raise SystemExit(f"Pi log lacks GUD commit for {identity}")
    physical_event = next((event for event in (
        "e4_mode_contract_physical_route", "mode_commit_decision") if event in pi_events), None)
    if not physical_event:
        raise SystemExit(f"Pi log lacks physical route for {identity}")

    require_fields(phone_events["host_mode_selected"][-1], (
        "connector", "format", "clock_khz", "hdisplay", "hsync_start", "hsync_end",
        "htotal", "vdisplay", "vsync_start", "vsync_end", "vtotal", "flags"),
        "host mode record")
    require_fields(phone_events["mir_source_ready"][-1], (
        "logical_width", "logical_height", "source_width", "source_height",
        "source_stride", "mir_format", "row_order", "transport_format", "conversion_path"),
        "Mir source record")
    require_fields(pi_events["e4_mode_contract_gud_commit"][-1], (
        "generation", "connector", "format", "clock_khz", "hdisplay", "hsync_start",
        "hsync_end", "htotal", "vdisplay", "vsync_start", "vsync_end", "vtotal", "flags"),
        "Pi GUD commit record")
    physical_fields = ("physical_mode", "physical_size", "physical_pitch") if physical_event == \
        "e4_mode_contract_physical_route" else ("new_physical", "active_size", "active_pitch")
    require_fields(pi_events[physical_event][-1], physical_fields, "Pi physical route record")

    print(f"PASS mode_contract_id={identity} physical_event={physical_event}")


if __name__ == "__main__":
    main()
