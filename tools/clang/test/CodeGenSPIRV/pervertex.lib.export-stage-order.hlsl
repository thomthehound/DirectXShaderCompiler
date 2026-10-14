// XFAIL: *
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s

// A following export is translated with Library stage context. It must not
// prevent per-vertex input lowering for the preceding Fragment entry.
// Unlike a pixel-only entry fixture, this fails with the old context.isPS()
// module gate and must produce PerVertexKHR and the expanded input layout.
// CHECK: OpEntryPoint Fragment
// CHECK: OpDecorate {{%[a-zA-Z0-9_]+}} PerVertexKHR
// CHECK: OpTypeArray %v4float %uint_3

[shader("pixel")]
float4 pixelMain(nointerpolation float4 input : TEXCOORD0) : SV_Target {
  return GetAttributeAtVertex(input, 1);
}

export float4 unrelated(float4 value) {
  return value;
}
