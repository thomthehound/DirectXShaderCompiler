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

// GFX11's v_dot4_i32_iu8 supports signedness independently for both packed
// byte operands. SPIR-V has mixed-signed integer dot semantics, but this DXC
// path is kept canonical until direct capability/operand handling is proven.
int SUDot4I8U8(uint signedA, uint unsignedB, int accum) {
  int sum = accum;
  sum += SBfe(asint(signedA), 0u, 8u) * int(UBfe(unsignedB, 0u, 8u));
  sum += SBfe(asint(signedA), 8u, 8u) * int(UBfe(unsignedB, 8u, 8u));
  sum += SBfe(asint(signedA), 16u, 8u) * int(UBfe(unsignedB, 16u, 8u));
  sum += SBfe(asint(signedA), 24u, 8u) * int(UBfe(unsignedB, 24u, 8u));
  return sum;
}
int USDot4U8I8(uint unsignedA, uint signedB, int accum) {
  int sum = accum;
  sum += int(UBfe(unsignedA, 0u, 8u)) * SBfe(asint(signedB), 0u, 8u);
  sum += int(UBfe(unsignedA, 8u, 8u)) * SBfe(asint(signedB), 8u, 8u);
  sum += int(UBfe(unsignedA, 16u, 8u)) * SBfe(asint(signedB), 16u, 8u);
  sum += int(UBfe(unsignedA, 24u, 8u)) * SBfe(asint(signedB), 24u, 8u);
  return sum;
}

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_DOT_H_
