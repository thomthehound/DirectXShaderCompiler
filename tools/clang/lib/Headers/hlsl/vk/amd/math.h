// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_MATH_H_
#define _HLSL_VK_AMD_MATH_H_

#include <vk/spirv.h>

// AMD-oriented low-level math surface for Vulkan.
//
// Every function in this header is either a direct SPIR-V operation or an HLSL
// operation with equivalent mathematical semantics. Hardware-specific AMD
// operations whose semantics cannot be carried directly through SPIR-V are not
// emulated here: they stay in the native-recovery probe until ISA evidence
// justifies promoting a canonical representation.
namespace vk {
namespace amd {

// Direct core SPIR-V integer/bit contracts. These preserve the operation as a
// single semantic instruction at the DXC -> SPIR-V boundary.
[[vk::ext_instruction(/* OpBitFieldUExtract */ 203)]] uint
UBfe(uint base, uint offset, uint count);

[[vk::ext_instruction(/* OpBitFieldSExtract */ 202)]] int
SBfe(int base, uint offset, uint count);

[[vk::ext_instruction(/* OpBitReverse */ 204)]] uint BitReverse(uint value);
[[vk::ext_instruction(/* OpBitCount */ 205)]] uint BitCount(uint value);

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
