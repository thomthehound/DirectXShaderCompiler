#include <vk/amd/dot_f16.h>

RWStructuredBuffer<uint2> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  vector<float16_t, 2> a = vector<float16_t, 2>(
      float16_t(float((tid.x & 7u) + 1u) * 0.125f),
      float16_t(float((tid.x & 15u) + 2u) * -0.0625f));
  vector<float16_t, 2> b = vector<float16_t, 2>(
      float16_t(float((tid.x & 3u) + 3u) * 0.25f),
      float16_t(float((tid.x & 31u) + 1u) * 0.03125f));

  float f32 = vk::amd::FDot2F32F16(a, b, float(tid.x) * 0.015625f);
  float16_t f16 = vk::amd::FDot2F16F16(a, b, float16_t(0.125f));
  Out[tid.x] = uint2(asuint(f32), f32tof16(float(f16)));
}
