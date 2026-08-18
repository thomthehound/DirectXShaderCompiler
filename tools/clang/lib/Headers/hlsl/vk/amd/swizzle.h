// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_SWIZZLE_H_
#define _HLSL_VK_AMD_SWIZZLE_H_

#include <vk/amd/intrinsics.h>

namespace vk {
namespace amd {

// AGS packs the three 5-bit masked-swizzle controls into its immediate as
//   bits  0..4  = AND mask
//   bits  5..9  = OR mask
//   bits 10..14 = XOR mask
// which is exactly the mask consumed by SPV_AMD_shader_ballot's
// SwizzleInvocationsMaskedAMD:
//   j = (((lane & 0x1f) & andMask) | orMask) ^ xorMask.
// Keep the AGS operation a non-type template argument so the generated mask is
// a constant, as required by the SPIR-V extension and by the AGS source API.
template <uint operation>
float Swizzle(float value) {
  const uint3 mask = uint3(operation & 0x1fu,
                           (operation >> 5u) & 0x1fu,
                           (operation >> 10u) & 0x1fu);
  return SwizzleInvocationsMasked(value, mask);
}

template <uint operation>
uint Swizzle(uint value) {
  const uint3 mask = uint3(operation & 0x1fu,
                           (operation >> 5u) & 0x1fu,
                           (operation >> 10u) & 0x1fu);
  return SwizzleInvocationsMasked(value, mask);
}

template <uint operation>
int Swizzle(int value) {
  return asint(Swizzle<operation>(asuint(value)));
}

// Current AGS named controls, retained here so users need not copy magic
// immediates from the AGS header when writing a Vulkan-equivalent shader.
static const uint SwizzleSwapX1 = 0x041fu;
static const uint SwizzleSwapX2 = 0x081fu;
static const uint SwizzleSwapX4 = 0x101fu;
static const uint SwizzleSwapX8 = 0x201fu;
static const uint SwizzleSwapX16 = 0x401fu;
static const uint SwizzleReverseX2 = 0x041fu;
static const uint SwizzleReverseX4 = 0x0c1fu;
static const uint SwizzleReverseX8 = 0x1c1fu;
static const uint SwizzleReverseX16 = 0x3c1fu;
static const uint SwizzleReverseX32 = 0x7c1fu;
static const uint SwizzleBCastX2 = 0x003eu;
static const uint SwizzleBCastX4 = 0x003cu;
static const uint SwizzleBCastX8 = 0x0038u;
static const uint SwizzleBCastX16 = 0x0030u;
static const uint SwizzleBCastX32 = 0x0020u;

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_SWIZZLE_H_
