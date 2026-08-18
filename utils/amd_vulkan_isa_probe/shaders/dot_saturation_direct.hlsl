#include <vk/amd/dot.h>

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x45d9f3bu + 0x89abcdefu;
  uint b = tid.x * 0x27d4eb2du + 0x13579bdfu;
  int sa = (tid.x & 1u) != 0u ? 0x7ffffff0 : asint(0x80000010u);
  uint ua = 0xfffffff0u - (tid.x & 7u);

  int ss = vk::amd::SDot4I8AccSat(a ^ 0x80808080u, b, sa);
  uint uu = vk::amd::UDot4U8AccSat(a, b ^ 0xffffffffu, ua);
  int su = vk::amd::SUDot4I8U8AccSat(a ^ 0x80808080u, b, sa);
  int us = vk::amd::USDot4U8I8AccSat(a, b ^ 0x80808080u, sa);
  Out[tid.x] = uint4(asuint(ss), uu, asuint(su), asuint(us));
}
