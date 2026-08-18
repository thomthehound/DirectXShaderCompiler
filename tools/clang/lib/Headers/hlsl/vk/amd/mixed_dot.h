// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_MIXED_DOT_H_
#define _HLSL_VK_AMD_MIXED_DOT_H_

#include <vk/spirv.h>

namespace vk {
namespace amd {

// A genuine SPIR-V bfloat16 scalar and 2-lane vector. Integer values are used
// only as bit-preserving transport at the HLSL boundary; arithmetic never sees
// them as integer operands.
using BFloat16 = vk::SpirvOpaqueType<
    /* OpTypeFloat */ 22,
    vk::Literal<vk::integral_constant<uint, 16> >,
    vk::Literal<vk::integral_constant<uint, 0> > >; // BFloat16KHR encoding

using BFloat16x2 = vk::SpirvOpaqueType<
    /* OpTypeVector */ 23, BFloat16,
    vk::Literal<vk::integral_constant<uint, 2> > >;

template <typename To, typename From>
[[vk::ext_instruction(/* OpBitcast */ 124)]] To Bitcast(From value);

// SPV_VALVE_mixed_float_dot_product preserves the mixed-precision operation
// directly instead of forcing Radeon to reconstruct it from unpacked scalar
// arithmetic.
[[vk::ext_extension("SPV_VALVE_mixed_float_dot_product")]]
[[vk::ext_capability(/* DotProductFloat16AccFloat32VALVE */ 6912)]]
[[vk::ext_instruction(/* OpFDot2MixAcc32VALVE */ 6916)]] float
Dot2F16AccF32(float16_t2 a, float16_t2 b, float accumulator);

[[vk::ext_extension("SPV_VALVE_mixed_float_dot_product")]]
[[vk::ext_capability(/* DotProductFloat16AccFloat16VALVE */ 6913)]]
[[vk::ext_instruction(/* OpFDot2MixAcc16VALVE */ 6917)]] float16_t
Dot2F16AccF16(float16_t2 a, float16_t2 b, float16_t accumulator);

// The BF16 mixed-dot capability implicitly declares BFloat16TypeKHR. DXC's
// capability visitor also sees the encoded OpTypeFloat and requests the KHR
// BF16 extension, so these declarations only need to name the instruction's
// VALVE extension.
[[vk::ext_extension("SPV_VALVE_mixed_float_dot_product")]]
[[vk::ext_capability(/* DotProductBFloat16AccVALVE */ 6914)]]
[[vk::ext_instruction(/* OpFDot2MixAcc32VALVE */ 6916)]] float
Dot2BF16AccF32Raw(BFloat16x2 a, BFloat16x2 b, float accumulator);

[[vk::ext_extension("SPV_VALVE_mixed_float_dot_product")]]
[[vk::ext_capability(/* DotProductBFloat16AccVALVE */ 6914)]]
[[vk::ext_instruction(/* OpFDot2MixAcc16VALVE */ 6917)]] BFloat16
Dot2BF16AccBF16Raw(BFloat16x2 a, BFloat16x2 b, BFloat16 accumulator);

// SPV_KHR_bfloat16 also provides the non-accumulating BF16 OpDot contract when
// the result remains BF16.
[[vk::ext_extension("SPV_KHR_bfloat16")]]
[[vk::ext_capability(/* BFloat16DotProductKHR */ 5117)]]
[[vk::ext_instruction(/* OpDot */ 148)]] BFloat16
Dot2BF16Raw(BFloat16x2 a, BFloat16x2 b);

// Packed transport helpers are intentionally zero-arithmetic OpBitcast paths.
float Dot2F16PackedAccF32(uint aBits, uint bBits, float accumulator) {
  return Dot2F16AccF32(Bitcast<float16_t2>(aBits),
                       Bitcast<float16_t2>(bBits), accumulator);
}

uint Dot2F16PackedAccF16Bits(uint aBits, uint bBits, uint16_t accumulatorBits) {
  float16_t result = Dot2F16AccF16(
      Bitcast<float16_t2>(aBits), Bitcast<float16_t2>(bBits),
      Bitcast<float16_t>(accumulatorBits));
  return uint(Bitcast<uint16_t>(result));
}

float Dot2BF16AccF32(uint aBits, uint bBits, float accumulator) {
  return Dot2BF16AccF32Raw(Bitcast<BFloat16x2>(aBits),
                           Bitcast<BFloat16x2>(bBits), accumulator);
}

uint Dot2BF16AccBF16Bits(uint aBits, uint bBits, uint16_t accumulatorBits) {
  BFloat16 result = Dot2BF16AccBF16Raw(
      Bitcast<BFloat16x2>(aBits), Bitcast<BFloat16x2>(bBits),
      Bitcast<BFloat16>(accumulatorBits));
  return uint(Bitcast<uint16_t>(result));
}

uint Dot2BF16Bits(uint aBits, uint bBits) {
  BFloat16 result =
      Dot2BF16Raw(Bitcast<BFloat16x2>(aBits), Bitcast<BFloat16x2>(bBits));
  return uint(Bitcast<uint16_t>(result));
}

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_MIXED_DOT_H_
