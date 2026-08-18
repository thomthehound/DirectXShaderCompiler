// RUN: %dxc -T cs_6_0 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

#include <vk/amd/subgroup.h>

// CHECK: OpCapability GroupNonUniformRotateKHR
// CHECK: OpExtension "SPV_KHR_subgroup_rotate"
// CHECK-COUNT-4: OpGroupNonUniformRotateKHR

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = tid.x * 17u + 3u;
  float f = float(tid.x) * 0.125f + 0.25f;
  uint delta = (tid.x & 3u) + 1u;

  uint wholeU = vk::amd::Rotate(u, delta);
  float wholeF = vk::amd::Rotate(f, delta);
  uint cluster8 = vk::amd::RotateClustered<8u>(u, 1u);
  float cluster16 = vk::amd::RotateClustered<16u>(f, 3u);

  Out[tid.x] = uint4(wholeU, asuint(wholeF), cluster8, asuint(cluster16));
}
