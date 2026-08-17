#!/usr/bin/env python3
"""Classify one exact-read FunctionFS payload from Pi kernel/userspace logs."""
import argparse
import re
from pathlib import Path


KERNEL_EVENTS = (
    "ffs_ep_queue_enter",
    "ffs_ep_queue_return",
    "dwc2_queue_enter",
    "dwc2_queue_added",
    "dwc2_start_req_enter",
    "dwc2_start_req_programming",
    "dwc2_doeptsiz_written",
    "dwc2_out_ep_enabled",
    "dwc2_start_req_return",
    "dwc2_irq_enter",
    "dwc2_ep1_out_epint_enter",
    "dwc2_out_irq",
    "dwc2_complete_enter",
    "dwc2_giveback_enter",
    "dwc2_giveback_return",
    "ffs_complete_enter",
    "ffs_complete_wake",
    "ffs_complete_exit",
    "ffs_wait_enter",
    "ffs_wait_return",
    "ffs_read_kernel_return",
)

EVENTS = (
    "exact_read_started",
    "phone_urb_submitted",
    "phone_urb_completed_successfully",
    *KERNEL_EVENTS,
    "pi_functionfs_read_returned",
    "payload_completed",
    "receiver_final_state",
)

COVERAGE_FIELDS = (
    "functionfs_trace_provider_qualified",
    "functionfs_kernel_trace_available",
    "dwc2_kernel_trace_available",
    "pi_userspace_trace_available",
    "phone_trace_available",
    "capture_started_before_payload",
    "capture_continued_after_timeout",
)

DIAGNOSTIC_FIELDS = (
    "negotiated_usb_speed",
    "selected_descriptor_speed",
    "selected_wMaxPacketSize",
    "host_observed_wMaxPacketSize",
    "userspace_serialized_wMaxPacketSize",
    "initial_doeptsiz",
    "immediate_doeptsiz_readback",
    "final_doeptsiz",
    "initial_packet_count",
    "final_packet_count",
    "initial_transfer_bytes",
    "final_transfer_bytes",
    "initial_doepctl",
    "final_doepctl",
    "dma_mode",
)

CORE_DIAGNOSTIC_FIELDS = (
    "core_state_start_req",
    "core_state_irq_enter",
    "core_state_irq_exit",
    "gsnpsid",
    "ghwcfg1",
    "ghwcfg2",
    "ghwcfg3",
    "ghwcfg4",
    "start_dsts",
    "irq_enter_dsts",
    "irq_exit_dsts",
    "start_grxstsr",
    "irq_enter_grxstsr",
    "irq_exit_grxstsr",
)

DWC2_SUBCLASSIFICATION_FIELD = "dwc2_subclassification"


def has_event(text, event):
    """Accept key/value, quoted key/value, and JSON structured events."""
    return re.search(
        rf'(?:\bevent\s*=\s*(?:"{re.escape(event)}"|{re.escape(event)})(?=\s|$|[,}}])'
        rf'|"event"\s*:\s*"{re.escape(event)}")',
        text,
    ) is not None


def phone_events(text):
    submitted = re.search(r"bulk_submit_start_ns=([1-9][0-9]*)", text) is not None
    completed = (
        re.search(r"bulk_result=0\b.*actual_bytes=12800\b", text) is not None
        or re.search(r"urb_status=0\b.*urb_actual_length=12800\b.*completion_wait_result=0\b", text)
        is not None
    )
    return submitted, completed


def queue_succeeded(text):
    return re.search(
        r'(?:\bevent\s*=\s*"?ffs_ep_queue_return"?(?=\s|$|[,}])'
        r'|"event"\s*:\s*"ffs_ep_queue_return").*\bret\s*=\s*"?0"?\b',
        text,
    ) is not None


def gadget_configuration_failed(phone_text, kernel_text):
    """Identify the control-plane failure before any framebuffer payload."""
    phone_failure = (
        re.search(r"can't set config #\d+, error -32", phone_text) is not None
        or re.search(r"SET_CONFIGURATION.*(?:-32|EPIPE)", phone_text) is not None
    )
    endpoint_enable_failure = (
        "usb_ep_enable: ep1 (ep1out) has maxpacket 0" in kernel_text
    )
    return phone_failure and endpoint_enable_failure


