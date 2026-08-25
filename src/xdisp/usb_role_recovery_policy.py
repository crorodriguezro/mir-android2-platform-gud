#!/usr/bin/python3

from collections import namedtuple


RecoveryOutcome = namedtuple(
    "RecoveryOutcome", "status teardown_observed host_ready elapsed_seconds"
)


def is_kernel_reattach_event(action, properties):
    return (
        action == "change"
        and properties.get("SUBSYSTEM") == "usb"
        and properties.get("DEVTYPE") == "usb_device"
        and properties.get("GUD_HOST_REATTACH") == "1"
    )


class Policy:
    def __init__(self, reattach_stable_seconds=1.5):
        self.reattach_stable_seconds = reattach_stable_seconds
        self.gud_present = False
        self.removal_consumed = False
        self.recovery_pending = False
        self.recovery_at = None

    def gud_added(self):
        self.gud_present = True
        self.removal_consumed = False
        self.recovery_pending = False
        self.recovery_at = None

    def gud_removed(self, mode):
        if not self.gud_present or self.removal_consumed:
            return False

        self.gud_present = False
        self.removal_consumed = True
        if mode != "host":
            return False

        self.recovery_pending = True
        self.recovery_at = None
        return True

    def reattach_observed(self, now):
        if not self.recovery_pending:
            return "ignored"

        if self.recovery_at is None:
            self.recovery_at = now + self.reattach_stable_seconds
            return "reattach"

        return "stable"

    def recovery_due(self, now):
        if (
            not self.recovery_pending
            or self.recovery_at is None
            or now < self.recovery_at
        ):
            return False

        self.recovery_pending = False
        self.recovery_at = None
        return True


class RecoveryTransaction:
    def __init__(
        self,
        read_mode,
        write_mode,
        xhci_present,
        host_ready,
        sleep,
        monotonic,
        teardown_timeout_seconds=3.0,
        role_settle_seconds=1.0,
        host_ready_timeout_seconds=3.0,
        wait_interval_seconds=0.05,
    ):
        self.read_mode = read_mode
        self.write_mode = write_mode
        self.xhci_present = xhci_present
        self.host_ready = host_ready
        self.sleep = sleep
        self.monotonic = monotonic
        self.teardown_timeout_seconds = teardown_timeout_seconds
        self.role_settle_seconds = role_settle_seconds
        self.host_ready_timeout_seconds = host_ready_timeout_seconds
        self.wait_interval_seconds = wait_interval_seconds

    def _wait_until(self, predicate, timeout_seconds):
        start = self.monotonic()
        while True:
            if predicate():
                return True

            elapsed = self.monotonic() - start
            if elapsed >= timeout_seconds:
                return False

            self.sleep(min(self.wait_interval_seconds, timeout_seconds - elapsed))

    def _outcome(self, status, teardown_observed, host_ready, start):
        return RecoveryOutcome(
            status,
            teardown_observed,
            host_ready,
            self.monotonic() - start,
        )

    def run(self):
        start = self.monotonic()
        if self.read_mode() != "host":
            return self._outcome("skipped-non-host", False, False, start)

        if not self.write_mode("device\n"):
            return self._outcome("device-write-failed", False, False, start)

        teardown_observed = self._wait_until(
            lambda: not self.xhci_present(), self.teardown_timeout_seconds
        )
        if teardown_observed:
            self.sleep(self.role_settle_seconds)

        # This is also the bounded ownership rollback if teardown timed out.
        if not self.write_mode("host\n"):
            return self._outcome(
                "host-write-failed", teardown_observed, False, start
            )

        host_ready = self._wait_until(
            self.host_ready, self.host_ready_timeout_seconds
        )
        if not teardown_observed:
            status = "teardown-timeout"
        elif not host_ready:
            status = "host-readiness-timeout"
        else:
            status = "recovered"

        return self._outcome(status, teardown_observed, host_ready, start)


if __name__ == "__main__":
    raise SystemExit("policy module is not executable")
