// RUN: %dxc -T ps_6_1 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

#include <vk/amd/barycentric.h>

// CHECK: OpExtension "SPV_AMD_shader_explicit_vertex_parameter"
// CHECK: BuiltIn BaryCoordPullModelAMD
// CHECK: OpVariable {{.*}} Input

float4 main() : SV_Target0 {
  float3 pull = vk::amd::PullModelBarycentricCoords();
  return float4(pull, pull.x + pull.y + pull.z);
}
