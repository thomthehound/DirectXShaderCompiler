// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_DX_AMD_INTRINSICS_H_
#define _HLSL_DX_AMD_INTRINSICS_H_

// Zero-overhead HLSL/DXIL spellings for the AMD-oriented wave operations used
// by APUSR. Keep these source-semantics identical to the vk::amd surface so the
// product shader can select a backend namespace without changing the algorithm.
namespace dx {
namespace amd {

float ReadFirstLane(float value) { return WaveReadLaneFirst(value); }
uint ReadFirstLane(uint value) { return WaveReadLaneFirst(value); }
int ReadFirstLane(int value) { return WaveReadLaneFirst(value); }

float ReadLaneAt(float value, uint lane) { return WaveReadLaneAt(value, lane); }
uint ReadLaneAt(uint value, uint lane) { return WaveReadLaneAt(value, lane); }
int ReadLaneAt(int value, uint lane) { return WaveReadLaneAt(value, lane); }

float XorLane(float value, uint laneMask) {
  return WaveReadLaneAt(value, WaveGetLaneIndex() ^ laneMask);
}
uint XorLane(uint value, uint laneMask) {
  return WaveReadLaneAt(value, WaveGetLaneIndex() ^ laneMask);
}
int XorLane(int value, uint laneMask) {
  return WaveReadLaneAt(value, WaveGetLaneIndex() ^ laneMask);
}

uint LaneId() { return WaveGetLaneIndex(); }
uint WaveSize() { return WaveGetLaneCount(); }

uint4 Ballot(bool predicate) { return WaveActiveBallot(predicate); }
bool BallotAny(bool predicate) { return WaveActiveAnyTrue(predicate); }
bool BallotAll(bool predicate) { return WaveActiveAllTrue(predicate); }

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

} // namespace amd
} // namespace dx

#endif // _HLSL_DX_AMD_INTRINSICS_H_
