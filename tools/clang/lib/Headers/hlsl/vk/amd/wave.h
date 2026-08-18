// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_WAVE_H_
#define _HLSL_VK_AMD_WAVE_H_

#include <vk/spirv.h>

namespace vk {
namespace amd {

// Core SPIR-V subgroup arithmetic. Group Operation is an instruction literal;
// Execution is the Subgroup scope id. These raw declarations preserve AGS scan
// mode directly instead of reconstructing min/max scans with lane shuffles.
template <typename T>
[[vk::ext_capability(63)]] // GroupNonUniformArithmetic
[[vk::ext_instruction(/* OpGroupNonUniformIAdd */ 349)]] T
GroupIAdd(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformFAdd */ 350)]] T
GroupFAdd(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformIMul */ 351)]] T
GroupIMul(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformFMul */ 352)]] T
GroupFMul(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformSMin */ 353)]] T
GroupSMin(vk::Scope execution, [[vk::ext_literal]] int operation, T value);
template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformUMin */ 354)]] T
GroupUMin(vk::Scope execution, [[vk::ext_literal]] int operation, T value);
template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformFMin */ 355)]] T
GroupFMin(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformSMax */ 356)]] T
GroupSMax(vk::Scope execution, [[vk::ext_literal]] int operation, T value);
template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformUMax */ 357)]] T
GroupUMax(vk::Scope execution, [[vk::ext_literal]] int operation, T value);
template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformFMax */ 358)]] T
GroupFMax(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformBitwiseAnd */ 359)]] T
GroupBitAnd(vk::Scope execution, [[vk::ext_literal]] int operation, T value);
template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformBitwiseOr */ 360)]] T
GroupBitOr(vk::Scope execution, [[vk::ext_literal]] int operation, T value);
template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformBitwiseXor */ 361)]] T
GroupBitXor(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

// SPIR-V Group Operation literals: Reduce=0, InclusiveScan=1, ExclusiveScan=2.
// Generate the complete scalar/vector AGS surface. All operations are component-
// wise across the wave, matching AGS floatN/intN/uintN overload semantics.
#define VK_AMD_FLOAT_SCAN_OVERLOADS(T)                                         \
  T InclusiveSum(T v) { return GroupFAdd(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveSum(T v) { return GroupFAdd(vk::ScopeSubgroup, 2, v); }          \
  T InclusiveProduct(T v) { return GroupFMul(vk::ScopeSubgroup, 1, v); }      \
  T ExclusiveProduct(T v) { return GroupFMul(vk::ScopeSubgroup, 2, v); }      \
  T InclusiveMin(T v) { return GroupFMin(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveMin(T v) { return GroupFMin(vk::ScopeSubgroup, 2, v); }          \
  T InclusiveMax(T v) { return GroupFMax(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveMax(T v) { return GroupFMax(vk::ScopeSubgroup, 2, v); }

#define VK_AMD_SIGNED_SCAN_OVERLOADS(T)                                        \
  T InclusiveSum(T v) { return GroupIAdd(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveSum(T v) { return GroupIAdd(vk::ScopeSubgroup, 2, v); }          \
  T InclusiveProduct(T v) { return GroupIMul(vk::ScopeSubgroup, 1, v); }      \
  T ExclusiveProduct(T v) { return GroupIMul(vk::ScopeSubgroup, 2, v); }      \
  T InclusiveMin(T v) { return GroupSMin(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveMin(T v) { return GroupSMin(vk::ScopeSubgroup, 2, v); }          \
  T InclusiveMax(T v) { return GroupSMax(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveMax(T v) { return GroupSMax(vk::ScopeSubgroup, 2, v); }          \
  T ReduceBitAnd(T v) { return GroupBitAnd(vk::ScopeSubgroup, 0, v); }        \
  T ReduceBitOr(T v) { return GroupBitOr(vk::ScopeSubgroup, 0, v); }          \
  T ReduceBitXor(T v) { return GroupBitXor(vk::ScopeSubgroup, 0, v); }

#define VK_AMD_UNSIGNED_SCAN_OVERLOADS(T)                                      \
  T InclusiveSum(T v) { return GroupIAdd(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveSum(T v) { return GroupIAdd(vk::ScopeSubgroup, 2, v); }          \
  T InclusiveProduct(T v) { return GroupIMul(vk::ScopeSubgroup, 1, v); }      \
  T ExclusiveProduct(T v) { return GroupIMul(vk::ScopeSubgroup, 2, v); }      \
  T InclusiveMin(T v) { return GroupUMin(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveMin(T v) { return GroupUMin(vk::ScopeSubgroup, 2, v); }          \
  T InclusiveMax(T v) { return GroupUMax(vk::ScopeSubgroup, 1, v); }          \
  T ExclusiveMax(T v) { return GroupUMax(vk::ScopeSubgroup, 2, v); }          \
  T ReduceBitAnd(T v) { return GroupBitAnd(vk::ScopeSubgroup, 0, v); }        \
  T ReduceBitOr(T v) { return GroupBitOr(vk::ScopeSubgroup, 0, v); }          \
  T ReduceBitXor(T v) { return GroupBitXor(vk::ScopeSubgroup, 0, v); }

VK_AMD_FLOAT_SCAN_OVERLOADS(float)
VK_AMD_FLOAT_SCAN_OVERLOADS(float2)
VK_AMD_FLOAT_SCAN_OVERLOADS(float3)
VK_AMD_FLOAT_SCAN_OVERLOADS(float4)
VK_AMD_SIGNED_SCAN_OVERLOADS(int)
VK_AMD_SIGNED_SCAN_OVERLOADS(int2)
VK_AMD_SIGNED_SCAN_OVERLOADS(int3)
VK_AMD_SIGNED_SCAN_OVERLOADS(int4)
VK_AMD_UNSIGNED_SCAN_OVERLOADS(uint)
VK_AMD_UNSIGNED_SCAN_OVERLOADS(uint2)
VK_AMD_UNSIGNED_SCAN_OVERLOADS(uint3)
VK_AMD_UNSIGNED_SCAN_OVERLOADS(uint4)

#undef VK_AMD_FLOAT_SCAN_OVERLOADS
#undef VK_AMD_SIGNED_SCAN_OVERLOADS
#undef VK_AMD_UNSIGNED_SCAN_OVERLOADS

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_WAVE_H_
