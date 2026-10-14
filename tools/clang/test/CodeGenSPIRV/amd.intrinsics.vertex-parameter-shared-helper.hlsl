// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD %s 2>&1 | FileCheck %s --check-prefix=SHARED
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD -DUNIQUE_OWNER %s | FileCheck %s --check-prefix=UNIQUE
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD -DUNIQUE_OWNER -DVERTEX_OWNER %s 2>&1 | FileCheck %s --check-prefix=NONPIXEL
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD -DUNIQUE_OWNER -DAMD_EXPORT_HELPER %s 2>&1 | FileCheck %s --check-prefix=EXPORT-BLOCKED

#include <vk/amd/intrinsics.h>

// One translated helper cannot embed a pointer to either entry's input
// signature when the same function is reachable from both entry points.
// SHARED: error: AMD vertex-parameter helper requires a unique shader entry point
// UNIQUE-COUNT-5: InterpolateAtVertexAMD
// NONPIXEL: error: AMD vertex-parameter access is only valid in a pixel shader
// EXPORT-BLOCKED: fatal error: generated SPIR-V is invalid: Capability Linkage is not allowed by Vulkan

// This internal helper is declared before both entry points. Unlike an HLSL
// export, it must produce a Vulkan-valid shader module when uniquely owned.
// The exported variant separately checks the existing linkage limitation.
#ifdef AMD_EXPORT_HELPER
export
#endif
float4 readPixelInput() {
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
