// RUN: not %dxc -T ps_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s 2>&1 | FileCheck %s

#include <vk/amd/intrinsics.h>

// Pixel input parameter registers are limited to the actual packed input
// signature, independently of any Vulkan interface Location assignment.
// CHECK: error: AMD vertex-parameter register component is not backed by a pixel shader input

float4 main(float4 input : TEXCOORD0) : SV_Target {
  return vk::AmdVertexParameter(0u, 31u);
}
