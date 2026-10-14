// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv %s | FileCheck %s

// Per-vertex interpolation in a library is selected by the actual entry
// execution models, not the stage left in SpirvContext after translation.
// CHECK: OpExtension "SPV_KHR_fragment_shader_barycentric"
// CHECK-COUNT-2: OpEntryPoint Fragment
// CHECK-COUNT-2: PerVertexKHR
// CHECK: OpTypeArray %v4float %uint_3

[shader("pixel")]
float4 first(nointerpolation float4 a : TEXCOORD0) : SV_Target {
  return GetAttributeAtVertex(a, 1);
}

[shader("pixel")]
float4 second(nointerpolation float4 b : TEXCOORD1) : SV_Target {
  return GetAttributeAtVertex(b, 2);
}
