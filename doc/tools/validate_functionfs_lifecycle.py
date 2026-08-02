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
    "dwc2_start_req_return",
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
)


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


def coverage(pi_text, phone_text, kernel_text, observed):
    kernel_open = has_event(kernel_text, "pi_kernel_trace_available")
    functionfs_provider = has_event(kernel_text, "ffs_trace_provider_active")
    functionfs_io = has_event(kernel_text, "ffs_io_observed")
    dwc2_io = observed["dwc2_queue_enter"]
    phone_submitted, _ = phone_events(phone_text)
    capture_start = has_event(kernel_text, "capture_open") or kernel_open
    capture_end = (
        has_event(kernel_text, "pi_kernel_capture_end")
        or "MARKER event=pi_kernel_capture_end" in kernel_text
    )

    return {
        # A provider marker plus an unfiltered request record proves both the
        # patched FunctionFS implementation and coverage of this request.
        "functionfs_kernel_trace_available": functionfs_provider and functionfs_io,
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
    observed["reason"] = ""

    if "classification=invalid_kernel_read_completion" in pi_text:
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
    print(f"classification={classification}")
    if observed["reason"]:
        print(f"reason={observed['reason']}")


if __name__ == "__main__":
    main()
