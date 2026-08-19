// RUN: not %dxc -T ps_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s 2>&1 | FileCheck %s

#include <vk/amd/intrinsics.h>

struct PSIn {
  float2 value : TEXCOORD0;
  nointerpolation uint parameter : TEXCOORD1;
};

// CHECK: error: AMD vertex-parameter indices must be compile-time integer constants
float4 main(PSIn input) : SV_Target {
  return vk::AmdVertexParameter(1u, input.parameter);
}
