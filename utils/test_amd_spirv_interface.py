"""Unit tests for tracing AMD pixel-input operands across entry interfaces."""
import unittest

from amd_spirv_interface import check_pixel_parameter_interface


def module_text(owner="first", include_first=True, include_second=False):
    first = "%input_first" if include_first else ""
    second = "%input_second %input_first" if include_second else "%input_second"
    return f"""
OpEntryPoint Fragment %first "first" {first}
OpEntryPoint Fragment %second "second" {second}
%input_first = OpVariable %_ptr_Input_v4float Input
%input_second = OpVariable %_ptr_Input_v2float Input
%component = OpAccessChain %_ptr_Input_float %input_first %uint_0
%value = OpExtInst %float %amd InterpolateAtVertexAMD %component %uint_1
"""


class TestAmdPixelInterface(unittest.TestCase):
    def test_owned_input(self):
        check_pixel_parameter_interface(module_text(), "first", "second")

    def test_wrong_owner(self):
        with self.assertRaisesRegex(ValueError, "missing from owner"):
            check_pixel_parameter_interface(module_text(), "second", "first")

    def test_missing_interface_variable(self):
        with self.assertRaisesRegex(ValueError, "missing from owner"):
            check_pixel_parameter_interface(
                module_text(include_first=False), "first", "second"
            )

    def test_shared_interface_variable(self):
        with self.assertRaisesRegex(ValueError, "also belongs"):
            check_pixel_parameter_interface(
                module_text(include_second=True), "first", "second"
            )

    def test_untraceable_pointer(self):
        sample = module_text().replace(
            "InterpolateAtVertexAMD %component",
            "InterpolateAtVertexAMD %nonexistent",
        )
        with self.assertRaisesRegex(ValueError, "cannot trace"):
            check_pixel_parameter_interface(sample, "first", "second")

    def test_missing_intrinsic(self):
        with self.assertRaisesRegex(ValueError, "no AMD interpolation"):
            check_pixel_parameter_interface(
                module_text().replace("InterpolateAtVertexAMD", "SomethingElse"),
                "first", "second"
            )


if __name__ == "__main__":
    unittest.main()
