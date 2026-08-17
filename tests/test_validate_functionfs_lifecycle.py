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
            "event=ffs_io_observed endpoint=1 direction=out requested_length=12800 "
            "event=capture_open MARKER event=pi_kernel_capture_end "
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
        for record in (
            "classification=invalid_kernel_read_completion",
            'classification="invalid_kernel_read_completion"',
        ):
            with self.subTest(record=record):
                self.assertEqual(
                    "invalid_kernel_read_completion",
                    lifecycle.validate(record, "", "")[0],
                )

    def test_new_dwc2_transfer_fields_are_extracted_and_decoded(self):
        pi, phone, kernel = self.complete_inputs()
        kernel += (
            "\nevent=dwc2_start_req_programming endpoint=1 direction=out "
            "req_length=12800 remaining_length=12800 packet_count=25 "
            "initial_doeptsiz=0x00200000 initial_doepctl=0x00000000 "
            "dma_mode=buffer_dma"
            "\nevent=dwc2_doeptsiz_written intended_value=0x00c83200 "
            "immediate_readback=0x00c83200"
            "\nevent=dwc2_irq_enter doepint=0x00006010 daint=0x00010000 "
            "doeptsiz=0x00200000 doepctl=0x84088000"
            "\nevent=dwc2_core_state phase=start_req gsnpsid=0x4f54280a "
            "ghwcfg1=0x00000000 ghwcfg2=0x228ddd50 ghwcfg3=0x0ff000e8 "
            "ghwcfg4=0x1ff00020 dsts=0x00000100 grxstsr=0x00000000"
            "\nevent=dwc2_core_state phase=irq_enter gsnpsid=0x4f54280a "
            "dsts=0x00000200 grxstsr=0x00000001"
            "\nevent=dwc2_core_state phase=irq_exit gsnpsid=0x4f54280a "
            "dsts=0x00000300 grxstsr=0x00000002"
        )
        _, observed = lifecycle.validate(pi, phone, kernel)
        self.assertEqual("0x00200000", observed["initial_doeptsiz"])
        self.assertEqual("0x00c83200", observed["immediate_doeptsiz_readback"])
        self.assertEqual("0x00200000", observed["final_doeptsiz"])
        self.assertEqual("25", observed["initial_packet_count"])
        self.assertEqual("4", observed["final_packet_count"])
        self.assertEqual("12800", observed["initial_transfer_bytes"])
        self.assertEqual("0", observed["final_transfer_bytes"])
        self.assertEqual("0x00000000", observed["initial_doepctl"])
        self.assertEqual("0x84088000", observed["final_doepctl"])
        self.assertEqual("buffer_dma", observed["dma_mode"])
        self.assertTrue(observed["core_state_start_req"])
        self.assertTrue(observed["core_state_irq_enter"])
        self.assertTrue(observed["core_state_irq_exit"])
        self.assertEqual("0x4f54280a", observed["gsnpsid"])
        self.assertEqual("0x228ddd50", observed["ghwcfg2"])
        self.assertEqual("0x00000100", observed["start_dsts"])
        self.assertEqual("0x00000200", observed["irq_enter_dsts"])
        self.assertEqual("0x00000300", observed["irq_exit_dsts"])
        self.assertEqual("0x00000000", observed["start_grxstsr"])
        self.assertEqual("0x00000001", observed["irq_enter_grxstsr"])
        self.assertEqual("0x00000002", observed["irq_exit_grxstsr"])

    def test_ep1_xfercompl_subclassification_survives_incomplete_functionfs_coverage(self):
        pi = "event=exact_read_started"
        phone = "bulk_submit_start_ns=1 urb_status=0 urb_actual_length=12800 completion_wait_result=0"
        kernel = "\n".join(
            (
                "event=pi_kernel_trace_available",
                "event=dwc2_start_req_programming endpoint=1 direction=out "
                "req_length=12800 remaining_length=12800 maxpacket=512 "
                "packet_count=25 calculated_epsize=0x00c83200",
                "event=dwc2_doeptsiz_written intended_value=0x00c83200 immediate_readback=0x00c83200",
                "event=dwc2_out_ep_enabled epena=1",
                "event=dwc2_irq_enter doepint=0x00006010 daint=0x00010000 doeptsiz=0x00200000",
                "MARKER event=pi_kernel_capture_end",
            )
        )
        classification, observed = lifecycle.validate(pi, phone, kernel)
        self.assertEqual("inconclusive", classification)
        self.assertEqual(
            "dwc2_ep1_xfercompl_missing", observed["dwc2_subclassification"]
        )

    def test_ep1_xfercompl_subclassification_requires_every_hardware_gate(self):
        phone = "bulk_submit_start_ns=1 urb_status=0 urb_actual_length=12800 completion_wait_result=0"
        base = "\n".join(
            (
                "event=dwc2_start_req_programming endpoint=1 direction=out "
                "req_length=12800 remaining_length=12800 maxpacket=512 "
                "packet_count=25 calculated_epsize=0x00c83200",
                "event=dwc2_doeptsiz_written intended_value=0x00c83200 immediate_readback=0x00c83200",
                "event=dwc2_out_ep_enabled epena=1",
                "event=dwc2_irq_enter doepint=0x00006010 daint=0x00010000",
            )
        )
        mutations = (
            ("endpoint=1", "endpoint=2"),
            ("direction=out", "direction=in"),
            ("req_length=12800", "req_length=12799"),
            ("remaining_length=12800", "remaining_length=12799"),
            ("maxpacket=512", "maxpacket=64"),
            ("packet_count=25", "packet_count=24"),
            ("calculated_epsize=0x00c83200", "calculated_epsize=0x00c03200"),
            ("intended_value=0x00c83200", "intended_value=0x00c03200"),
            ("immediate_readback=0x00c83200", "immediate_readback=0x00c03200"),
            ("event=dwc2_doeptsiz_written", "event=unrelated"),
            ("epena=1", "epena=0"),
            ("doepint=0x00006010", "doepint=0x00006011"),
            ("daint=0x00010000", "daint=0x00030000"),
            ("event=dwc2_irq_enter", "event=unrelated_irq"),
        )
        for old, new in mutations:
            with self.subTest(gate=old):
                _, observed = lifecycle.validate("", phone, base.replace(old, new))
                self.assertEqual("", observed["dwc2_subclassification"])

        _, observed = lifecycle.validate("", phone.replace("urb_status=0", "urb_status=-5"), base)
        self.assertEqual("", observed["dwc2_subclassification"])

        _, observed = lifecycle.validate(
            "", phone, base + "\nevent=dwc2_complete_enter"
        )
        self.assertEqual("", observed["dwc2_subclassification"])
        for extra_irq in (
            "event=dwc2_irq_enter doepint=0x00000001 daint=0x00010000",
            "event=dwc2_irq_enter doepint=0x00000000 daint=0x00030000",
        ):
            with self.subTest(extra_irq=extra_irq):
                _, observed = lifecycle.validate("", phone, base + "\n" + extra_irq)
                self.assertEqual("", observed["dwc2_subclassification"])

    def test_ep0_marker_qualifies_provider_but_not_payload_coverage(self):
        kernel = (
            "event=ffs_trace_provider_active "
            "event=ffs_io_observed endpoint=0 direction=control io_kind=event"
        )
        _, observed = lifecycle.validate("", "", kernel)
        self.assertTrue(observed["functionfs_trace_provider_qualified"])
        self.assertFalse(observed["functionfs_kernel_trace_available"])


if __name__ == "__main__":
    unittest.main()
