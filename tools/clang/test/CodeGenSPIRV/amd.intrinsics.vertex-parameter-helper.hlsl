// RUN: %dxc -T ps_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

#include <vk/amd/intrinsics.h>

// The helper is translated after the entry wrapper, but its raw register
// lookup must still resolve against that entry point's input signature.
// CHECK: OpExtension "SPV_AMD_shader_explicit_vertex_parameter"
// CHECK-COUNT-4: InterpolateAtVertexAMD

float4 readPixelInput() {
  return vk::AmdVertexParameter(1u, 0u);
}

float4 main(float4 input : TEXCOORD0) : SV_Target {
  return readPixelInput();
}
