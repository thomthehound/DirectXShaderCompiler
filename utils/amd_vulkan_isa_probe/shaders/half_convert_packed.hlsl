#include <vk/amd/math.h>

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float x = 0.5f + float(tid.x & 63u) * (1.0f / 64.0f);
  float y = x * 1.375f - 0.25f;
  uint packed = vk::amd::PackF16x2(float2(x, y));
  float2 unpacked = vk::amd::UnpackF16x2(packed);
  Out[tid.x] = uint4(packed, asuint(unpacked.x), asuint(unpacked.y),
                     asuint(unpacked.x + unpacked.y));
}
