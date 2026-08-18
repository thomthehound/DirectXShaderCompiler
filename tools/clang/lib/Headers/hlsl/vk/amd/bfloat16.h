// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_BFLOAT16_H_
#define _HLSL_VK_AMD_BFLOAT16_H_

#include <vk/spirv.h>

namespace vk {
namespace amd {

// Genuine SPIR-V BFloat16KHR types. Integer values are used only as
// bit-preserving transport at the HLSL boundary; BF16 arithmetic/conversion
// instructions never see integer operands.
using BFloat16 = vk::SpirvOpaqueType<
    /* OpTypeFloat */ 22,
    vk::Literal<vk::integral_constant<uint, 16> >,
    vk::Literal<vk::integral_constant<uint, 0> > >; // BFloat16KHR encoding

using BFloat16x2 = vk::SpirvOpaqueType<
    /* OpTypeVector */ 23, BFloat16,
    vk::Literal<vk::integral_constant<uint, 2> > >;

template <typename To, typename From>
[[vk::ext_instruction(/* OpBitcast */ 124)]] To Bitcast(From value);

// SPV_KHR_bfloat16 explicitly permits OpFConvert to and from BFloat16KHR.
// BF16 -> F32 is exact. F32 -> BF16 is a first-class floating conversion, but
// Vulkan does not define one universal rounding result for the narrowing step;
// callers that require a specific BF16 rounding mode need a stronger contract.
[[vk::ext_extension("SPV_KHR_bfloat16")]]
[[vk::ext_capability(/* BFloat16TypeKHR */ 5116)]]
[[vk::ext_instruction(/* OpFConvert */ 115)]] BFloat16
F32ToBF16Raw(float value);

[[vk::ext_extension("SPV_KHR_bfloat16")]]
[[vk::ext_capability(/* BFloat16TypeKHR */ 5116)]]
[[vk::ext_instruction(/* OpFConvert */ 115)]] BFloat16x2
F32x2ToBF16x2Raw(float2 value);

[[vk::ext_extension("SPV_KHR_bfloat16")]]
[[vk::ext_capability(/* BFloat16TypeKHR */ 5116)]]
[[vk::ext_instruction(/* OpFConvert */ 115)]] float
BF16ToF32Raw(BFloat16 value);

[[vk::ext_extension("SPV_KHR_bfloat16")]]
[[vk::ext_capability(/* BFloat16TypeKHR */ 5116)]]
[[vk::ext_instruction(/* OpFConvert */ 115)]] float2
BF16x2ToF32x2Raw(BFloat16x2 value);

uint F32ToBF16Bits(float value) {
  return uint(Bitcast<uint16_t>(F32ToBF16Raw(value)));
}

uint F32x2ToBF16x2Bits(float2 value) {
  return Bitcast<uint>(F32x2ToBF16x2Raw(value));
}

float BF16BitsToF32(uint bits) {
  return BF16ToF32Raw(Bitcast<BFloat16>(uint16_t(bits)));
}

float2 BF16x2BitsToF32x2(uint bits) {
  return BF16x2ToF32x2Raw(Bitcast<BFloat16x2>(bits));
}

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_BFLOAT16_H_
