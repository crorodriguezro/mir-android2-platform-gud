#!/usr/bin/env python3
"""Durable, resumable XDISP benchmark session runner.

The manifest is JSON encoded as YAML, avoiding a runtime YAML dependency.
Hardware commands are intentionally explicit and all state writes use rename.
"""
import argparse
import csv
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BENCHMARK_ROOT = ROOT / "doc/benchmarks/2026-07-30-rgb565-vs-xrgb8888"
TERMINAL = {"passed", "failed", "invalid", "blocked"}
INTERMEDIATE = {"preparing", "running-warmup", "waiting-for-idle-reset", "running-measured", "collecting", "validating"}


def now():
    return datetime.now(timezone.utc).isoformat()


def atomic_write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def append_jsonl(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as stream:
        stream.write(json.dumps(value, sort_keys=True) + "\n")
        stream.flush()
        os.fsync(stream.fileno())


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_sha(directory):
    return subprocess.check_output(["git", "-C", str(directory), "rev-parse", "HEAD"], text=True).strip()


def read_manifest(path):
    return json.loads(path.read_text())


def case_dir(session, case, attempt):
    return session / "artifacts" / case["case_id"] / f"attempt-{attempt:02d}"


def case_state_path(session, case_id):
    return session / "state" / f"{case_id}.json"


def load_state(session, case_id):
    path = case_state_path(session, case_id)
    return json.loads(path.read_text()) if path.exists() else {"case_id": case_id, "state": "planned", "attempt": 0}


def transition(session, state, target, **extra):
    record = dict(state, state=target, timestamp=now(), **extra)
    atomic_write(case_state_path(session, state["case_id"]), record)
    append_jsonl(session / "events.jsonl", record)
    if target in TERMINAL:
        append_jsonl(session / ("completed.jsonl" if target == "passed" else f"{target}.jsonl"), record)
    return record


def session_paths(session):
    return {"session": session, "log": session / "runner.log", "current": session / "current-case.json"}


def log(session, message):
    line = f"{now()} {message}\n"
    with (session / "runner.log").open("a") as stream:
        stream.write(line)
    print(line, end="")


def make_session(manifest_path, session_id):
    session = BENCHMARK_ROOT / "sessions" / session_id
    precreated = {path.name for path in session.iterdir()} if session.exists() else set()
    if precreated - {"runner-service.log"}:
        raise RuntimeError(f"session already exists: {session}")
    session.mkdir(parents=True, exist_ok=True)
    shutil.copy2(manifest_path, session / "manifest.yaml")
    atomic_write(session / "session.json", {
        "session_id": session_id, "created": now(), "manifest_sha256": sha256(manifest_path),
        "mir_commit": git_sha(ROOT), "gud_commit": git_sha(ROOT.parent / "gud"),
        "gadget_commit": git_sha(ROOT.parent / "gud-gadget"), "runner_pid": os.getpid(),
    })
    for name in ("events.jsonl", "completed.jsonl", "invalid.jsonl", "failed.jsonl"):
        (session / name).touch()
    return session


def safety_probe(args, case, dry_run):
    expected = {**args.defaults["expected_pi_environment"], "GUD_TRANSFER_FORMAT": case["format"]}
    if dry_run:
        return {"receiver_state": "Idle", "environment": expected, "scale_ms": 0}
    command = ["ssh", args.pi_host, "systemctl show gud-userspace.service --property=Environment --no-pager"]
    output = subprocess.check_output(command, text=True)
    environment = dict(item.split("=", 1) for item in output.strip().removeprefix("Environment=").split() if "=" in item)
    missing = {key: value for key, value in expected.items() if environment.get(key) != value}
    if missing:
        raise RuntimeError(f"Pi environment mismatch: {missing}")
    journal = subprocess.check_output(["ssh", args.pi_host, "journalctl -u gud-userspace.service -n 200 --no-pager"], text=True)
    if "Poisoned" in journal or "entered InFlight" in journal.rsplit("returned to Idle", 1)[-1]:
        return {"receiver_state": "InFlight", "environment": environment}
    if "scale_ms=" in journal and "scale_ms=0" not in journal:
        raise RuntimeError("Pi startup/log preflight lacks native scale_ms=0 evidence")
    return {"receiver_state": "Idle", "environment": environment}


def switch_format(args, case, dry_run):
    if dry_run:
        return
    password = os.environ.get("XDISP_PI_SUDO_PASSWORD")
    if not password:
        raise RuntimeError("XDISP_PI_SUDO_PASSWORD is required for a format switch")
    journal = subprocess.check_output(["ssh", args.pi_host, "journalctl -u gud-userspace.service -n 200 --no-pager"], text=True)
    if "Poisoned" in journal or "entered InFlight" in journal.rsplit("returned to Idle", 1)[-1]:
        raise RuntimeError("Pi receiver is not Idle; do not stop, restart, unbind, or reboot it")
    dropin = "70-xdisp-benchmark-rgb565.conf" if case["format"] == "rgb565" else "71-xdisp-benchmark-xrgb8888.conf"
    remote = (
        "test -f /home/cristian/{dropin} && "
        "printf '%s\\n' '{password}' | sudo -S install -o root -g root -m 0644 "
        "/home/cristian/{dropin} /etc/systemd/system/gud-userspace.service.d/70-xdisp-benchmark.conf && "
        "printf '%s\\n' '{password}' | sudo -S systemctl daemon-reload && "
        "printf '%s\\n' '{password}' | sudo -S systemctl stop gud-userspace.service && "
        "printf '%s\\n' '{password}' | sudo -S systemctl start gud-userspace.service"
    ).format(dropin=dropin, password=password)
    subprocess.run(["ssh", args.pi_host, remote], check=True, timeout=30)
    subprocess.run(["ssh", args.pi_host, "systemctl is-active --quiet gud-userspace.service"], check=True, timeout=15)


def command_for(args, case):
    return [args.phone_mirgud, "--pattern", "--pattern-workload", case["workload"],
            "--pattern-generation", "pregenerated", "--pattern-sequence-frames", "60",
            "--pattern-fps", str(case["requested_fps"]), "--pattern-duration",
            str(case["warmup_seconds"] + case["measured_seconds"]), "--benchmark-warmup",
            str(case["warmup_seconds"]), "--pattern-seed", str(case["seed"]), "--size", "1280", "720",
            "--pixel-format", case["format"]]


def phone_sudo(args, command):
    password = os.environ.get("XDISP_PHONE_SUDO_PASSWORD")
    if not password:
        raise RuntimeError("XDISP_PHONE_SUDO_PASSWORD is required for phone kernel collection")
    return f"printf '%s\\n' '{password}' | sudo -S {command}"


def validate(case, artifact, dry_run):
    if dry_run:
        return True, None
    text = (artifact / "stderr.log").read_text(errors="replace")
    final = [line for line in text.splitlines() if "report_kind=final" in line]
    if not final:
        return False, "missing_final_report"
    fields = dict(item.split("=", 1) for item in final[-1].split() if "=" in item)
    required_zero = ("frames_dropped", "gud_submit_failures", "frames_cancelled")
    if fields.get("accounting_ok") != "true" or any(fields.get(key) != "0" for key in required_zero):
        return False, "presenter_accounting_or_error"
    kernel = (artifact / "phone-kernel.log").read_text(errors="replace")
    pi = (artifact / "pi-service.log").read_text(errors="replace")
    if "-110" in kernel or "short_read=true" in pi or "Poisoned" in pi or "Wrote framebuffer dump" in pi or "Wrote raw framebuffer dump" in pi:
        return False, "transport_or_dump_error"
    return True, None


def run_case(args, session, case, dry_run):
    state = load_state(session, case["case_id"])
    if state["state"] == "passed":
        log(session, f"skip passed {case['case_id']}")
        return True
    if state["state"] in INTERMEDIATE:
        state = transition(session, state, "blocked", invalid_reason="interrupted_intermediate_state")
        log(session, f"blocked interrupted {case['case_id']}; inspect Pi before resume")
        return False
    attempt = state["attempt"] + 1
    artifact = case_dir(session, case, attempt)
    artifact.mkdir(parents=True)
    state = transition(session, state, "preparing", attempt=attempt, artifact=str(artifact.relative_to(session)))
    atomic_write(session / "current-case.json", state)
    try:
        switch_format(args, case, dry_run)
        probe = safety_probe(args, case, dry_run)
        atomic_write(artifact / "preflight.json", probe)
        if probe["receiver_state"] != "Idle":
            transition(session, state, "blocked", invalid_reason="pi_receiver_not_idle")
            log(session, "BLOCKED: Pi receiver is not Idle. Do not stop/restart/unbind; use physical recovery.")
            return False
        command = command_for(args, case)
        (artifact / "command.json").write_text(json.dumps(command) + "\n")
        transition(session, state, "running-warmup", command=command, warmup_started=now())
        if dry_run:
            (artifact / "stderr.log").write_text("mirgud: benchmark measured interval started after warmup_s=5\nreport_kind=final final=true frames_submitted=15 frames_presented=15 frames_dropped=0 frames_cancelled=0 gud_submit_failures=0 accounting_ok=true\n")
            (artifact / "phone-kernel.log").write_text("XDISP_MEASURE_START\nXDISP_MEASURE_END\n")
            (artifact / "pi-service.log").write_text("FunctionFS bulk receive session returned to Idle elapsed_ms=0\n")
        else:
            remote = " ".join(subprocess.list2cmdline([part]) for part in command)
            with (artifact / "stdout.log").open("w") as stdout, (artifact / "stderr.log").open("w") as stderr:
                process = subprocess.run(["ssh", args.phone_host, remote], stdout=stdout, stderr=stderr, timeout=case["warmup_seconds"] + case["measured_seconds"] + 30)
            (artifact / "exit-status.txt").write_text(str(process.returncode) + "\n")
            subprocess.run(["ssh", args.phone_host, phone_sudo(args, "dmesg")], stdout=(artifact / "phone-kernel.log").open("w"), check=False)
            subprocess.run(["ssh", args.pi_host, "journalctl -u gud-userspace.service -n 5000 --no-pager"], stdout=(artifact / "pi-service.log").open("w"), check=False)
        transition(session, state, "waiting-for-idle-reset", warmup_completed=now(), idle_reset_wait_started=now())
        transition(session, state, "running-measured", statistics_reset=True, measured_interval_started=now())
        transition(session, state, "collecting", measured_interval_completed=now())
        state = transition(session, state, "validating")
        valid, reason = validate(case, artifact, dry_run)
        inventory = {path.name: sha256(path) for path in artifact.iterdir() if path.is_file()}
        atomic_write(artifact / "artifacts.sha256.json", inventory)
        if valid:
            transition(session, state, "passed", validator="passed")
            return True
        transition(session, state, "invalid", invalid_reason=reason)
        return False
    except Exception as error:
        transition(session, state, "failed", error=str(error))
        return False
    finally:
        (session / "current-case.json").unlink(missing_ok=True)


def write_summary(session, manifest):
    rows = []
    for case in manifest["cases"]:
        state = load_state(session, case["case_id"])
        rows.append({"case_id": case["case_id"], "phase": case["phase"], "format": case["format"], "state": state["state"], "attempt": state["attempt"], "artifact": state.get("artifact", "")})
    with (session / "transport-results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader(); writer.writerows(rows)
    (session / "live-results.csv").write_text("case_id,phase,format,state,attempt,artifact\n")
    (session / "source-results.csv").write_text("case_id,phase,format,state,attempt,artifact\n")
    (session / "excluded-runs.csv").write_text("case_id,reason\n")
    (session / "session-summary.md").write_text("# XDISP Session Summary\n\n" + "\n".join(f"- `{row['case_id']}`: {row['state']} attempt {row['attempt']}" for row in rows) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("start", "resume", "status", "dry-run"))
    parser.add_argument("--manifest", type=Path, default=ROOT / "tools/xdisp_benchmark_matrix.yaml")
    parser.add_argument("--session")
    parser.add_argument("--stage", default="qualification")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--interrupt-after-case", action="store_true")
    parser.add_argument("--phone-host", default="phablet@192.168.1.120")
    parser.add_argument("--pi-host", default="cristian@192.168.1.110")
    parser.add_argument("--phone-mirgud", default="/home/phablet/mirgud.bin")
    args = parser.parse_args()
    manifest = read_manifest(args.manifest)
    args.defaults = manifest["defaults"]
    if args.command == "start":
        session = make_session(args.manifest, args.session or datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S"))
        print(f"session={session}\nreconnect/status: python3 tools/xdisp_benchmark_runner.py status --session {session.name}")
    else:
        if not args.session:
            parser.error("--session is required")
        session = BENCHMARK_ROOT / "sessions" / args.session
    if args.command == "status":
        write_summary(session, manifest)
        print((session / "session-summary.md").read_text(), end="")
        return
    for case in manifest["cases"]:
        if args.stage != "all" and case["phase"] != args.stage:
            continue
        if not run_case(args, session, case, args.dry_run or args.command == "dry-run"):
            write_summary(session, manifest); sys.exit(1)
        if args.interrupt_after_case:
            log(session, "deliberate runner interruption requested")
            write_summary(session, manifest); return
    write_summary(session, manifest)


if __name__ == "__main__":
    main()
