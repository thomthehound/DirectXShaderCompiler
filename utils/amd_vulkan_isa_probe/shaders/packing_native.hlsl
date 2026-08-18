#include <vk/amd/packing.h>

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float a = float(int(tid.x & 31u) - 15) * (1.0f / 16.0f);
  float b = float(int((tid.x * 7u) & 31u) - 15) * (1.0f / 16.0f);
  float2 sn = float2(a, b);
  float2 un = sn * 0.5f + 0.5f;
  float4 un4 = float4(un, frac(un.x * 0.375f + 0.125f),
                     frac(un.y * 0.625f + 0.25f));

  int2 si = int2(int(tid.x) * 4097 - 70000,
                 int(tid.x) * -8191 + 90000);
  uint2 ui = uint2(tid.x * 4097u, tid.x * 8191u + 60000u);

  uint pSn16 = vk::amd::PackSnorm2x16(sn);
  uint pUn16 = vk::amd::PackUnorm2x16(un);
  uint pI16 = vk::amd::PackI16x2(si);
  uint pU16 = vk::amd::PackU16x2(ui);
  uint pU8 = vk::amd::PackUnorm4x8(un4);

  Out[tid.x] = uint4(pSn16, pUn16, pI16 ^ pU16, pU8);
}
