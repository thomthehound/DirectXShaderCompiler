// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_CLOCK_H_
#define _HLSL_VK_AMD_CLOCK_H_

#include <vk/spirv.h>

namespace vk {
namespace amd {

[[vk::ext_capability(5055)]] // ShaderClockKHR
[[vk::ext_extension("SPV_KHR_shader_clock")]]
[[vk::ext_instruction(/* OpReadClockKHR */ 5056)]] uint64_t
ReadClockRaw(vk::Scope scope);

// AGS ShaderClock samples a monotonically increasing timestamp without a
// cross-device-coherence or code-motion-barrier requirement. Subgroup-scope
// OpReadClockKHR preserves that contract and is the standard Vulkan mapping for
// the ordinary shader clock.
uint64_t ShaderClock() { return ReadClockRaw(vk::ScopeSubgroup); }

// Useful qualification primitive only. Device-scope OpReadClockKHR samples a
// device-consistent clock, but SPV_KHR_shader_clock explicitly does NOT promise
// a code-motion barrier. AMD AGS ShaderRealtimeClock does. Do not substitute
// this helper for AGS ShaderRealtimeClock or mark it as parity.
uint64_t DeviceClockNoBarrier() { return ReadClockRaw(vk::ScopeDevice); }

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_CLOCK_H_
