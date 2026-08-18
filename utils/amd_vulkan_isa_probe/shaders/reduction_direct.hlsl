#include <vk/amd/intrinsics.h>

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = tid.x * 17u + 3u;
  uint sum = vk::amd::ActiveSum(u);
  uint lo = vk::amd::ActiveMin(u);
  uint hi = vk::amd::ActiveMax(u);
  Out[tid.x] = uint4(sum, lo, hi, vk::amd::WaveSize());
}
