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

// Exact XOR-butterfly exchange used by APUSR's generated FSR4 FP16 reduction
// trees. The source lane is current_lane XOR mask. This is intentionally kept
// as a subgroup shuffle contract: Radeon may select DPP/permlane/DS exchange,
// but we do not claim one until installed-driver ISA proves the choice.
float LaneXor(float value, uint mask) {
  return WaveReadLaneAt(value, WaveGetLaneIndex() ^ mask);
}
uint LaneXor(uint value, uint mask) {
  return WaveReadLaneAt(value, WaveGetLaneIndex() ^ mask);
}
int LaneXor(int value, uint mask) {
  return WaveReadLaneAt(value, WaveGetLaneIndex() ^ mask);
}

// Fixed masks match the butterfly stages emitted by the imported FSR4 kernels
// and give the backend compile-time lane-routing information that DPP/permlane
// selection normally needs. Mask 32 is meaningful for wave64 and is still a
// valid subgroup shuffle expression on wave32 only when the caller does not
// execute that stage.
float LaneXor1(float v) { return LaneXor(v, 1u); }
float LaneXor2(float v) { return LaneXor(v, 2u); }
float LaneXor4(float v) { return LaneXor(v, 4u); }
float LaneXor8(float v) { return LaneXor(v, 8u); }
float LaneXor16(float v) { return LaneXor(v, 16u); }
float LaneXor32(float v) { return LaneXor(v, 32u); }
uint LaneXor1(uint v) { return LaneXor(v, 1u); }
uint LaneXor2(uint v) { return LaneXor(v, 2u); }
uint LaneXor4(uint v) { return LaneXor(v, 4u); }
uint LaneXor8(uint v) { return LaneXor(v, 8u); }
uint LaneXor16(uint v) { return LaneXor(v, 16u); }
uint LaneXor32(uint v) { return LaneXor(v, 32u); }

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

int ActiveBitAnd(int value) { return asint(WaveActiveBitAnd(asuint(value))); }
uint ActiveBitAnd(uint value) { return WaveActiveBitAnd(value); }
int ActiveBitOr(int value) { return asint(WaveActiveBitOr(asuint(value))); }
uint ActiveBitOr(uint value) { return WaveActiveBitOr(value); }
int ActiveBitXor(int value) { return asint(WaveActiveBitXor(asuint(value))); }
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

// Raw AGS signature-register forms are compiler intrinsics because their
// indices must remain immediate values through AST emission:
//   vk::AmdVertexParameter(vertexIdx, parameterIdx)
//   vk::AmdVertexParameterComponent(vertexIdx, parameterIdx, componentIdx)
// They resolve D3D parameter-register packing independently of Vulkan
// Location and lower to the same InterpolateAtVertexAMD instruction below.

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