def diagnostic_value(name, *texts):
    """Read an optional diagnostic field without affecting classification."""
    for text in texts:
        match = re.search(
            rf'(?:\b{re.escape(name)}\s*=\s*"?([^\s",}}]+)'
            rf'|"{re.escape(name)}"\s*:\s*"?([^\s",}}]+))',
            text,
        )
        if match:
            return next(value for value in match.groups() if value is not None)
    return ""


def event_records(text, event):
    """Return event-local records, including logs with several test events."""
    marker = re.compile(
        r'(?:\bevent\s*=\s*(?:"[^"]+"|[^\s,}]+)|"event"\s*:\s*"[^"]+")'
    )
    matches = list(marker.finditer(text))
    records = []
    for index, match in enumerate(matches):
        if not has_event(match.group(0), event):
            continue
        line_end = text.find("\n", match.end())
        if line_end < 0:
            line_end = len(text)
        if index + 1 < len(matches):
            line_end = min(line_end, matches[index + 1].start())
        records.append(text[match.start():line_end])
    return records


def event_value(text, event, name, *, last=False):
    records = event_records(text, event)
    if last:
        records.reverse()
    for record in records:
        value = diagnostic_value(name, record)
        if value:
            return value
    return ""


def integer_value(value):
    try:
        return int(value, 0)
    except (TypeError, ValueError):
        return None


def dwc2_diagnostics(kernel_text):
    final_doeptsiz = event_value(
        kernel_text, "dwc2_irq_enter", "doeptsiz", last=True
    ) or event_value(kernel_text, "dwc2_ep1_out_epint_enter", "doeptsiz", last=True)
    final_doepctl = event_value(
        kernel_text, "dwc2_irq_enter", "doepctl", last=True
    ) or event_value(kernel_text, "dwc2_ep1_out_epint_enter", "doepctl", last=True)
    final_size = integer_value(final_doeptsiz)
    return {
        "initial_doeptsiz": event_value(
            kernel_text, "dwc2_start_req_programming", "initial_doeptsiz"
        ),
        "immediate_doeptsiz_readback": event_value(
            kernel_text, "dwc2_doeptsiz_written", "immediate_readback"
        ),
        "final_doeptsiz": final_doeptsiz,
        "initial_packet_count": event_value(
            kernel_text, "dwc2_start_req_programming", "packet_count"
        ),
        "final_packet_count": "" if final_size is None else str((final_size >> 19) & 0x3ff),
        "initial_transfer_bytes": event_value(
            kernel_text, "dwc2_start_req_programming", "remaining_length"
        ),
        "final_transfer_bytes": "" if final_size is None else str(final_size & 0x7ffff),
        "initial_doepctl": event_value(
            kernel_text, "dwc2_start_req_programming", "initial_doepctl"
        ),
        "final_doepctl": final_doepctl,
        "dma_mode": event_value(
            kernel_text, "dwc2_start_req_programming", "dma_mode"
        ),
    }


def dwc2_core_diagnostics(kernel_text):
    def phase_value(phase, name):
        for record in event_records(kernel_text, "dwc2_core_state"):
            if diagnostic_value("phase", record) == phase:
                return diagnostic_value(name, record)
        return ""

    start_gsnpsid = phase_value("start_req", "gsnpsid")
    irq_enter_gsnpsid = phase_value("irq_enter", "gsnpsid")
    irq_exit_gsnpsid = phase_value("irq_exit", "gsnpsid")
    return {
        "core_state_start_req": bool(start_gsnpsid),
        "core_state_irq_enter": bool(irq_enter_gsnpsid),
        "core_state_irq_exit": bool(irq_exit_gsnpsid),
        "gsnpsid": start_gsnpsid or irq_enter_gsnpsid or irq_exit_gsnpsid,
        "ghwcfg1": phase_value("start_req", "ghwcfg1"),
        "ghwcfg2": phase_value("start_req", "ghwcfg2"),
        "ghwcfg3": phase_value("start_req", "ghwcfg3"),
        "ghwcfg4": phase_value("start_req", "ghwcfg4"),
        "start_dsts": phase_value("start_req", "dsts"),
        "irq_enter_dsts": phase_value("irq_enter", "dsts"),
        "irq_exit_dsts": phase_value("irq_exit", "dsts"),
        "start_grxstsr": phase_value("start_req", "grxstsr"),
        "irq_enter_grxstsr": phase_value("irq_enter", "grxstsr"),
        "irq_exit_grxstsr": phase_value("irq_exit", "grxstsr"),
    }


