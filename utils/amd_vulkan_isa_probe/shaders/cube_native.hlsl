#include <vk/amd/intrinsics.h>

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float x = float(int(tid.x & 15u) - 7) * 0.375f + 0.125f;
  float y = float(int((tid.x >> 2u) & 15u) - 7) * 0.3125f - 0.25f;
  float z = float(int((tid.x * 5u) & 15u) - 7) * 0.4375f + 0.5f;
  float3 p = float3(x, y, z);

  float face = vk::amd::CubeFaceIndex(p);
  float2 coord = vk::amd::CubeFaceCoord(p);
  Out[tid.x] = uint4(asuint(face), asuint(coord.x), asuint(coord.y),
                     asuint(face + coord.x + coord.y));
}
