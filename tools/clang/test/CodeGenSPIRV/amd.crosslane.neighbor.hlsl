// RUN: %dxc -T cs_6_0 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

#include <vk/amd/crosslane.h>

// CHECK: OpCapability GroupNonUniformShuffle
// CHECK: OpGroupNonUniformShuffle
// CHECK: OpIEqual
// CHECK: OpSelect

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = tid.x * 0x9e3779b9u + 7u;
  float f = float(tid.x) * 0.25f + 0.5f;

  uint up = vk::amd::LaneUp1(u) ^ vk::amd::LaneUp4(u);
  uint down = vk::amd::LaneDown1(u) ^ vk::amd::LaneDown4(u);
  uint rr = vk::amd::RowRotateRight1(u) ^ vk::amd::RowRotateRight4(u);
  float rl = vk::amd::RowRotateLeft1(f) + vk::amd::RowRotateLeft4(f);

  Out[tid.x] = uint4(up, down, rr, asuint(rl));
}