def dwc2_subclassification(phone_text, kernel_text, observed):
    """Classify only the fully evidenced EP1 completion-bit boundary."""
    endpoint = event_value(kernel_text, "dwc2_start_req_programming", "endpoint")
    direction = event_value(kernel_text, "dwc2_start_req_programming", "direction")
    programming_values = {
        name: integer_value(
            event_value(kernel_text, "dwc2_start_req_programming", name)
        )
        for name in (
            "req_length",
            "remaining_length",
            "maxpacket",
            "packet_count",
            "calculated_epsize",
        )
    }
    doeptsiz_values = {
        name: integer_value(event_value(kernel_text, "dwc2_doeptsiz_written", name))
        for name in ("intended_value", "immediate_readback")
    }
    programming_correct = programming_values == {
        "req_length": 12800,
        "remaining_length": 12800,
        "maxpacket": 512,
        "packet_count": 25,
        "calculated_epsize": 0x00C83200,
    } and doeptsiz_values == {
        "intended_value": 0x00C83200,
        "immediate_readback": 0x00C83200,
    }
    enabled = event_value(
        kernel_text, "dwc2_out_ep_enabled", "epena"
    )
    irq_records = event_records(kernel_text, "dwc2_irq_enter")
    irq_doepint = [integer_value(diagnostic_value("doepint", record)) for record in irq_records]
    irq_daint = [integer_value(diagnostic_value("daint", record)) for record in irq_records]
    xfercompl_absent = bool(irq_doepint) and all(
        value is not None and not (value & 0x1) for value in irq_doepint
    )
    ep1_daint_absent = bool(irq_daint) and all(
        value is not None and not (value & (1 << 17)) for value in irq_daint
    )
    _, host_completed = phone_events(phone_text)
    proven = (
        observed["dwc2_start_req_programming"]
        and endpoint == "1"
        and direction == "out"
        and programming_correct
        and host_completed
        and observed["dwc2_doeptsiz_written"]
        and observed["dwc2_out_ep_enabled"]
        and enabled == "1"
        and xfercompl_absent
        and ep1_daint_absent
        and not observed["dwc2_complete_enter"]
    )
    return "dwc2_ep1_xfercompl_missing" if proven else ""


def coverage(pi_text, phone_text, kernel_text, observed):
    kernel_open = has_event(kernel_text, "pi_kernel_trace_available")
    functionfs_provider = has_event(kernel_text, "ffs_trace_provider_active")
    functionfs_io = has_event(kernel_text, "ffs_io_observed")
    functionfs_payload_io = any(
        diagnostic_value("endpoint", record) == "1"
        and diagnostic_value("direction", record) == "out"
        and diagnostic_value("requested_length", record) == "12800"
        for record in event_records(kernel_text, "ffs_io_observed")
    )
    dwc2_io = observed["dwc2_queue_enter"]
    phone_submitted, _ = phone_events(phone_text)
    capture_start = has_event(kernel_text, "capture_open") or kernel_open
    capture_end = (
        has_event(kernel_text, "pi_kernel_capture_end")
        or "MARKER event=pi_kernel_capture_end" in kernel_text
    )

    return {
        # EP0 is sufficient to qualify provider wiring before a payload.  The
        # payload evidence gate separately requires the tracked EP1 request.
        "functionfs_trace_provider_qualified": functionfs_provider and functionfs_io,
        "functionfs_kernel_trace_available": functionfs_provider and functionfs_payload_io,
        "dwc2_kernel_trace_available": kernel_open,
        "pi_userspace_trace_available": observed["exact_read_started"],
        "phone_trace_available": phone_submitted,
        "capture_started_before_payload": capture_start,
        "capture_continued_after_timeout": capture_end,
    }


