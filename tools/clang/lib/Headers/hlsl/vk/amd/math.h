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
