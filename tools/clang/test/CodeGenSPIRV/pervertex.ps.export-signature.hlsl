// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s

// Both functions must be emitted as library roots. The export is queued
// first; the pixel entry is translated last, so the legacy context.isPS()
// gate runs PervertexInputVisitor on the entire module.
// The unrelated export must retain an ordinary float4 callable signature.
// CHECK: OpEntryPoint Fragment
// CHECK: OpDecorate [[EXPORT:%[a-zA-Z0-9_]+]] LinkageAttributes "exportedPassthrough" Export
// CHECK: %_ptr_Function_v4float = OpTypePointer Function %v4float
// CHECK: [[EXPORT]] = OpFunction %v4float
// CHECK-NEXT: [[PARAM:%[a-zA-Z0-9_]+]] = OpFunctionParameter %_ptr_Function_v4float

export float4 exportedPassthrough(nointerpolation float4 value) {
  return value;
}

[shader("pixel")]
float4 pixelMain(nointerpolation float4 input : TEXCOORD0) : SV_Target {
  return GetAttributeAtVertex(input, 2);
}