def validate(pi_text, phone_text, kernel_text):
    observed = {event: False for event in EVENTS}
    observed["exact_read_started"] = has_event(pi_text, "exact_read_started")
    observed["pi_functionfs_read_returned"] = has_event(pi_text, "pi_functionfs_read_returned")
    observed["payload_completed"] = has_event(pi_text, "payload_completed")
    observed["receiver_final_state"] = "receiver_state=Idle" in pi_text
    observed["phone_urb_submitted"], observed["phone_urb_completed_successfully"] = phone_events(phone_text)
    for event in KERNEL_EVENTS:
        observed[event] = has_event(kernel_text, event)

    observed.update(coverage(pi_text, phone_text, kernel_text, observed))
    for field in DIAGNOSTIC_FIELDS:
        observed[field] = diagnostic_value(field, pi_text, phone_text, kernel_text)
    observed.update(dwc2_diagnostics(kernel_text))
    observed.update(dwc2_core_diagnostics(kernel_text))
    observed[DWC2_SUBCLASSIFICATION_FIELD] = dwc2_subclassification(
        phone_text, kernel_text, observed
    )
    observed["reason"] = ""

    if re.search(
        r'\bclassification\s*=\s*"?invalid_kernel_read_completion"?', pi_text
    ):
        return "invalid_kernel_read_completion", observed
    if gadget_configuration_failed(phone_text, kernel_text):
        observed["reason"] = "set_configuration_functionfs_endpoint_enable_failed"
        return "gadget_configuration_failed", observed

    if not observed["functionfs_kernel_trace_available"]:
        observed["reason"] = "functionfs_kernel_trace_unavailable_or_incomplete"
        return "inconclusive", observed
    if not observed["exact_read_started"]:
        return "functionfs_queue_not_entered", observed
    if not observed["pi_userspace_trace_available"]:
        observed["reason"] = "pi_userspace_trace_unavailable_or_incomplete"
        return "inconclusive", observed
    if not observed["dwc2_kernel_trace_available"]:
        observed["reason"] = "dwc2_kernel_trace_unavailable_or_incomplete"
        return "inconclusive", observed
    if not observed["phone_trace_available"]:
        observed["reason"] = "phone_trace_unavailable_or_incomplete"
        return "inconclusive", observed
    if not observed["capture_started_before_payload"] or not observed[
        "capture_continued_after_timeout"
    ]:
        observed["reason"] = "capture_window_unavailable_or_incomplete"
        return "inconclusive", observed
    if not observed["phone_urb_submitted"] or not observed["phone_urb_completed_successfully"]:
        return "inconclusive", observed
    if not observed["ffs_ep_queue_enter"]:
        return "functionfs_queue_not_entered", observed
    if not observed["ffs_ep_queue_return"] or not queue_succeeded(kernel_text):
        return "functionfs_usb_ep_queue_failed", observed
    if not observed["dwc2_queue_enter"]:
        return "dwc2_queue_not_entered", observed
    if not observed["dwc2_queue_added"] or not observed["dwc2_start_req_enter"]:
        return "dwc2_request_not_started", observed
    if not observed["dwc2_complete_enter"]:
        return "dwc2_transfer_completion_missing", observed
    if not observed["dwc2_giveback_enter"] or not observed["dwc2_giveback_return"]:
        return "dwc2_giveback_missing", observed
    if not observed["ffs_complete_enter"]:
        return "functionfs_completion_callback_missing", observed
    if not observed["ffs_complete_wake"] or not observed["ffs_complete_exit"]:
        return "functionfs_completion_wakeup_missing", observed
    if not observed["ffs_wait_return"]:
        return "functionfs_wait_return_missing", observed
    if not observed["ffs_read_kernel_return"]:
        return "kernel_read_return_missing", observed
    if not observed["pi_functionfs_read_returned"]:
        return "userspace_read_return_missing", observed
    if not observed["payload_completed"] or not observed["receiver_final_state"]:
        return "inconclusive", observed
    return "success", observed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pi-userspace", type=Path, required=True)
    parser.add_argument("--oneplus-kernel", type=Path, required=True)
    parser.add_argument("--pi-kernel", type=Path, required=True)
    args = parser.parse_args()
    classification, observed = validate(
        args.pi_userspace.read_text(errors="replace"),
        args.oneplus_kernel.read_text(errors="replace"),
        args.pi_kernel.read_text(errors="replace"),
    )
    for event in EVENTS:
        print(f"{event}={str(observed[event]).lower()}")
    for field in COVERAGE_FIELDS:
        print(f"{field}={str(observed[field]).lower()}")
    for field in DIAGNOSTIC_FIELDS:
        print(f"{field}={observed[field]}")
    for field in CORE_DIAGNOSTIC_FIELDS:
        value = observed[field]
        if isinstance(value, bool):
            value = str(value).lower()
        print(f"{field}={value}")
    print(f"{DWC2_SUBCLASSIFICATION_FIELD}={observed[DWC2_SUBCLASSIFICATION_FIELD]}")
    print(f"classification={classification}")
    if observed["reason"]:
        print(f"reason={observed['reason']}")


if __name__ == "__main__":
    main()
