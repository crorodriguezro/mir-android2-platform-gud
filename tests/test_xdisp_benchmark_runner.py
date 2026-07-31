import json
import shutil
import subprocess
import unittest
import uuid
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/xdisp_benchmark_runner.py"


class RunnerTest(unittest.TestCase):
    def test_dry_run_resume_skips_passed_case(self):
        session = "runner-test-" + uuid.uuid4().hex
        command = ["python3", str(RUNNER), "start", "--session", session, "--dry-run"]
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(0, result.returncode, result.stderr)
        path = ROOT / "doc/benchmarks/2026-07-30-rgb565-vs-xrgb8888/sessions" / session
        completed = [json.loads(line) for line in (path / "completed.jsonl").read_text().splitlines()]
        self.assertEqual(2, len(completed))
        result = subprocess.run(["python3", str(RUNNER), "resume", "--session", session, "--dry-run"], cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(2, len((path / "completed.jsonl").read_text().splitlines()))
        shutil.rmtree(path)

    def test_start_accepts_an_empty_precreated_session_directory(self):
        session = "runner-test-" + uuid.uuid4().hex
        path = ROOT / "doc/benchmarks/2026-07-30-rgb565-vs-xrgb8888/sessions" / session
        path.mkdir(parents=True)
        result = subprocess.run(["python3", str(RUNNER), "start", "--session", session, "--dry-run"], cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(0, result.returncode, result.stderr)
        shutil.rmtree(path)

    def test_start_accepts_a_precreated_service_log(self):
        session = "runner-test-" + uuid.uuid4().hex
        path = ROOT / "doc/benchmarks/2026-07-30-rgb565-vs-xrgb8888/sessions" / session
        path.mkdir(parents=True)
        (path / "runner-service.log").touch()
        result = subprocess.run(["python3", str(RUNNER), "start", "--session", session, "--dry-run"], cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(0, result.returncode, result.stderr)
        shutil.rmtree(path)


if __name__ == "__main__":
    unittest.main()
