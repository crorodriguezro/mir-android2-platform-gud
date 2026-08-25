#!/usr/bin/python3

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../../src/xdisp"))
from usb_role_recovery_policy import (
    Policy,
    RecoveryTransaction,
    is_kernel_reattach_event,
)


class UsbRoleRecoveryPolicyTest(unittest.TestCase):
    def test_real_host_removal_waits_for_stable_reattach(self):
        policy = Policy(reattach_stable_seconds=1.5)
        policy.gud_added()
        self.assertTrue(policy.gud_removed("host"))
        self.assertFalse(policy.recovery_due(10.0))
        self.assertEqual("reattach", policy.reattach_observed(10.0))
        self.assertFalse(policy.recovery_due(11.49))
        self.assertTrue(policy.recovery_due(11.5))
        self.assertFalse(policy.recovery_due(12.0))

    def test_duplicate_detach_cannot_arm_another_recovery(self):
        policy = Policy()
        policy.gud_added()
        self.assertTrue(policy.gud_removed("host"))
        self.assertFalse(policy.gud_removed("host"))

    def test_non_host_modes_are_consumed_without_recovery(self):
        for mode in ("peripheral", "none", "unknown"):
            policy = Policy()
            policy.gud_added()
            self.assertFalse(policy.gud_removed(mode))
            self.assertFalse(policy.recovery_pending)

    def test_initial_absence_never_arms_recovery(self):
        policy = Policy()
        self.assertFalse(policy.gud_removed("host"))
        self.assertEqual("ignored", policy.reattach_observed(1.0))
        self.assertFalse(policy.recovery_due(100.0))

    def test_new_gud_add_cancels_pending_and_arms_next_removal(self):
        policy = Policy()
        policy.gud_added()
        policy.gud_removed("host")
        policy.reattach_observed(1.0)
        policy.gud_added()
        self.assertFalse(policy.recovery_due(100.0))
        self.assertTrue(policy.gud_removed("host"))

    def test_only_tagged_kernel_usb_change_is_a_reattach(self):
        tagged = {
            "SUBSYSTEM": "usb",
            "DEVTYPE": "usb_device",
            "GUD_HOST_REATTACH": "1",
            "GUD_ROOT_PORT": "1",
        }
        self.assertTrue(is_kernel_reattach_event("change", tagged))
        self.assertFalse(is_kernel_reattach_event("add", tagged))
        self.assertFalse(
            is_kernel_reattach_event(
                "change", {"SUBSYSTEM": "power_supply", "GUD_HOST_REATTACH": "1"}
            )
        )
        self.assertFalse(
            is_kernel_reattach_event(
                "add", {"SUBSYSTEM": "usb", "DEVTYPE": "usb_device"}
            )
        )


class FakeRecoveryIo:
    def __init__(
        self,
        mode="host",
        device_write_ok=True,
        host_write_ok=True,
        teardown_never=False,
        ready_never=False,
    ):
        self.now = 0.0
        self.mode = mode
        self.device_write_ok = device_write_ok
        self.host_write_ok = host_write_ok
        self.teardown_never = teardown_never
        self.ready_never = ready_never
        self.device_written = False
        self.host_written = False
        self.xhci_off_at = None
        self.host_ready_at = None
        self.writes = []

    def read_mode(self):
        return self.mode

    def write_mode(self, value):
        role = value.strip()
        self.writes.append((role, self.now))
        if role == "device":
            if not self.device_write_ok:
                return False
            self.device_written = True
            self.mode = "device"
            self.xhci_off_at = self.now + 0.2
            return True
        if role == "host":
            if not self.host_write_ok:
                return False
            self.host_written = True
            self.mode = "host"
            self.host_ready_at = self.now + 0.2
            return True
        return False

    def xhci_present(self):
        if self.teardown_never:
            return True
        if not self.device_written:
            return True
        if not self.host_written:
            return self.now < self.xhci_off_at
        return self.now >= self.host_ready_at

    def host_ready(self):
        return (
            not self.ready_never
            and self.host_written
            and self.now >= self.host_ready_at
        )

    def sleep(self, seconds):
        self.now += seconds

    def monotonic(self):
        return self.now

    def transaction(self):
        return RecoveryTransaction(
            self.read_mode,
            self.write_mode,
            self.xhci_present,
            self.host_ready,
            self.sleep,
            self.monotonic,
            teardown_timeout_seconds=0.5,
            role_settle_seconds=1.0,
            host_ready_timeout_seconds=0.5,
            wait_interval_seconds=0.05,
        )


class UsbRoleRecoveryTransactionTest(unittest.TestCase):
    def test_success_waits_for_teardown_and_settle_before_host(self):
        fake = FakeRecoveryIo()
        outcome = fake.transaction().run()
        self.assertEqual("recovered", outcome.status)
        self.assertEqual(["device", "host"], [role for role, _ in fake.writes])
        self.assertGreaterEqual(fake.writes[1][1], 1.2)
        self.assertTrue(outcome.teardown_observed)
        self.assertTrue(outcome.host_ready)

    def test_non_host_mode_performs_no_writes(self):
        fake = FakeRecoveryIo(mode="device")
        outcome = fake.transaction().run()
        self.assertEqual("skipped-non-host", outcome.status)
        self.assertEqual([], fake.writes)

    def test_device_write_failure_performs_no_host_write(self):
        fake = FakeRecoveryIo(device_write_ok=False)
        outcome = fake.transaction().run()
        self.assertEqual("device-write-failed", outcome.status)
        self.assertEqual(["device"], [role for role, _ in fake.writes])

    def test_teardown_timeout_performs_one_bounded_host_rollback(self):
        fake = FakeRecoveryIo(teardown_never=True)
        outcome = fake.transaction().run()
        self.assertEqual("teardown-timeout", outcome.status)
        self.assertEqual(["device", "host"], [role for role, _ in fake.writes])

    def test_host_write_failure_is_terminal(self):
        fake = FakeRecoveryIo(host_write_ok=False)
        outcome = fake.transaction().run()
        self.assertEqual("host-write-failed", outcome.status)
        self.assertEqual(["device", "host"], [role for role, _ in fake.writes])

    def test_host_readiness_timeout_does_not_retry(self):
        fake = FakeRecoveryIo(ready_never=True)
        outcome = fake.transaction().run()
        self.assertEqual("host-readiness-timeout", outcome.status)
        self.assertEqual(["device", "host"], [role for role, _ in fake.writes])


if __name__ == "__main__":
    unittest.main()
