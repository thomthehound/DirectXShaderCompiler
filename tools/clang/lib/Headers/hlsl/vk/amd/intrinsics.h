// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_INTRINSICS_H_
#define _HLSL_VK_AMD_INTRINSICS_H_

#include <vk/spirv.h>

// Direct HLSL spellings for registered AMD extended-instruction sets and
// zero-overhead Vulkan equivalents for AMD's D3D shader-intrinsic surface.
// Do not emulate hardware-specific operations here when SPIR-V cannot carry
// equivalent semantics; those remain explicit contract gaps.
namespace vk {
namespace amd {

// Core/KHR subgroup equivalents for AMD's D3D wave intrinsics.
float ReadFirstLane(float value) { return WaveReadLaneFirst(value); }
uint ReadFirstLane(uint value) { return WaveReadLaneFirst(value); }
int ReadFirstLane(int value) { return WaveReadLaneFirst(value); }

float ReadLane(float value, uint lane) { return WaveReadLaneAt(value, lane); }
uint ReadLane(uint value, uint lane) { return WaveReadLaneAt(value, lane); }
int ReadLane(int value, uint lane) { return WaveReadLaneAt(value, lane); }

// Unlike the legacy D3D Readlane transport, ReadlaneAt permits a non-uniform
// lane index. DXC lowers WaveReadLaneAt to OpGroupNonUniformShuffle.
float ReadLaneAt(float value, uint lane) { return WaveReadLaneAt(value, lane); }
uint ReadLaneAt(uint value, uint lane) { return WaveReadLaneAt(value, lane); }
int ReadLaneAt(int value, uint lane) { return WaveReadLaneAt(value, lane); }

uint LaneId() { return WaveGetLaneIndex(); }
uint WaveSize() { return WaveGetLaneCount(); }

uint2 Ballot(bool predicate) {
  uint4 mask = WaveActiveBallot(predicate);
  return mask.xy;
}
bool BallotAny(bool predicate) { return WaveActiveAnyTrue(predicate); }
bool BallotAll(bool predicate) { return WaveActiveAllTrue(predicate); }

// Exact standard subgroup reductions. These exist on DXIL as Wave* operations
// and on Vulkan as OpGroupNonUniform* operations, so APUSR can use the same
// semantics on both backends without carrying shuffle trees solely for API
// portability. Clustered/postfix/min-max scan forms remain separately tracked
// because HLSL does not expose all of AMD's AGS WaveReduce/WaveScan surface.
float ActiveSum(float value) { return WaveActiveSum(value); }
int ActiveSum(int value) { return WaveActiveSum(value); }
uint ActiveSum(uint value) { return WaveActiveSum(value); }

float ActiveProduct(float value) { return WaveActiveProduct(value); }
int ActiveProduct(int value) { return WaveActiveProduct(value); }
uint ActiveProduct(uint value) { return WaveActiveProduct(value); }

float ActiveMin(float value) { return WaveActiveMin(value); }
int ActiveMin(int value) { return WaveActiveMin(value); }
uint ActiveMin(uint value) { return WaveActiveMin(value); }

float ActiveMax(float value) { return WaveActiveMax(value); }
int ActiveMax(int value) { return WaveActiveMax(value); }
uint ActiveMax(uint value) { return WaveActiveMax(value); }

int ActiveBitAnd(int value) { return WaveActiveBitAnd(value); }
uint ActiveBitAnd(uint value) { return WaveActiveBitAnd(value); }
int ActiveBitOr(int value) { return WaveActiveBitOr(value); }
uint ActiveBitOr(uint value) { return WaveActiveBitOr(value); }
int ActiveBitXor(int value) { return WaveActiveBitXor(value); }
uint ActiveBitXor(uint value) { return WaveActiveBitXor(value); }

float PrefixSum(float value) { return WavePrefixSum(value); }
int PrefixSum(int value) { return WavePrefixSum(value); }
uint PrefixSum(uint value) { return WavePrefixSum(value); }

float PrefixProduct(float value) { return WavePrefixProduct(value); }
int PrefixProduct(int value) { return WavePrefixProduct(value); }
uint PrefixProduct(uint value) { return WavePrefixProduct(value); }

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
[[vk::ext_instruction(4, "SPV_AMD_shader_ballot")]] uint Mbcnt(uint64_t mask);

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
