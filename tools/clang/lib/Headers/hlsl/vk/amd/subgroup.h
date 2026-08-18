// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_SUBGROUP_H_
#define _HLSL_VK_AMD_SUBGROUP_H_

#include <vk/spirv.h>

namespace vk {
namespace amd {

// SPV_KHR_subgroup_rotate is supported by the SPIR-V Headers/Tools revisions
// pinned by this DXC fork. Keep it separate from generic WaveReadLaneAt shuffle
// helpers so APUSR can compare an exact rotate contract against shuffle-derived
// Radeon DPP/permlane codegen.
[[vk::ext_capability(6026)]] // GroupNonUniformRotateKHR
[[vk::ext_extension("SPV_KHR_subgroup_rotate")]]
[[vk::ext_instruction(/* OpGroupNonUniformRotateKHR */ 4431)]] float
SubgroupRotateRaw(vk::Scope execution, float value, uint delta);

[[vk::ext_capability(6026)]]
[[vk::ext_extension("SPV_KHR_subgroup_rotate")]]
[[vk::ext_instruction(/* OpGroupNonUniformRotateKHR */ 4431)]] uint
SubgroupRotateRaw(vk::Scope execution, uint value, uint delta);

[[vk::ext_capability(6026)]]
[[vk::ext_extension("SPV_KHR_subgroup_rotate")]]
[[vk::ext_instruction(/* OpGroupNonUniformRotateKHR */ 4431)]] int
SubgroupRotateRaw(vk::Scope execution, int value, uint delta);

[[vk::ext_capability(6026)]]
[[vk::ext_extension("SPV_KHR_subgroup_rotate")]]
[[vk::ext_instruction(/* OpGroupNonUniformRotateKHR */ 4431)]] float
SubgroupRotateClusteredRaw(vk::Scope execution, float value, uint delta,
                           uint clusterSize);

[[vk::ext_capability(6026)]]
[[vk::ext_extension("SPV_KHR_subgroup_rotate")]]
[[vk::ext_instruction(/* OpGroupNonUniformRotateKHR */ 4431)]] uint
SubgroupRotateClusteredRaw(vk::Scope execution, uint value, uint delta,
                           uint clusterSize);

[[vk::ext_capability(6026)]]
[[vk::ext_extension("SPV_KHR_subgroup_rotate")]]
[[vk::ext_instruction(/* OpGroupNonUniformRotateKHR */ 4431)]] int
SubgroupRotateClusteredRaw(vk::Scope execution, int value, uint delta,
                           uint clusterSize);

float Rotate(float value, uint delta) {
  return SubgroupRotateRaw(vk::ScopeSubgroup, value, delta);
}
uint Rotate(uint value, uint delta) {
  return SubgroupRotateRaw(vk::ScopeSubgroup, value, delta);
}
int Rotate(int value, uint delta) {
  return SubgroupRotateRaw(vk::ScopeSubgroup, value, delta);
}

float RotateClustered(float value, uint delta, uint clusterSize) {
  return SubgroupRotateClusteredRaw(vk::ScopeSubgroup, value, delta,
                                    clusterSize);
}
uint RotateClustered(uint value, uint delta, uint clusterSize) {
  return SubgroupRotateClusteredRaw(vk::ScopeSubgroup, value, delta,
                                    clusterSize);
}
int RotateClustered(int value, uint delta, uint clusterSize) {
  return SubgroupRotateClusteredRaw(vk::ScopeSubgroup, value, delta,
                                    clusterSize);
}

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_SUBGROUP_H_
