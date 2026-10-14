// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s

// A Fragment entry may pass an ordinary provoking-vertex value to a
// linkage-visible function. Do not widen that function's ABI to float4[3],
// and do not reject the call just because the original input was flat.
// CHECK: OpEntryPoint Fragment
// CHECK-DAG: OpDecorate [[EXPORT:%[a-zA-Z0-9_]+]] LinkageAttributes "exportedPassthrough" Export
// CHECK-DAG: OpDecorate [[INPUT:%[a-zA-Z0-9_]+]] PerVertexKHR
// CHECK: [[EXPORT]] = OpFunction %v4float
// CHECK-NEXT: [[PARAM:%[a-zA-Z0-9_]+]] = OpFunctionParameter %_ptr_Function_v4float
// CHECK: OpFunctionCall %v4float [[EXPORT]]

export float4 exportedPassthrough(float4 value) {
  return value;
}

[shader("pixel")]
float4 pixelMain(nointerpolation float4 input : TEXCOORD0) : SV_Target {
  return exportedPassthrough(input) + GetAttributeAtVertex(input, 1);
}
