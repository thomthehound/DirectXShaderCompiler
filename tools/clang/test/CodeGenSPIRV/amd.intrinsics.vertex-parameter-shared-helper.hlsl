// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD %s 2>&1 | FileCheck %s --check-prefix=SHARED
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD -DUNIQUE_OWNER %s | FileCheck %s --check-prefix=UNIQUE
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD -DUNIQUE_OWNER -DVERTEX_OWNER %s 2>&1 | FileCheck %s --check-prefix=NONPIXEL

#include <vk/amd/intrinsics.h>

// One translated helper cannot embed a pointer to either entry's input
// signature when the same function is reachable from both entry points.
// SHARED: error: AMD vertex-parameter helper requires a unique shader entry point
// UNIQUE-COUNT-5: InterpolateAtVertexAMD
// NONPIXEL: error: AMD vertex-parameter access is only valid in a pixel shader

// An exported helper is queued before either entry point when declared first.
// Its uniquely owned case must not depend on source declaration order.
export float4 readPixelInput() {
  return vk::AmdVertexParameter(1u, 0u) +
         float4(vk::AmdVertexParameterComponent(1u, 0u, 0u), 0, 0, 0);
}

#ifdef VERTEX_OWNER
[shader("vertex")]
float4 first(float4 lo : TEXCOORD0) : SV_Position {
  return readPixelInput();
}
#else
[shader("pixel")]
float4 first(float4 lo : TEXCOORD0) : SV_Target {
  return readPixelInput();
}
#endif

[shader("pixel")]
float4 second(float2 lo : TEXCOORD0, float2 hi : TEXCOORD1) : SV_Target {
#ifdef UNIQUE_OWNER
  return float4(lo, hi);
#else
  return readPixelInput();
#endif
}
