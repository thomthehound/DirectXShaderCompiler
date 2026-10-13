#!/usr/bin/env python3
"""Regression tests for cooperative-matrix compiler and driver evidence."""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
from khr_cooperative_matrix_control import (  # noqa: E402
    read_opcodes, OP_COOPERATIVE_MATRIX_MUL_ADD_KHR,
    OP_COOPERATIVE_MATRIX_LOAD_KHR, OP_COOPERATIVE_MATRIX_STORE_KHR,
)

FAKE_DXC = """#!/usr/bin/env python3
import os
import struct
import sys
from pathlib import Path
if os.environ.get("FAKE_COMPILE_FAIL") == "yes":
    sys.exit(1)
path = Path(sys.argv[sys.argv.index("-Fo") + 1])
words = [0x07230203, 0x10600, 0, 12, 0,
         (2 << 16) | 17, 6022,
         (2 << 16) | 4456, 1,
         (2 << 16) | 4459, 1]
if "-DCONTROL_F16=0" in sys.argv:
    words += [(2 << 16) | 4457, 1, (2 << 16) | 4458, 1]
path.write_bytes(struct.pack("<" + "I" * len(words), *words))
"""
FAKE_PROBE = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path
mode = os.environ.get("FAKE_DRIVER_MODE", "success")
if "--properties-only" in sys.argv:
    if mode == "unsupported-property":
        print("UNSUPPORTED_PROPERTY: unsupported")
        sys.exit(33)
    if mode == "property-error":
        print("driver died")
        sys.exit(1)
    print("PROPERTY_SUPPORTED")
    print("ISA_NOT_TESTED:")
    sys.exit(0)
if mode == "unsupported-disassembly":
    print("UNSUPPORTED: VK_AMD_shader_info")
    sys.exit(30)
if mode == "pipeline-error":
    print("PIPELINE_REJECTED: -3")
    sys.exit(39)
Path(sys.argv[2]).write_text("v_wmma_f16_16x16x16_f16")
print("ISA_WRITTEN")
"""


class KhrMatrixControlTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name, contents in (("fake_dxc", FAKE_DXC), ("fake_probe", FAKE_PROBE)):
            path = self.root / name
            path.write_text(contents, encoding="utf-8")
            path.chmod(0o755)

    def run_control(self, mode=None, hardware=False):
        cmd = [
            sys.executable, str(ROOT / "khr_cooperative_matrix_control.py"),
            "--dxc", str(self.root / "fake_dxc"),
            "--out-dir", str(self.root / "out"),
            "--mode", (mode.get("CONTROL_MODE", "f16xf16-f16") if mode else "f16xf16-f16"),
        ]
        if hardware:
            cmd.extend(["--matrix-probe", str(self.root / "fake_probe")])
        result = subprocess.run(
            cmd, env={**os.environ, **(mode or {})},
            capture_output=True, text=True, check=False,
        )
        report = json.loads(
            (self.root / "out/khr_matrix_report.json").read_text()
        )
        return result.returncode, report

    def test_compiler_only_does_not_claim_driver_qualification(self):
        rc, report = self.run_control()
        self.assertEqual(rc, 0)
        self.assertEqual(report["overall_status"], "compiler-only")
        self.assertEqual(report["cases"][0]["driver_status"], "not-run")

    def test_compiler_error_is_fatal(self):
        rc, report = self.run_control({"FAKE_COMPILE_FAIL": "yes"})
        self.assertEqual(rc, 1)
        self.assertEqual(report["overall_status"], "failed")

    def test_driver_native_isa_is_observed_not_verified(self):
        rc, report = self.run_control(hardware=True)
        self.assertEqual(rc, 0)
        self.assertEqual(report["overall_status"], "completed")
        self.assertTrue(report["cases"][0]["native_matrix_isa_observed"])
        self.assertFalse("runtime_verified" in report["cases"][0])

    def test_property_unsupported_does_not_count_as_isa_miss(self):
        rc, report = self.run_control(
            {"FAKE_DRIVER_MODE": "unsupported-property"}, hardware=True
        )
        self.assertEqual(rc, 0)
        self.assertEqual(report["overall_status"], "completed-with-unsupported")
        self.assertFalse(report["cases"][0]["isa_observed"])

    def test_disassembly_unsupported_does_not_count_as_isa_miss(self):
        rc, report = self.run_control(
            {"FAKE_DRIVER_MODE": "unsupported-disassembly"}, hardware=True
        )
        self.assertEqual(rc, 0)
        self.assertEqual(report["overall_status"], "completed-with-unsupported")
        self.assertEqual(report["cases"][0]["driver_status"], "unsupported")

    def test_pipeline_error_is_fatal(self):
        rc, report = self.run_control(
            {"FAKE_DRIVER_MODE": "pipeline-error"}, hardware=True
        )
        self.assertEqual(rc, 1)
        self.assertEqual(report["cases"][0]["driver_status"], "failed")

    def test_structured_buffer_matrix_operations_are_required(self):
        rc, report = self.run_control({"CONTROL_MODE": "i32xi32-i32"})
        self.assertEqual(rc, 0)
        self.assertEqual(report["cases"][0]["compiler_status"], "pass")
        ops, _ = read_opcodes(self.root / "out/i32xi32-i32.spv")
        self.assertIn(OP_COOPERATIVE_MATRIX_LOAD_KHR, ops)
        self.assertIn(OP_COOPERATIVE_MATRIX_STORE_KHR, ops)

    def test_invalid_spirv_is_rejected(self):
        bad = self.root / "bad.spv"
        bad.write_bytes(struct.pack("<6I", 0x07230203, 0, 0, 1, 0, 0))
        with self.assertRaises(ValueError):
            read_opcodes(bad)

    def test_spirv_parser_observes_required_opcode(self):
        self.run_control()
        found, cap = read_opcodes(self.root / "out/f16xf16-f16.spv")
        self.assertTrue(cap)
        self.assertIn(OP_COOPERATIVE_MATRIX_MUL_ADD_KHR, found)


if __name__ == "__main__":
    unittest.main()
