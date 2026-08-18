// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_MATH_H_
#define _HLSL_VK_AMD_MATH_H_

#include <vk/spirv.h>

// AMD-oriented low-level math surface for Vulkan.
//
// Direct contracts preserve an operation explicitly in SPIR-V. Canonical
// recovery candidates preserve exact source semantics while exposing a graph
// that an AMD Vulkan compiler can potentially combine back to native ISA.
// Candidates are not called native until the installed-driver probe proves it.
namespace vk {
namespace amd {

// Direct core SPIR-V integer/bit contracts.
[[vk::ext_instruction(/* OpBitFieldUExtract */ 203)]] uint
UBfe(uint base, uint offset, uint count);

[[vk::ext_instruction(/* OpBitFieldSExtract */ 202)]] int
SBfe(int base, uint offset, uint count);

[[vk::ext_instruction(/* OpBitReverse */ 204)]] uint BitReverse(uint value);
[[vk::ext_instruction(/* OpBitCount */ 205)]] uint BitCount(uint value);

uint AbsDiffU32(uint a, uint b) { return a > b ? a - b : b - a; }

// Exact semantic graphs for the scalar SAD family. These are intentionally
// unrolled so an AMD Vulkan compiler sees the lane structure directly.
uint SadU8(uint a, uint b, uint accum) {
  uint d0 = AbsDiffU32(UBfe(a, 0u, 8u), UBfe(b, 0u, 8u));
  uint d1 = AbsDiffU32(UBfe(a, 8u, 8u), UBfe(b, 8u, 8u));
  uint d2 = AbsDiffU32(UBfe(a, 16u, 8u), UBfe(b, 16u, 8u));
  uint d3 = AbsDiffU32(UBfe(a, 24u, 8u), UBfe(b, 24u, 8u));
  return accum + d0 + d1 + d2 + d3;
}

uint SadHiU8(uint a, uint b, uint accum) {
  return accum + ((SadU8(a, b, 0u)) << 16u);
}

uint SadU16(uint a, uint b, uint accum) {
  uint d0 = AbsDiffU32(UBfe(a, 0u, 16u), UBfe(b, 0u, 16u));
  uint d1 = AbsDiffU32(UBfe(a, 16u, 16u), UBfe(b, 16u, 16u));
  return accum + d0 + d1;
}

uint SadU32(uint a, uint b, uint accum) {
  return accum + AbsDiffU32(a, b);
}

uint MsadU8(uint source, uint reference, uint accum) {
  uint s0 = UBfe(source, 0u, 8u);
  uint s1 = UBfe(source, 8u, 8u);
  uint s2 = UBfe(source, 16u, 8u);
  uint s3 = UBfe(source, 24u, 8u);
  uint r0 = UBfe(reference, 0u, 8u);
  uint r1 = UBfe(reference, 8u, 8u);
  uint r2 = UBfe(reference, 16u, 8u);
  uint r3 = UBfe(reference, 24u, 8u);
  uint d0 = r0 != 0u ? AbsDiffU32(s0, r0) : 0u;
  uint d1 = r1 != 0u ? AbsDiffU32(s1, r1) : 0u;
  uint d2 = r2 != 0u ? AbsDiffU32(s2, r2) : 0u;
  uint d3 = r3 != 0u ? AbsDiffU32(s3, r3) : 0u;
  return accum + d0 + d1 + d2 + d3;
}

// Canonical recovery candidates for AMD 24-bit multiply. The native operations
// consume the low 24 bits of each input as unsigned or signed 24-bit integers.
// Keeping the range restriction explicit gives the Vulkan backend the proof it
// needs to select v_mul_{u32_u24,i32_i24} if its combiner recognizes the form.
uint MulU24(uint a, uint b) {
  return (a & 0x00ffffffu) * (b & 0x00ffffffu);
}
int MulI24(int a, int b) {
  int a24 = (a << 8) >> 8;
  int b24 = (b << 8) >> 8;
  return a24 * b24;
}

// Non-legacy VALU math. These spell the standard HLSL operation deliberately:
// Vulkan carries equivalent mathematical semantics and the AMD Vulkan backend
// remains free to select the native v_* operation. Native ISA recovery is
// checked separately; these are not substitutes for the legacy-behaviour AMD
// intrinsics (rcp_legacy, rsq_legacy, fma_legacy, fmul_legacy, etc.).
float Rcp(float value) { return rcp(value); }
float Sqrt(float value) { return sqrt(value); }
float Rsq(float value) { return rsqrt(value); }
float Sin(float value) { return sin(value); }
float Cos(float value) { return cos(value); }
float Log2(float value) { return log2(value); }
float Exp2(float value) { return exp2(value); }
float Fract(float value) { return frac(value); }

// AMD's v_med3 path has a registered SPIR-V AMD extended instruction. Keep the
// AMD identity instead of rebuilding it from min/max arithmetic.
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(7, "SPV_AMD_shader_trinary_minmax")]] float
FMed3(float x, float y, float z);

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_MATH_H_
