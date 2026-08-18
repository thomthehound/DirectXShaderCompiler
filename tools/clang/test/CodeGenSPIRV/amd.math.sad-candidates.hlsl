// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv %s | FileCheck %s

#include <vk/amd/math.h>

// Structural contract for the canonical SAD graphs. Byte/word lanes must stay
// explicit and masked SAD must retain four independent reference-zero selects.
// This test does not claim native ISA; the installed-driver math probe does.
// CHECK: OpBitFieldUExtract
// CHECK-COUNT-4: OpSelect
// CHECK-COUNT-4: OpINotEqual
// CHECK-NOT: SAbs

RWStructuredBuffer<uint> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x10204081u + 0xf0806040u;
  uint b = tid.x * 0x0103070fu + 0x00112233u;
  uint accum = tid.x * 17u + 3u;

  uint r = vk::amd::SadU8(a, b, accum);
  r ^= vk::amd::SadHiU8(b, a, accum + 1u);
  r ^= vk::amd::SadU16(a, b, accum + 2u);
  r ^= vk::amd::SadU32(a, b, accum + 3u);
  r ^= vk::amd::MsadU8(a, b, accum + 4u);
  Out[tid.x] = r;
}
