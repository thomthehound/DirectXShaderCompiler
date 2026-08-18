// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_DOT_H_
#define _HLSL_VK_AMD_DOT_H_

#include <vk/amd/math.h>

namespace vk {
namespace amd {

// Standardized packed 4x8 contracts. DXC emits SPV_KHR_integer_dot_product
// OpSDot/OpUDot directly for these HLSL builtins.
int SDot4I8(uint a, uint b, int accum) {
  return dot4add_i8packed(a, b, accum);
}
uint UDot4U8(uint a, uint b, uint accum) {
  return dot4add_u8packed(a, b, accum);
}

// Exact saturating-accumulate variants from SPV_KHR_integer_dot_product.
// Normal HLSL dot4add is intentionally non-saturating, so these are separate
// AMD-oriented entry points rather than a semantic change to dot4add itself.
// PackedVectorFormat4x8BitKHR is literal value 0.
[[vk::ext_capability(6019)]] // DotProductKHR
[[vk::ext_capability(6018)]] // DotProductInput4x8BitPackedKHR
[[vk::ext_extension("SPV_KHR_integer_dot_product")]]
[[vk::ext_instruction(/* OpSDotAccSatKHR */ 4453)]] int
SDot4I8AccSatRaw(uint a, uint b, int accum,
                 [[vk::ext_literal]] int packedVectorFormat);

[[vk::ext_capability(6019)]] // DotProductKHR
[[vk::ext_capability(6018)]] // DotProductInput4x8BitPackedKHR
[[vk::ext_extension("SPV_KHR_integer_dot_product")]]
[[vk::ext_instruction(/* OpUDotAccSatKHR */ 4454)]] uint
UDot4U8AccSatRaw(uint a, uint b, uint accum,
                 [[vk::ext_literal]] int packedVectorFormat);

int SDot4I8AccSat(uint a, uint b, int accum) {
  return SDot4I8AccSatRaw(a, b, accum, 0);
}
uint UDot4U8AccSat(uint a, uint b, uint accum) {
  return UDot4U8AccSatRaw(a, b, accum, 0);
}

// SPIR-V directly standardizes mixed-sign integer dot. Vector 1 is interpreted
// as signed and Vector 2 as unsigned; swapping operands implements the reverse
// signedness. Unlike the previous scalar extract/multiply candidate, this keeps
// mixed packed-dot identity intact at the DXC -> Vulkan boundary.
[[vk::ext_capability(6019)]] // DotProductKHR
[[vk::ext_capability(6018)]] // DotProductInput4x8BitPackedKHR
[[vk::ext_extension("SPV_KHR_integer_dot_product")]]
[[vk::ext_instruction(/* OpSUDotKHR */ 4452)]] int
SUDot4I8U8Raw(uint signedA, uint unsignedB,
              [[vk::ext_literal]] int packedVectorFormat);

[[vk::ext_capability(6019)]] // DotProductKHR
[[vk::ext_capability(6018)]] // DotProductInput4x8BitPackedKHR
[[vk::ext_extension("SPV_KHR_integer_dot_product")]]
[[vk::ext_instruction(/* OpSUDotAccSatKHR */ 4455)]] int
SUDot4I8U8AccSatRaw(uint signedA, uint unsignedB, int accum,
                    [[vk::ext_literal]] int packedVectorFormat);

int SUDot4I8U8(uint signedA, uint unsignedB, int accum) {
  return SUDot4I8U8Raw(signedA, unsignedB, 0) + accum;
}
int USDot4U8I8(uint unsignedA, uint signedB, int accum) {
  return SUDot4I8U8Raw(signedB, unsignedA, 0) + accum;
}
int SUDot4I8U8AccSat(uint signedA, uint unsignedB, int accum) {
  return SUDot4I8U8AccSatRaw(signedA, unsignedB, accum, 0);
}
int USDot4U8I8AccSat(uint unsignedA, uint signedB, int accum) {
  return SUDot4I8U8AccSatRaw(signedB, unsignedA, accum, 0);
}

// Canonical AMD 2x16 integer dot forms. These preserve exact packed-lane
// signedness while giving Radeon a recognizable graph for v_dot2_*_i16.
int SDot2I16(uint a, uint b, int accum) {
  int a0 = SBfe(asint(a), 0u, 16u);
  int a1 = SBfe(asint(a), 16u, 16u);
  int b0 = SBfe(asint(b), 0u, 16u);
  int b1 = SBfe(asint(b), 16u, 16u);
  return accum + a0 * b0 + a1 * b1;
}
uint UDot2U16(uint a, uint b, uint accum) {
  uint a0 = UBfe(a, 0u, 16u);
  uint a1 = UBfe(a, 16u, 16u);
  uint b0 = UBfe(b, 0u, 16u);
  uint b1 = UBfe(b, 16u, 16u);
  return accum + a0 * b0 + a1 * b1;
}

// Packed half inputs without first-class Float16 SPIR-V arithmetic. This maps
// directly onto the representation APUSR frequently has at resource boundaries:
// two IEEE binary16 values in each uint. f16tof32 defines the conversion; both
// products and the accumulator are FP32. Radeon can therefore attempt to recover
// v_dot2_f32_f16 without the probe device needing shaderFloat16 enabled.
float FDot2F32F16Bits(uint packedA, uint packedB, float accum) {
  float a0 = f16tof32(packedA & 0xffffu);
  float a1 = f16tof32(packedA >> 16u);
  float b0 = f16tof32(packedB & 0xffffu);
  float b1 = f16tof32(packedB >> 16u);
  return accum + a0 * b0 + a1 * b1;
}

// AMD's packed 8x4 integer dot family. SPIR-V has no established packed-i4
// contract equivalent to the AMD instruction surface, so preserve the exact
// nibble semantics as a canonical recovery candidate. The installed-driver ISA
// probe decides whether Radeon combines this graph into v_dot8_*_i4.
int SDot8I4(uint a, uint b, int accum) {
  int sum = accum;
  [unroll]
  for (uint lane = 0u; lane < 8u; ++lane) {
    uint shift = lane * 4u;
    sum += SBfe(asint(a), shift, 4u) * SBfe(asint(b), shift, 4u);
  }
  return sum;
}
uint UDot8U4(uint a, uint b, uint accum) {
  uint sum = accum;
  [unroll]
  for (uint lane = 0u; lane < 8u; ++lane) {
    uint shift = lane * 4u;
    sum += UBfe(a, shift, 4u) * UBfe(b, shift, 4u);
  }
  return sum;
}
int SUDot8I4U4(uint signedA, uint unsignedB, int accum) {
  int sum = accum;
  [unroll]
  for (uint lane = 0u; lane < 8u; ++lane) {
    uint shift = lane * 4u;
    sum += SBfe(asint(signedA), shift, 4u) * int(UBfe(unsignedB, shift, 4u));
  }
  return sum;
}
int USDot8U4I4(uint unsignedA, uint signedB, int accum) {
  int sum = accum;
  [unroll]
  for (uint lane = 0u; lane < 8u; ++lane) {
    uint shift = lane * 4u;
    sum += int(UBfe(unsignedA, shift, 4u)) * SBfe(asint(signedB), shift, 4u);
  }
  return sum;
}

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_DOT_H_
