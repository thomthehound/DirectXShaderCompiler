// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_CROSSLANE_H_
#define _HLSL_VK_AMD_CROSSLANE_H_

#include <vk/amd/intrinsics.h>
#include <vk/amd/subgroup.h>

namespace vk {
namespace amd {

// Canonical subgroup-shuffle contracts for DPP/permlane/DS recovery.
// Boundary behaviour is explicit: up/down reads keep the current lane when the
// requested neighbour is outside the wave. Row rotates are confined to 16-lane
// rows and wrap within the row. These are exact semantics, not native claims.
float LaneUpSelf(float value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint source = lane >= delta ? lane - delta : lane;
  return WaveReadLaneAt(value, source);
}
uint LaneUpSelf(uint value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint source = lane >= delta ? lane - delta : lane;
  return WaveReadLaneAt(value, source);
}
int LaneUpSelf(int value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint source = lane >= delta ? lane - delta : lane;
  return WaveReadLaneAt(value, source);
}

float LaneDownSelf(float value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint candidate = lane + delta;
  uint source = candidate < WaveGetLaneCount() ? candidate : lane;
  return WaveReadLaneAt(value, source);
}
uint LaneDownSelf(uint value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint candidate = lane + delta;
  uint source = candidate < WaveGetLaneCount() ? candidate : lane;
  return WaveReadLaneAt(value, source);
}
int LaneDownSelf(int value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint candidate = lane + delta;
  uint source = candidate < WaveGetLaneCount() ? candidate : lane;
  return WaveReadLaneAt(value, source);
}

// "Right" means values rotate toward increasing lane indices: output lane N
// reads lane N-delta. "Left" is the inverse. Both stay inside each 16-lane row.
float RowRotateRight16(float value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint rowBase = lane & ~15u;
  uint rowLane = lane & 15u;
  uint source = rowBase | ((rowLane + 16u - (delta & 15u)) & 15u);
  return WaveReadLaneAt(value, source);
}
uint RowRotateRight16(uint value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint rowBase = lane & ~15u;
  uint rowLane = lane & 15u;
  uint source = rowBase | ((rowLane + 16u - (delta & 15u)) & 15u);
  return WaveReadLaneAt(value, source);
}
int RowRotateRight16(int value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint rowBase = lane & ~15u;
  uint rowLane = lane & 15u;
  uint source = rowBase | ((rowLane + 16u - (delta & 15u)) & 15u);
  return WaveReadLaneAt(value, source);
}

float RowRotateLeft16(float value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint rowBase = lane & ~15u;
  uint rowLane = lane & 15u;
  uint source = rowBase | ((rowLane + (delta & 15u)) & 15u);
  return WaveReadLaneAt(value, source);
}
uint RowRotateLeft16(uint value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint rowBase = lane & ~15u;
  uint rowLane = lane & 15u;
  uint source = rowBase | ((rowLane + (delta & 15u)) & 15u);
  return WaveReadLaneAt(value, source);
}
int RowRotateLeft16(int value, uint delta) {
  uint lane = WaveGetLaneIndex();
  uint rowBase = lane & ~15u;
  uint rowLane = lane & 15u;
  uint source = rowBase | ((rowLane + (delta & 15u)) & 15u);
  return WaveReadLaneAt(value, source);
}

// Fixed spellings give Radeon compile-time routing controls to recognize.
float LaneUp1(float v) { return LaneUpSelf(v, 1u); }
float LaneUp2(float v) { return LaneUpSelf(v, 2u); }
float LaneUp4(float v) { return LaneUpSelf(v, 4u); }
float LaneDown1(float v) { return LaneDownSelf(v, 1u); }
float LaneDown2(float v) { return LaneDownSelf(v, 2u); }
float LaneDown4(float v) { return LaneDownSelf(v, 4u); }
uint LaneUp1(uint v) { return LaneUpSelf(v, 1u); }
uint LaneUp2(uint v) { return LaneUpSelf(v, 2u); }
uint LaneUp4(uint v) { return LaneUpSelf(v, 4u); }
uint LaneDown1(uint v) { return LaneDownSelf(v, 1u); }
uint LaneDown2(uint v) { return LaneDownSelf(v, 2u); }
uint LaneDown4(uint v) { return LaneDownSelf(v, 4u); }

float RowRotateRight1(float v) { return RowRotateRight16(v, 1u); }
float RowRotateRight4(float v) { return RowRotateRight16(v, 4u); }
float RowRotateLeft1(float v) { return RowRotateLeft16(v, 1u); }
float RowRotateLeft4(float v) { return RowRotateLeft16(v, 4u); }
uint RowRotateRight1(uint v) { return RowRotateRight16(v, 1u); }
uint RowRotateRight4(uint v) { return RowRotateRight16(v, 4u); }
uint RowRotateLeft1(uint v) { return RowRotateLeft16(v, 1u); }
uint RowRotateLeft4(uint v) { return RowRotateLeft16(v, 4u); }

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_CROSSLANE_H_
