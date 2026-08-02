import importlib.util
import unittest
from pathlib import Path


VALIDATOR = Path(__file__).resolve().parents[1] / "doc/tools/validate_functionfs_lifecycle.py"
spec = importlib.util.spec_from_file_location("lifecycle", VALIDATOR)
lifecycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifecycle)


class LifecycleValidatorTest(unittest.TestCase):
    def complete_inputs(self):
        pi = "event=exact_read_started event=pi_functionfs_read_returned event=payload_completed receiver_state=Idle"
        phone = "bulk_submit_start_ns=1 urb_status=0 urb_actual_length=12800 completion_wait_result=0"
        kernel = (
            "event=pi_kernel_trace_available event=ffs_trace_provider_active "
            "event=ffs_io_observed event=capture_open MARKER event=pi_kernel_capture_end "
        ) + " ".join(
            f"event={event}" for event in lifecycle.KERNEL_EVENTS
        ) + " ret=0"
        # ret=0 must belong to the FunctionFS queue-return record.
        kernel = kernel.replace("event=ffs_ep_queue_return", "event=ffs_ep_queue_return ret=0")
        return pi, phone, kernel

    def assert_boundary(self, missing, expected):
        pi, phone, kernel = self.complete_inputs()
        if missing == "exact_read_started":
            pi = pi.replace("event=exact_read_started ", "")
        else:
            kernel = kernel.replace(f"event={missing} ", "")
        self.assertEqual(expected, lifecycle.validate(pi, phone, kernel)[0])

    def test_functionfs_queue_not_entered(self):
        self.assert_boundary("exact_read_started", "functionfs_queue_not_entered")

    def test_kernel_functionfs_queue_event_not_entered(self):
        self.assert_boundary("ffs_ep_queue_enter", "functionfs_queue_not_entered")

    def test_functionfs_queue_return_missing_is_a_queue_failure(self):
        self.assert_boundary("ffs_ep_queue_return", "functionfs_usb_ep_queue_failed")

    def test_functionfs_usb_ep_queue_failed(self):
        pi, phone, kernel = self.complete_inputs()
        kernel = kernel.replace("event=ffs_ep_queue_return ret=0", "event=ffs_ep_queue_return ret=-5")
        kernel = kernel.replace(" ret=0", "", 1)
        self.assertEqual("functionfs_usb_ep_queue_failed", lifecycle.validate(pi, phone, kernel)[0])

    def test_dwc2_queue_not_entered(self):
        self.assert_boundary("dwc2_queue_enter", "dwc2_queue_not_entered")

    def test_dwc2_request_not_started(self):
        self.assert_boundary("dwc2_start_req_enter", "dwc2_request_not_started")

    def test_dwc2_completion_missing(self):
        self.assert_boundary("dwc2_complete_enter", "dwc2_transfer_completion_missing")

    def test_dwc2_giveback_missing(self):
        self.assert_boundary("dwc2_giveback_enter", "dwc2_giveback_missing")

    def test_functionfs_callback_missing(self):
        self.assert_boundary("ffs_complete_enter", "functionfs_completion_callback_missing")

    def test_functionfs_wakeup_missing(self):
        self.assert_boundary("ffs_complete_wake", "functionfs_completion_wakeup_missing")

    def test_functionfs_wait_return_missing(self):
        self.assert_boundary("ffs_wait_return", "functionfs_wait_return_missing")

    def test_kernel_read_return_missing(self):
        self.assert_boundary("ffs_read_kernel_return", "kernel_read_return_missing")

    def test_userspace_read_return_missing(self):
        pi, phone, kernel = self.complete_inputs()
        pi = pi.replace("event=pi_functionfs_read_returned ", "")
        self.assertEqual("userspace_read_return_missing", lifecycle.validate(pi, phone, kernel)[0])

    def test_success_requires_idle_after_payload_completion(self):
        pi, phone, kernel = self.complete_inputs()
        self.assertEqual("success", lifecycle.validate(pi, phone, kernel)[0])

    def test_missing_kernel_layer_is_inconclusive(self):
        pi = "event=exact_read_started"
        phone = "bulk_submit_start_ns=1 urb_status=0 urb_actual_length=12800 completion_wait_result=0"
        self.assertEqual("inconclusive", lifecycle.validate(pi, phone, "")[0])

    def test_plain_key_value_records_are_recognized(self):
        self.assertTrue(lifecycle.has_event("event=pi_functionfs_read_entered", "pi_functionfs_read_entered"))

    def test_quoted_key_value_records_are_recognized(self):
        self.assertTrue(lifecycle.has_event('event="pi_functionfs_read_entered"', "pi_functionfs_read_entered"))

    def test_json_records_are_recognized(self):
        self.assertTrue(lifecycle.has_event('{"event":"pi_functionfs_read_entered"}', "pi_functionfs_read_entered"))

    def test_journald_prefixed_records_are_recognized(self):
        line = 'Aug 01 host gud-drm[1]: event="exact_read_started"'
        self.assertTrue(lifecycle.has_event(line, "exact_read_started"))

    def test_truncated_records_do_not_prove_trace_coverage(self):
        pi = 'event="exact_read_started"'
        phone = "bulk_submit_start_ns=1 urb_status=0 urb_actual_length=12800 completion_wait_result=0"
        kernel = "event=pi_kernel_trace_available event=ffs_trace_provider_active event=ffs_io_observed"
        classification, observed = lifecycle.validate(pi, phone, kernel)
        self.assertEqual("inconclusive", classification)
        self.assertFalse(observed["capture_continued_after_timeout"])

    def test_missing_functionfs_trace_layer_has_specific_reason(self):
        pi = 'event="exact_read_started"'
        phone = "bulk_submit_start_ns=1 urb_status=0 urb_actual_length=12800 completion_wait_result=0"
        kernel = "event=pi_kernel_trace_available event=dwc2_queue_enter MARKER event=pi_kernel_capture_end"
        classification, observed = lifecycle.validate(pi, phone, kernel)
        self.assertEqual("inconclusive", classification)
        self.assertEqual("functionfs_kernel_trace_unavailable_or_incomplete", observed["reason"])

    def test_configuration_failure_is_separate_from_payload_failure(self):
        phone = "usb 1-1.2: can't set config #1, error -32"
        kernel = "usb_ep_enable: ep1 (ep1out) has maxpacket 0"
        classification, observed = lifecycle.validate("", phone, kernel)
        self.assertEqual("gadget_configuration_failed", classification)
        self.assertEqual(
            "set_configuration_functionfs_endpoint_enable_failed", observed["reason"]
        )

    def test_optional_descriptor_diagnostics_do_not_change_classification(self):
        phone = "negotiated_usb_speed=full host_observed_wMaxPacketSize=0"
        kernel = "usb_ep_enable: ep1 (ep1out) has maxpacket 0 selected_wMaxPacketSize=0"
        classification, observed = lifecycle.validate("", phone, kernel)
        self.assertEqual("inconclusive", classification)
        self.assertEqual("full", observed["negotiated_usb_speed"])
        self.assertEqual("0", observed["host_observed_wMaxPacketSize"])
        self.assertEqual("0", observed["selected_wMaxPacketSize"])

    def test_invalid_completion_remains_separate(self):
        self.assertEqual(
            "invalid_kernel_read_completion",
            lifecycle.validate("classification=invalid_kernel_read_completion", "", "")[0],
        )


if __name__ == "__main__":
    unittest.main()
