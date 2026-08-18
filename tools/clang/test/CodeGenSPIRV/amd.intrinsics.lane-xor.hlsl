// RUN: %dxc -T cs_6_0 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

#include <vk/amd/intrinsics.h>

// APUSR's imported FP16 operators use lane XOR 1/2/4/8/16/32 butterfly stages.
// Preserve those as fixed subgroup shuffles. XOR32 is only executed on wave64.
// The backend may later recover DPP or permlane, but this test only asserts the
// exact SPIR-V routing structure.
// CHECK: OpCapability GroupNonUniformShuffle
// CHECK-COUNT-12: OpBitwiseXor
// CHECK-COUNT-12: OpGroupNonUniformShuffle
// CHECK-NOT: OpLoopMerge

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float f = float(tid.x) + 0.25f;
  uint u = tid.x * 17u + 3u;

  float f1 = vk::amd::LaneXor1(f);
  float f2 = vk::amd::LaneXor2(f);
  float f4 = vk::amd::LaneXor4(f);
  float f8 = vk::amd::LaneXor8(f);
  float f16 = vk::amd::LaneXor16(f);
  uint u1 = vk::amd::LaneXor1(u);
  uint u2 = vk::amd::LaneXor2(u);
  uint u4 = vk::amd::LaneXor4(u);
  uint u8 = vk::amd::LaneXor8(u);
  uint u16 = vk::amd::LaneXor16(u);

  float hiF = 0.0f;
  uint hiU = 0u;
  if (vk::amd::WaveSize() > 32u) {
    hiF = vk::amd::LaneXor32(f);
    hiU = vk::amd::LaneXor32(u);
  }

  Out[tid.x] = uint4(
      asuint(f1 + f2 + f4),
      asuint(f8 + f16 + hiF),
      u1 ^ u2 ^ u4,
      u8 ^ u16 ^ hiU);
}
