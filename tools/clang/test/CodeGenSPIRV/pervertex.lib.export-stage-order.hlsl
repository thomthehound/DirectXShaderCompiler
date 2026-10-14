// XFAIL: *
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s

// The entry is translated before the export, which leaves SpirvContext in
// Library mode. Per-vertex lowering is nevertheless required for the pixel
// entry's actual interface, without modifying the unrelated export's ABI.
// CHECK: OpEntryPoint Fragment
// CHECK-DAG: OpDecorate [[EXPORT:%[a-zA-Z0-9_]+]] LinkageAttributes "unrelated" Export
// CHECK-DAG: OpDecorate [[INPUT:%[a-zA-Z0-9_]+]] PerVertexKHR
// CHECK: OpTypeArray %v4float %uint_3
// CHECK: [[INPUT]] = OpVariable %_ptr_Input__arr_v4float_uint_3 Input

[shader("pixel")]
float4 pixelMain(nointerpolation float4 input : TEXCOORD0) : SV_Target {
  return GetAttributeAtVertex(input, 1);
}

export float4 unrelated(float4 value) {
  return value;
}
