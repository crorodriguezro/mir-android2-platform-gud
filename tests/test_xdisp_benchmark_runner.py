import json
import shutil
import subprocess
import unittest
import uuid
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/xdisp_benchmark_runner.py"
STAGE_B = ROOT / "tools/xdisp_benchmark_stage_b_motion.yaml"


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

    def test_stage_b_manifest_has_balanced_stable_rate_groups(self):
        manifest = json.loads(STAGE_B.read_text())
        rates = [2, 4, 6, 8, 10]
        self.assertEqual(20, len(manifest["cases"]))
        for rate in rates:
            cases = [case for case in manifest["cases"] if case["rate"] == rate]
            self.assertEqual(["rgb565", "xrgb8888", "xrgb8888", "rgb565"], [case["format"] for case in cases])
            self.assertEqual([1, 1, 2, 2], [case["round"] for case in cases])
            self.assertEqual([1, 2, 3, 4], [case["order"] for case in cases])

    def test_validator_allows_one_shutdown_cancellation(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("runner", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        path = ROOT / "doc/benchmarks/2026-07-30-rgb565-vs-xrgb8888/sessions" / ("validator-" + uuid.uuid4().hex)
        path.mkdir(parents=True)
        (path / "stderr.log").write_text("report_kind=final final=true benchmark_elapsed_us=60000000 accounting_ok=true frames_dropped=0 gud_submit_failures=0 frames_cancelled=1\n")
        (path / "phone-kernel.log").write_text("")
        (path / "pi-service.log").write_text("")
        self.assertEqual((True, None), runner.validate({"measured_seconds": 60}, path, False))
        shutil.rmtree(path)


if __name__ == "__main__":
    unittest.main()
