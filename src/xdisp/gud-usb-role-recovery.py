#!/usr/bin/python3

import glob
import logging
import os
import signal
import socket
import sys
import time

from usb_role_recovery_policy import (
    Policy,
    RecoveryTransaction,
    is_kernel_reattach_event,
)


GUD_PRODUCT = "1d50/614d"
PLATFORM_DEVICES = "/sys/bus/platform/devices"
USB_DEVICES = "/sys/bus/usb/devices"
REATTACH_STABLE_SECONDS = 1.5
TEARDOWN_TIMEOUT_SECONDS = 3.0
ROLE_SETTLE_SECONDS = 1.0
HOST_READY_TIMEOUT_SECONDS = 3.0
WAIT_INTERVAL_SECONDS = 0.05
running = True


def stop_handler(_signum, _frame):
    global running
    running = False


def read_value(path):
    try:
        with open(path, "r", encoding="ascii") as stream:
            return stream.read().strip()
    except OSError:
        return ""


def write_value(path, value):
    try:
        with open(path, "w", encoding="ascii") as stream:
            stream.write(value)
        return True
    except OSError as error:
        logging.error("write %s failed: %s", path, error)
        return False


def find_controller():
    candidates = []
    for base in glob.glob(os.path.join(PLATFORM_DEVICES, "*")):
        mode = os.path.join(base, "mode")
        if not os.access(mode, os.W_OK):
            continue
        uevent = read_value(os.path.join(base, "uevent"))
        if "DRIVER=msm-dwc3" in uevent and "OF_COMPATIBLE_0=qcom,dwc-usb3-msm" in uevent:
            candidates.append((base, mode))
    if len(candidates) != 1:
        raise RuntimeError(
            "expected one Qualcomm DWC3 mode node, found %d" % len(candidates)
        )
    return candidates[0]


def controller_xhci_paths(controller_path):
    controller_real = os.path.realpath(controller_path) + os.sep
    return [
        candidate
        for candidate in glob.glob(os.path.join(PLATFORM_DEVICES, "xhci-hcd.*"))
        if os.path.realpath(candidate).startswith(controller_real)
    ]


def xhci_present(controller_path):
    return bool(controller_xhci_paths(controller_path))


def host_ready(controller_path):
    for xhci in controller_xhci_paths(controller_path):
        xhci_real = os.path.realpath(xhci) + os.sep
        root_hubs = [
            root
            for root in glob.glob(os.path.join(USB_DEVICES, "usb*"))
            if os.path.realpath(root).startswith(xhci_real)
        ]
        if len(root_hubs) >= 2:
            return True
    return False


def gud_present():
    for device in glob.glob(os.path.join(USB_DEVICES, "*")):
        if read_value(os.path.join(device, "idVendor")) == "1d50" and read_value(
            os.path.join(device, "idProduct")
        ) == "614d":
            return True
    return False


def recover_host(controller_path, mode_path):
    transaction = RecoveryTransaction(
        read_mode=lambda: read_value(mode_path),
        write_mode=lambda value: write_value(mode_path, value),
        xhci_present=lambda: xhci_present(controller_path),
        host_ready=lambda: host_ready(controller_path),
        sleep=time.sleep,
        monotonic=time.monotonic,
        teardown_timeout_seconds=TEARDOWN_TIMEOUT_SECONDS,
        role_settle_seconds=ROLE_SETTLE_SECONDS,
        host_ready_timeout_seconds=HOST_READY_TIMEOUT_SECONDS,
        wait_interval_seconds=WAIT_INTERVAL_SECONDS,
    )
    logging.info(
        "starting one-shot host recovery after kernel-confirmed root-port reattach"
    )
    outcome = transaction.run()
    logging.info(
        "one-shot host recovery result=%s teardown_observed=%s host_ready=%s elapsed=%.3fs",
        outcome.status,
        outcome.teardown_observed,
        outcome.host_ready,
        outcome.elapsed_seconds,
    )
    return outcome


def parse_event(data):
    fields = data.decode("utf-8", errors="replace").split("\0")
    if not fields or "@" not in fields[0]:
        return None, {}
    action = fields[0].split("@", 1)[0]
    properties = {}
    for field in fields[1:]:
        if "=" in field:
            key, value = field.split("=", 1)
            properties[key] = value
    return action, properties


def is_gud_event(properties):
    return (
        properties.get("SUBSYSTEM") == "usb"
        and properties.get("DEVTYPE") == "usb_device"
        and properties.get("PRODUCT", "").startswith(GUD_PRODUCT + "/")
    )


def main():
    controller_path, mode_path = find_controller()
    monitor = socket.socket(
        socket.AF_NETLINK, socket.SOCK_DGRAM, getattr(socket, "NETLINK_KOBJECT_UEVENT", 15)
    )
    monitor.bind((os.getpid(), 1))
    monitor.settimeout(0.25)

    policy = Policy(REATTACH_STABLE_SECONDS)
    if gud_present():
        policy.gud_added()

    logging.info("monitoring USB GUD removal on %s", mode_path)
    while running:
        action = None
        properties = {}
        try:
            data = monitor.recv(8192)
            action, properties = parse_event(data)
        except socket.timeout:
            pass
        except OSError as error:
            if running:
                logging.error("USB event monitor failed: %s", error)
            break

        now = time.monotonic()
        if is_gud_event(properties):
            if action == "add":
                if policy.recovery_pending:
                    logging.info("GUD re-enumerated normally; cancelling pending recovery")
                policy.gud_added()
            elif action == "remove" and policy.gud_removed(read_value(mode_path)):
                logging.info(
                    "GUD removal consumed; waiting for kernel root-port reattach signal"
                )
        elif policy.recovery_pending and is_kernel_reattach_event(
            action, properties
        ):
            transition = policy.reattach_observed(now)
            logging.info(
                "kernel root-port reattach event port=%s transition=%s",
                properties.get("GUD_ROOT_PORT", "unknown"),
                transition,
            )

        if policy.recovery_due(time.monotonic()):
            if gud_present():
                logging.info("GUD is already present; cancelling due recovery")
                policy.gud_added()
            else:
                recover_host(controller_path, mode_path)

    monitor.close()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="gud-usb-role-recovery: %(message)s")
    signal.signal(signal.SIGTERM, stop_handler)
    signal.signal(signal.SIGINT, stop_handler)
    try:
        main()
    except Exception as error:
        logging.error("fatal: %s", error)
        sys.exit(1)
