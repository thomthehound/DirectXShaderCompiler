// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_INTRINSICS_H_
#define _HLSL_VK_AMD_INTRINSICS_H_

#include <vk/spirv.h>

// Direct HLSL spellings for the extended-instruction sets registered by AMD.
// These declarations do not emulate the operations. They deliberately preserve
// the SPV_AMD_* instruction-set identity in emitted SPIR-V.
namespace vk {
namespace amd {

// SPV_AMD_shader_trinary_minmax
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(1, "SPV_AMD_shader_trinary_minmax")]] T
FMin3(T x, T y, T z);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(2, "SPV_AMD_shader_trinary_minmax")]] T
UMin3(T x, T y, T z);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(3, "SPV_AMD_shader_trinary_minmax")]] T
SMin3(T x, T y, T z);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(4, "SPV_AMD_shader_trinary_minmax")]] T
FMax3(T x, T y, T z);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(5, "SPV_AMD_shader_trinary_minmax")]] T
UMax3(T x, T y, T z);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(6, "SPV_AMD_shader_trinary_minmax")]] T
SMax3(T x, T y, T z);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(7, "SPV_AMD_shader_trinary_minmax")]] T
FMid3(T x, T y, T z);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(8, "SPV_AMD_shader_trinary_minmax")]] T
UMid3(T x, T y, T z);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_trinary_minmax")]]
[[vk::ext_instruction(9, "SPV_AMD_shader_trinary_minmax")]] T
SMid3(T x, T y, T z);

// SPV_AMD_gcn_shader
[[vk::ext_extension("SPV_AMD_gcn_shader")]]
[[vk::ext_instruction(1, "SPV_AMD_gcn_shader")]] float
CubeFaceIndex(float3 p);
[[vk::ext_extension("SPV_AMD_gcn_shader")]]
[[vk::ext_instruction(2, "SPV_AMD_gcn_shader")]] float2
CubeFaceCoord(float3 p);
[[vk::ext_extension("SPV_AMD_gcn_shader")]]
[[vk::ext_instruction(3, "SPV_AMD_gcn_shader")]] uint64_t Time();

// SPV_AMD_shader_ballot
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_ballot")]]
[[vk::ext_instruction(1, "SPV_AMD_shader_ballot")]] T
SwizzleInvocations(T data, uint4 offset);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_ballot")]]
[[vk::ext_instruction(2, "SPV_AMD_shader_ballot")]] T
SwizzleInvocationsMasked(T data, uint3 mask);
template <typename T>
[[vk::ext_extension("SPV_AMD_shader_ballot")]]
[[vk::ext_instruction(3, "SPV_AMD_shader_ballot")]] T
WriteInvocation(T inputValue, T writeValue, uint invocationIndex);
[[vk::ext_extension("SPV_AMD_shader_ballot")]]
[[vk::ext_instruction(4, "SPV_AMD_shader_ballot")]] uint Mbcnt(uint mask);

// SPV_AMD_shader_explicit_vertex_parameter. The source operand must remain a
// pointer to fragment input storage, hence ext_reference.
template <typename T>
[[vk::ext_capability(52)]]
[[vk::ext_extension("SPV_AMD_shader_explicit_vertex_parameter")]]
[[vk::ext_instruction(1, "SPV_AMD_shader_explicit_vertex_parameter")]] T
InterpolateAtVertex([[vk::ext_reference]] T interpolant, uint vertexIndex);

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_INTRINSICS_H_
