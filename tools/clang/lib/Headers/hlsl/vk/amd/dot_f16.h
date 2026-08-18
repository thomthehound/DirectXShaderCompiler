// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_DOT_F16_H_
#define _HLSL_VK_AMD_DOT_F16_H_

// This header intentionally requires DXC native 16-bit types. Keep it separate
// from vk/amd/dot.h so integer-only shaders do not acquire a 16-bit-type build
// requirement merely by including the AMD integer surface.
namespace vk {
namespace amd {

// Exact non-clamped llvm.amdgcn.fdot2 source semantics:
//   <2 x half> a, <2 x half> b, float accumulator -> float
// Widen each half operand before multiplication so this is not accidentally a
// half-precision dot followed by a float conversion. Radeon ISA decides whether
// the canonical graph is recovered as v_dot2_f32_f16 / v_dot2acc_f32_f16.
float FDot2F32F16(vector<float16_t, 2> a,
                  vector<float16_t, 2> b,
                  float accum) {
  float a0 = float(a.x);
  float a1 = float(a.y);
  float b0 = float(b.x);
  float b1 = float(b.y);
  return accum + a0 * b0 + a1 * b1;
}

// A packed-half-result form is useful for kernels which intentionally retain a
// half accumulator. It is a separate contract from FDot2F32F16 and must not be
// used as a substitute for FP32 accumulation.
float16_t FDot2F16F16(vector<float16_t, 2> a,
                      vector<float16_t, 2> b,
                      float16_t accum) {
  return accum + a.x * b.x + a.y * b.y;
}

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_DOT_F16_H_
