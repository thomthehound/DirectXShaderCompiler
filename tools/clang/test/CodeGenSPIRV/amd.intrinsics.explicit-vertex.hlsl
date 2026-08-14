// RUN: %dxc -T ps_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

#include <vk/amd/intrinsics.h>

// CHECK: OpCapability InterpolationFunction
// CHECK: OpExtension "SPV_AMD_shader_explicit_vertex_parameter"
// CHECK: [[INTERP:%[0-9]+]] = OpExtInstImport "SPV_AMD_shader_explicit_vertex_parameter"
// CHECK: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD

struct PSIn {
  float4 position : SV_Position;
  float value : TEXCOORD0;
};

float4 main(PSIn input) : SV_Target {
  float value = vk::amd::InterpolateAtVertex(input.value, 0);
  return value.xxxx;
}
