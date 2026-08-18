#include <vk/amd/math.h>

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float x = 0.5f + float(tid.x & 63u) * (1.0f / 64.0f);
  float y = x * 1.375f - 0.25f;
  uint hx = vk::amd::F32ToF16Bits(x);
  uint hy = vk::amd::F32ToF16Bits(y);
  float rx = vk::amd::F16BitsToF32(hx);
  float ry = vk::amd::F16BitsToF32(hy);
  Out[tid.x] = uint4(hx, hy, asuint(rx), asuint(ry));
}
