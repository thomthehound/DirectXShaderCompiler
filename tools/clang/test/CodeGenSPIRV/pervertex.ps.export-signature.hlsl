// XFAIL: *
// RUN: %dxc -T ps_6_6 -E pixelMain -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s

// An unrelated linkage export must retain its source-level callable ABI.
// Per-vertex input lowering in pixelMain must not expand the export's
// nointerpolation parameter from float4 into a three-element array.
// Placing the pixel entry last keeps the original PS module pass enabled.
// CHECK: OpDecorate %exportedPassthrough LinkageAttributes "exportedPassthrough" Export
// CHECK: %_ptr_Function_v4float = OpTypePointer Function %v4float
// CHECK: %exportedPassthrough = OpFunction %v4float
// CHECK: OpFunctionParameter %_ptr_Function_v4float

export float4 exportedPassthrough(nointerpolation float4 value) {
  return value;
}

float4 pixelMain(nointerpolation float4 input : TEXCOORD0) : SV_Target {
  return GetAttributeAtVertex(input, 2);
}
