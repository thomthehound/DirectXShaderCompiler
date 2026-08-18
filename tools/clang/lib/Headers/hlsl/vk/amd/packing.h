// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_PACKING_H_
#define _HLSL_VK_AMD_PACKING_H_

#include <vk/spirv.h>

namespace vk {
namespace amd {

// Direct GLSL.std.450 normalized packing contracts. These preserve the
// two-lane normalized-conversion operation in SPIR-V so Radeon can select the
// native v_cvt_pknorm_i16/u16 family when profitable.
[[vk::ext_instruction(/* PackSnorm2x16 */ 56, "GLSL.std.450")]] uint
PackSnorm2x16(float2 value);

[[vk::ext_instruction(/* PackUnorm2x16 */ 57, "GLSL.std.450")]] uint
PackUnorm2x16(float2 value);

// Four-byte normalized packing is not the same contract as
// v_cvt_pk_u8_f32 (which inserts one converted byte into an existing dword),
// but retaining this high-level identity gives Radeon the strongest standard
// representation from which to recover an efficient packed-U8 sequence.
[[vk::ext_instruction(/* PackSnorm4x8 */ 54, "GLSL.std.450")]] uint
PackSnorm4x8(float4 value);

[[vk::ext_instruction(/* PackUnorm4x8 */ 55, "GLSL.std.450")]] uint
PackUnorm4x8(float4 value);

// Exact canonical graphs for AMD's packed signed/unsigned 16-bit integer
// conversion instructions. The hardware operation saturates each 32-bit lane
// to the destination 16-bit range before packing the two low halves.
uint PackI16x2(int2 value) {
  int2 clamped = clamp(value, int2(-32768, -32768), int2(32767, 32767));
  return (uint(clamped.x) & 0xffffu) | ((uint(clamped.y) & 0xffffu) << 16u);
}

uint PackU16x2(uint2 value) {
  uint2 clamped = min(value, uint2(65535u, 65535u));
  return (clamped.x & 0xffffu) | ((clamped.y & 0xffffu) << 16u);
}

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_PACKING_H_
