// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_VALVE_MIXED_FLOAT_DOT_PRODUCT_H_
#define _HLSL_VK_VALVE_MIXED_FLOAT_DOT_PRODUCT_H_

// Exact HLSL spellings for SPV_VALVE_mixed_float_dot_product.
//
// These are first-class SPIR-V dot-accumulate operations, not arithmetic
// reconstruction candidates. BF16 overloads are intentionally not represented
// as uint16_t vectors: doing so would destroy the BFloat16KHR type encoding the
// SPIR-V instruction requires.
namespace vk {
namespace valve {

[[vk::ext_capability(6912)]]
[[vk::ext_extension("SPV_VALVE_mixed_float_dot_product")]]
[[vk::ext_instruction(/* OpFDot2MixAcc32VALVE */ 6916)]]
float FDot2Acc32(float16_t2 a, float16_t2 b, float accumulator);

[[vk::ext_capability(6913)]]
[[vk::ext_extension("SPV_VALVE_mixed_float_dot_product")]]
[[vk::ext_instruction(/* OpFDot2MixAcc16VALVE */ 6917)]]
float16_t FDot2Acc16(float16_t2 a, float16_t2 b, float16_t accumulator);

} // namespace valve
} // namespace vk

#endif // _HLSL_VK_VALVE_MIXED_FLOAT_DOT_PRODUCT_H_
