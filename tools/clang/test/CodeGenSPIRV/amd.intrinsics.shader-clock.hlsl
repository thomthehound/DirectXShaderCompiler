// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

#include <vk/amd/clock.h>

// CHECK: OpCapability ShaderClockKHR
// CHECK: OpExtension "SPV_KHR_shader_clock"
// CHECK-COUNT-2: OpReadClockKHR

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint64_t subgroupClock = vk::amd::ShaderClock();
  uint64_t deviceClock = vk::amd::DeviceClockNoBarrier();
  Out[tid.x] = uint4(uint(subgroupClock), uint(subgroupClock >> 32u),
                     uint(deviceClock), uint(deviceClock >> 32u));
}
