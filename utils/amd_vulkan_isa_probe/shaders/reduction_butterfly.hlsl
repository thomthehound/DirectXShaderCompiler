#include <vk/amd/intrinsics.h>

RWStructuredBuffer<uint4> Out : register(u0);

uint ButterflySum(uint value) {
  value += vk::amd::LaneXor1(value);
  value += vk::amd::LaneXor2(value);
  value += vk::amd::LaneXor4(value);
  value += vk::amd::LaneXor8(value);
  value += vk::amd::LaneXor16(value);
  if (vk::amd::WaveSize() > 32u)
    value += vk::amd::LaneXor32(value);
  return value;
}

uint ButterflyMin(uint value) {
  value = min(value, vk::amd::LaneXor1(value));
  value = min(value, vk::amd::LaneXor2(value));
  value = min(value, vk::amd::LaneXor4(value));
  value = min(value, vk::amd::LaneXor8(value));
  value = min(value, vk::amd::LaneXor16(value));
  if (vk::amd::WaveSize() > 32u)
    value = min(value, vk::amd::LaneXor32(value));
  return value;
}

uint ButterflyMax(uint value) {
  value = max(value, vk::amd::LaneXor1(value));
  value = max(value, vk::amd::LaneXor2(value));
  value = max(value, vk::amd::LaneXor4(value));
  value = max(value, vk::amd::LaneXor8(value));
  value = max(value, vk::amd::LaneXor16(value));
  if (vk::amd::WaveSize() > 32u)
    value = max(value, vk::amd::LaneXor32(value));
  return value;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = tid.x * 17u + 3u;
  Out[tid.x] = uint4(ButterflySum(u), ButterflyMin(u), ButterflyMax(u),
                     vk::amd::WaveSize());
}
