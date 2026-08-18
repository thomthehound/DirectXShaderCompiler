// RUN: %dxc -T cs_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

#include <vk/amd/swizzle.h>

// CHECK: OpExtension "SPV_AMD_shader_ballot"
// CHECK: OpExtInstImport "SPV_AMD_shader_ballot"
// CHECK-COUNT-6: OpExtInst {{.*}} SwizzleInvocationsMaskedAMD

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = tid.x * 17u + 3u;
  float f = float(tid.x) + 0.25f;

  uint a = vk::amd::Swizzle<vk::amd::SwizzleSwapX1>(u);
  uint b = vk::amd::Swizzle<vk::amd::SwizzleReverseX8>(u);
  uint c = vk::amd::Swizzle<vk::amd::SwizzleBCastX4>(u);
  int d = vk::amd::Swizzle<vk::amd::SwizzleSwapX16>(asint(u));
  float e = vk::amd::Swizzle<vk::amd::SwizzleReverseX32>(f);
  float g = vk::amd::Swizzle<vk::amd::SwizzleBCastX32>(f);

  Out[tid.x] = uint4(a ^ b ^ c, asuint(d), asuint(e), asuint(g));
}
