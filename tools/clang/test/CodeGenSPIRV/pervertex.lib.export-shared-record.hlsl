// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s

// One source record must be lowered to two distinct SPIR-V representations:
// the exported callable keeps its ordinary float4 field, while the pixel
// input receives a per-vertex array. No FieldDecl mutation is permitted.
// CHECK: OpEntryPoint Fragment
// CHECK-DAG: [[ORDINARY:%[a-zA-Z0-9_]+]] = OpTypeStruct %v4float
// CHECK-DAG: [[SPECIAL:%[a-zA-Z0-9_]+]] = OpTypeStruct %_arr_v4float_uint_3
// CHECK: PerVertexKHR

struct SharedData {
  nointerpolation float4 color : TEXCOORD0;
};

export float4 exportedRead(SharedData value) {
  return value.color;
}

[shader("pixel")]
float4 pixelMain(SharedData input) : SV_Target {
  return GetAttributeAtVertex(input.color, 1);
}
