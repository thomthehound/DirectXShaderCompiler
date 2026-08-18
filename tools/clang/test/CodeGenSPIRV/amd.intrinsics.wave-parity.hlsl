// RUN: %dxc -T cs_6_0 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

#include <vk/amd/intrinsics.h>

// CHECK: OpCapability GroupNonUniform
// CHECK: OpCapability GroupNonUniformBallot
// CHECK: OpCapability GroupNonUniformShuffle
// CHECK: OpGroupNonUniformBroadcastFirst
// CHECK: OpGroupNonUniformShuffle
// CHECK: OpGroupNonUniformBallot
// CHECK: OpGroupNonUniformAny
// CHECK: OpGroupNonUniformAll
// CHECK-NOT: OpExtInst {{.*}} SwizzleInvocationsAMD

RWStructuredBuffer<uint4> outBuffer : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint lane = vk::amd::LaneId();
  uint dynamicLane = (lane * 7u + 3u) % vk::amd::WaveSize();

  uint first = vk::amd::ReadFirstLane(tid.x ^ 0x55aa55aau);
  uint selected = vk::amd::ReadLaneAt(tid.x + lane * 17u, dynamicLane);
  uint2 ballot = vk::amd::Ballot((lane & 3u) == 1u);
  bool any = vk::amd::BallotAny((lane & 7u) == 5u);
  bool all = vk::amd::BallotAll(lane < vk::amd::WaveSize());

  outBuffer[tid.x] = uint4(first, selected, ballot.x ^ ballot.y,
                           uint(any) | (uint(all) << 1));
}
