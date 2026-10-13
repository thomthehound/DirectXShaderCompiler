// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD %s 2>&1 | FileCheck %s

#include <vk/amd/intrinsics.h>

// One translated helper cannot embed a pointer to either entry's input
// signature when the same function is reachable from both entry points.
// CHECK: error: AMD vertex-parameter helper requires a unique shader entry point

float4 readPixelInput() {
  return vk::AmdVertexParameter(1u, 0u);
}

[shader("pixel")]
float4 first(float4 lo : TEXCOORD0) : SV_Target {
  return readPixelInput();
}

[shader("pixel")]
float4 second(float2 lo : TEXCOORD0, float2 hi : TEXCOORD1) : SV_Target {
  return readPixelInput();
}
