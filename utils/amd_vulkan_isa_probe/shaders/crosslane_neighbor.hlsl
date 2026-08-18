#include <vk/amd/crosslane.h>

RWStructuredBuffer<uint4> Out : register(u0);

// Fixed neighbour and 16-lane row routes give the Radeon backend constant
// patterns it can lower to DPP/permlane/DS routing when profitable.
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = tid.x * 0x9e3779b9u + 7u;
  float f = float(tid.x) * 0.25f + 0.5f;

  uint a = vk::amd::LaneUp1(u) ^ vk::amd::LaneUp2(u) ^ vk::amd::LaneUp4(u);
  uint b = vk::amd::LaneDown1(u) ^ vk::amd::LaneDown2(u) ^ vk::amd::LaneDown4(u);
  uint c = vk::amd::RowRotateRight1(u) ^ vk::amd::RowRotateRight4(u);
  float d = vk::amd::RowRotateLeft1(f) + vk::amd::RowRotateLeft4(f);

  Out[tid.x] = uint4(a, b, c, asuint(d));
}
