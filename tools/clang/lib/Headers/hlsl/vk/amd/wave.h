// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_WAVE_H_
#define _HLSL_VK_AMD_WAVE_H_

#include <vk/spirv.h>

namespace vk {
namespace amd {

static const int GroupOperationReduce = 0;
static const int GroupOperationInclusiveScan = 1;
static const int GroupOperationExclusiveScan = 2;

// Core SPIR-V subgroup arithmetic. Group Operation is an instruction literal;
// Execution is the Subgroup scope id. These raw declarations preserve AGS scan
// mode directly instead of reconstructing min/max scans with lane shuffles.
template <typename T>
[[vk::ext_capability(63)]] // GroupNonUniformArithmetic
[[vk::ext_instruction(/* OpGroupNonUniformIAdd */ 349)]] T
GroupIAdd(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformFAdd */ 350)]] float
GroupFAdd(vk::Scope execution, [[vk::ext_literal]] int operation, float value);

template <typename T>
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformIMul */ 351)]] T
GroupIMul(vk::Scope execution, [[vk::ext_literal]] int operation, T value);

[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformFMul */ 352)]] float
GroupFMul(vk::Scope execution, [[vk::ext_literal]] int operation, float value);

[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformSMin */ 353)]] int
GroupSMin(vk::Scope execution, [[vk::ext_literal]] int operation, int value);
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformUMin */ 354)]] uint
GroupUMin(vk::Scope execution, [[vk::ext_literal]] int operation, uint value);
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformFMin */ 355)]] float
GroupFMin(vk::Scope execution, [[vk::ext_literal]] int operation, float value);

[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformSMax */ 356)]] int
GroupSMax(vk::Scope execution, [[vk::ext_literal]] int operation, int value);
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformUMax */ 357)]] uint
GroupUMax(vk::Scope execution, [[vk::ext_literal]] int operation, uint value);
[[vk::ext_capability(63)]]
[[vk::ext_instruction(/* OpGroupNonUniformFMax */ 358)]] float
GroupFMax(vk::Scope execution, [[vk::ext_literal]] int operation, float value);

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

float InclusiveSum(float v) { return GroupFAdd(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
int InclusiveSum(int v) { return GroupIAdd(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
uint InclusiveSum(uint v) { return GroupIAdd(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
float ExclusiveSum(float v) { return GroupFAdd(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }
int ExclusiveSum(int v) { return GroupIAdd(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }
uint ExclusiveSum(uint v) { return GroupIAdd(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }

float InclusiveProduct(float v) { return GroupFMul(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
int InclusiveProduct(int v) { return GroupIMul(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
uint InclusiveProduct(uint v) { return GroupIMul(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
float ExclusiveProduct(float v) { return GroupFMul(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }
int ExclusiveProduct(int v) { return GroupIMul(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }
uint ExclusiveProduct(uint v) { return GroupIMul(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }

float InclusiveMin(float v) { return GroupFMin(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
int InclusiveMin(int v) { return GroupSMin(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
uint InclusiveMin(uint v) { return GroupUMin(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
float ExclusiveMin(float v) { return GroupFMin(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }
int ExclusiveMin(int v) { return GroupSMin(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }
uint ExclusiveMin(uint v) { return GroupUMin(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }

float InclusiveMax(float v) { return GroupFMax(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
int InclusiveMax(int v) { return GroupSMax(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
uint InclusiveMax(uint v) { return GroupUMax(vk::ScopeSubgroup, GroupOperationInclusiveScan, v); }
float ExclusiveMax(float v) { return GroupFMax(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }
int ExclusiveMax(int v) { return GroupSMax(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }
uint ExclusiveMax(uint v) { return GroupUMax(vk::ScopeSubgroup, GroupOperationExclusiveScan, v); }

// AGS permits these three only as reductions, not scans.
int ReduceBitAnd(int v) { return GroupBitAnd(vk::ScopeSubgroup, GroupOperationReduce, v); }
uint ReduceBitAnd(uint v) { return GroupBitAnd(vk::ScopeSubgroup, GroupOperationReduce, v); }
int ReduceBitOr(int v) { return GroupBitOr(vk::ScopeSubgroup, GroupOperationReduce, v); }
uint ReduceBitOr(uint v) { return GroupBitOr(vk::ScopeSubgroup, GroupOperationReduce, v); }
int ReduceBitXor(int v) { return GroupBitXor(vk::ScopeSubgroup, GroupOperationReduce, v); }
uint ReduceBitXor(uint v) { return GroupBitXor(vk::ScopeSubgroup, GroupOperationReduce, v); }

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_WAVE_H_
