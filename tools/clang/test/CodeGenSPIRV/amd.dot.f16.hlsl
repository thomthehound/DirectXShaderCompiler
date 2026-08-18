// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv -enable-16bit-types %s | FileCheck %s

#include <vk/amd/dot_f16.h>

// Preserve native half inputs and distinct FP32-vs-FP16 accumulation graphs.
// Native v_dot2 selection is checked by the installed-driver probe.
// CHECK: OpTypeFloat 16
// CHECK: OpFConvert
// CHECK: OpFMul
// CHECK: OpFAdd

RWStructuredBuffer<uint2> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float16_t a0 = float16_t(float((tid.x & 7u) + 1u) * 0.125f);
  float16_t a1 = float16_t(float((tid.x & 15u) + 2u) * -0.0625f);
  float16_t b0 = float16_t(float((tid.x & 3u) + 3u) * 0.25f);
  float16_t b1 = float16_t(float((tid.x & 31u) + 1u) * 0.03125f);
  vector<float16_t, 2> a = vector<float16_t, 2>(a0, a1);
  vector<float16_t, 2> b = vector<float16_t, 2>(b0, b1);

  float f32Accum = float(tid.x) * 0.015625f;
  float16_t f16Accum = float16_t(float(tid.x & 7u) * 0.03125f);
  float f32Result = vk::amd::FDot2F32F16(a, b, f32Accum);
  float16_t f16Result = vk::amd::FDot2F16F16(a, b, f16Accum);

  Out[tid.x] = uint2(asuint(f32Result), f32tof16(float(f16Result)));
}
