#!/usr/bin/env python3
"""Regression coverage for qualification evidence and process exit codes."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent

FAKE = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path
name = Path(sys.argv[0]).name
if name == "fake_dxc":
    if os.getenv("TEST_DXC") == "fail":
        sys.exit(9)
    Path(sys.argv[sys.argv.index("-Fo") + 1]).write_bytes(b"SPIRV")
elif name == "fake_driver":
    mode = os.getenv("TEST_DRIVER", "pass")
    if mode == "unsupported":
        sys.exit(3)
    if mode == "fail":
        sys.exit(1)
    if mode == "no-isa":
        sys.exit(0)
    Path(sys.argv[2]).write_text("v_perm_b32 v_dot4_i32_i8")
elif name == "fake_verifier":
    sys.exit({"pass": 0, "mismatch": 1, "error": 2}[os.getenv("TEST_VERIFY", "pass")])
else:
    sys.exit(17)
"""


class ProbeEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("fake_dxc", "fake_driver", "fake_verifier"):
            p = self.root / name
            p.write_text(FAKE, encoding="utf-8")
            p.chmod(0o755)

    def execute(self, name, settings=None, verify=False):
        out = self.root / "out"
        args = [sys.executable, str(ROOT / name),
                "--dxc", str(self.root / "fake_dxc"),
                "--driver-probe", str(self.root / "fake_driver"),
                "--candidate", "0", "--out-dir", str(out)]
        if verify:
            args += ["--runtime-verifier", str(self.root / "fake_verifier")]
        proc = subprocess.run(args, cwd=self.root,
                              env={**os.environ, **(settings or {})},
                              text=True, capture_output=True, check=False)
        report_file = "dot_report.json" if name == "dot_probe.py" else "candidate_zoo_report.json"
        return proc.returncode, json.loads((out / report_file).read_text())

    def test_dot_native_observation_is_not_semantics_proof(self):
        rc, report = self.execute("dot_probe.py")
        self.assertEqual(rc, 0)
        self.assertEqual(report["records"][0]["status"], "completed")
        self.assertEqual(report["records"][0]["runtime_semantics"], "not-run")

    def test_dot_driver_error_and_missing_isa_fail(self):
        for mode in ("fail", "no-isa"):
            with self.subTest(mode=mode):
                rc, report = self.execute("dot_probe.py", {"TEST_DRIVER": mode})
                self.assertEqual(rc, 2)
                self.assertEqual(report["records"][0]["status"], "failed")

    def test_dot_unsupported_is_distinct(self):
        rc, report = self.execute("dot_probe.py", {"TEST_DRIVER": "unsupported"})
        self.assertEqual(rc, 0)
        self.assertEqual(report["records"][0]["status"], "unsupported")

    def test_dot_compiler_failure(self):
        rc, report = self.execute("dot_probe.py", {"TEST_DXC": "fail"})
        self.assertEqual(rc, 1)
        self.assertEqual(report["records"][0]["status"], "failed")

    def test_unverified_isa_not_promoted(self):
        rc, report = self.execute("candidate_zoo.py")
        self.assertEqual(rc, 0)
        self.assertEqual(report["hit_count"], 0)
        self.assertEqual(report["records"][0]["status"], "isa-only")
        self.assertEqual(report["records"][0]["runtime"], "not-run")

    def test_verified_isa_promoted(self):
        rc, report = self.execute("candidate_zoo.py", verify=True)
        self.assertEqual(rc, 0)
        self.assertEqual(report["hit_count"], 1)
        self.assertEqual(report["records"][0]["status"], "completed")

    def test_semantic_mismatch_not_promoted(self):
        rc, report = self.execute("candidate_zoo.py", {"TEST_VERIFY": "mismatch"}, verify=True)
        self.assertEqual(rc, 1)
        self.assertEqual(report["hit_count"], 0)
        self.assertEqual(report["records"][0]["runtime"], "mismatch")

    def test_verifier_infrastructure_error_not_mismatch(self):
        rc, report = self.execute("candidate_zoo.py", {"TEST_VERIFY": "error"}, verify=True)
        self.assertEqual(rc, 2)
        self.assertEqual(report["hit_count"], 0)
        self.assertEqual(report["runtime_mismatches"], 0)
        self.assertEqual(report["records"][0]["runtime"], "error")

    def test_unsupported_driver_not_promoted(self):
        rc, report = self.execute("candidate_zoo.py", {"TEST_DRIVER": "unsupported"}, verify=True)
        self.assertEqual(rc, 0)
        self.assertEqual(report["hit_count"], 0)
        self.assertEqual(report["records"][0]["status"], "unsupported")

    def test_candidate_probe_errors_fail(self):
        for mode in ("fail", "no-isa"):
            with self.subTest(mode=mode):
                rc, report = self.execute("candidate_zoo.py", {"TEST_DRIVER": mode}, verify=True)
                self.assertEqual(rc, 2)
                self.assertEqual(report["hit_count"], 0)
                self.assertEqual(report["records"][0]["status"], "failed")



