#include <vk/amd/intrinsics.h>

RWStructuredBuffer<uint4> Out : register(u0);

// Fixed butterfly stages mirror the generated APUSR FSR4 reduction trees.
// Pipeline creation is enough for installed-driver ISA inspection.
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float f = float(tid.x) * 0.125f + 0.25f;
  uint u = tid.x * 0x9e3779b9u + 3u;

  float a = vk::amd::LaneXor1(f) + vk::amd::LaneXor2(f) + vk::amd::LaneXor4(f);
  float b = vk::amd::LaneXor8(f) + vk::amd::LaneXor16(f);
  uint c = vk::amd::LaneXor1(u) ^ vk::amd::LaneXor2(u) ^ vk::amd::LaneXor4(u);
  uint d = vk::amd::LaneXor8(u) ^ vk::amd::LaneXor16(u);

  // XOR 32 is only a valid in-wave butterfly stage for wave64. Keep the
  // condition uniform so wave32 qualification never executes an invalid read.
  if (vk::amd::WaveSize() > 32u) {
    b += vk::amd::LaneXor32(f);
    d ^= vk::amd::LaneXor32(u);
  }

  Out[tid.x] = uint4(asuint(a), asuint(b), c, d);
}
