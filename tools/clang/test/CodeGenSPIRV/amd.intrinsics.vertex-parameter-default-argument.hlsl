// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s --check-prefix=UNIQUE
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD -DDEFAULT_SINGLE_OWNER %s | FileCheck %s --check-prefix=UNIQUE
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-extension=AMD -DDEFAULT_USED %s 2>&1 | FileCheck %s --check-prefix=SHARED

#include <vk/amd/intrinsics.h>

// A call in an omitted default argument belongs to the caller's entry.
// CXXDefaultArgExpr has no children: traversing the caller's ordinary AST
// children alone misses its call to the parameter helper.
// UNIQUE: InterpolateAtVertexAMD
// SHARED: error: AMD vertex-parameter helper requires a unique shader entry point

float4 readPixelInput() {
  return vk::AmdVertexParameter(1u, 0u);
}

float4 withDefault(float4 input = readPixelInput()) {
  return input;
}

[shader("pixel")]
float4 first(float4 input : TEXCOORD0) : SV_Target {
#ifdef DEFAULT_SINGLE_OWNER
  return input;
#else
  return readPixelInput();
#endif
}

[shader("pixel")]
float4 second(float2 lo : TEXCOORD0, float2 hi : TEXCOORD1) : SV_Target {
#if defined(DEFAULT_USED) || defined(DEFAULT_SINGLE_OWNER)
  return withDefault();
#else
  return withDefault(float4(lo, hi));
#endif
}