class QSadEvidenceTest(unittest.TestCase):
    def make_case(self, name, status):
        from apusr_qsad_probe import CaseResult

        case = CaseResult(name=name, shader="shader.hlsl", flags=[])
        case.driver_status = status
        if status == "native-mqsad":
            case.driver_sad_counts = {"v_mqsad_u32_u8": 1}
        return case

    def test_shader_info_unsupported_is_not_probe_failure(self):
        from apusr_qsad_probe import CommandResult, evidence_status

        for code in (3, 4):
            with self.subTest(code=code):
                result = CommandResult(["driver-probe"], code, "", "")
                self.assertEqual(evidence_status(result, None, {}), "unsupported")

    def test_driver_error_and_empty_disassembly_are_failures(self):
        from apusr_qsad_probe import CommandResult, evidence_status

        bad = CommandResult(["driver-probe"], 1, "", "")
        self.assertEqual(evidence_status(bad, None, {}), "probe-failed")
        with tempfile.TemporaryDirectory() as folder:
            empty_isa = Path(folder) / "shader.isa"
            empty_isa.write_text("")
            good = CommandResult(["driver-probe"], 0, "", "")
            self.assertEqual(evidence_status(good, empty_isa, {}), "missing-output")

    def test_complete_three_way_native_verdict(self):
        from apusr_qsad_probe import driver_verdict

        cases = [
            self.make_case("scalar", "no-sad-family"),
            self.make_case("udot", "native-mqsad"),
            self.make_case("swar", "other-sad-family"),
        ]
        result = driver_verdict(cases)
        self.assertEqual(result["verdict"], "mqsad-recovered")
        self.assertEqual(result["exact_mqsad_cases"], ["udot"])

    def test_incomplete_three_way_does_not_promote_native_hit(self):
        from apusr_qsad_probe import driver_verdict

        cases = [
            self.make_case("scalar", "native-mqsad"),
            self.make_case("udot", "unsupported"),
            self.make_case("swar", "no-sad-family"),
        ]
        result = driver_verdict(cases)
        self.assertEqual(result["verdict"], "incomplete-driver-probe")
        self.assertEqual(result["exact_mqsad_cases"], ["scalar"])
        self.assertEqual(result["unsupported_cases"], ["udot"])

    def test_all_driver_disassembly_unsupported(self):
        from apusr_qsad_probe import driver_verdict

        cases = [self.make_case(name, "unsupported") for name in ("scalar", "udot", "swar")]
        self.assertEqual(driver_verdict(cases)["verdict"], "driver-disassembly-unsupported")

    def test_full_probe_reference_rotation_preserves_equivalence(self):
        from sad_semantics import apusr_probe_result

        args = (0x01020304, 0x10203040, 0x50607080,
                0x01FF8002, 0x7F030C08, 0x98765432)
        self.assertEqual(
            apusr_probe_result(*args, masked=True),
            apusr_probe_result(*args, masked=False),
        )
        with_zero = (args[0], args[1], args[2], 0, args[4], args[5])
        self.assertNotEqual(
            apusr_probe_result(*with_zero, masked=True),
            apusr_probe_result(*with_zero, masked=False),
        )


if __name__ == "__main__":
    unittest.main()
