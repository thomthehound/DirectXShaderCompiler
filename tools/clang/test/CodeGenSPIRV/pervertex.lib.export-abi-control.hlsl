// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s

// Control: absent vertex interpolation, a library export with a
// nointerpolation parameter retains its ordinary float4 ABI.
// CHECK: OpDecorate %exportedPassthrough LinkageAttributes "exportedPassthrough" Export
// CHECK: %_ptr_Function_v4float = OpTypePointer Function %v4float
// CHECK: %exportedPassthrough = OpFunction %v4float
// CHECK: OpFunctionParameter %_ptr_Function_v4float

[shader("pixel")]
float4 pixelMain(nointerpolation float4 input : TEXCOORD0) : SV_Target {
  return input;
}

export float4 exportedPassthrough(nointerpolation float4 value) {
  return value;
}
