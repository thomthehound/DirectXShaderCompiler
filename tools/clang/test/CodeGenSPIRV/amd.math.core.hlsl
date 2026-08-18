// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

#include <vk/amd/math.h>

// Verify low-level integer operations remain semantic SPIR-V instructions
// rather than expanding into shift/mask arithmetic.
// CHECK: OpBitFieldUExtract
// CHECK: OpBitFieldSExtract
// CHECK: OpBitReverse
// CHECK: OpBitCount
// CHECK: OpExtInst %float {{%[0-9]+}} FMid3AMD
// CHECK-NOT: OpShiftRightLogical
// CHECK-NOT: OpBitwiseAnd

RWStructuredBuffer<uint> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = vk::amd::UBfe(0xf0e1d2c3u, 7u, 13u);
  int s = vk::amd::SBfe(asint(0x8f31c007u), 5u, 11u);
  uint r = vk::amd::BitReverse(0x01234567u);
  uint c = vk::amd::BitCount(0xf0f00f0fu);
  float m = vk::amd::FMed3(-4.0f, 9.0f, 1.5f);

  float x = float((u ^ r ^ c) + asuint(s)) * 0.000001f + m;
  float y = vk::amd::Rcp(x + 3.0f) + vk::amd::Sqrt(abs(x) + 1.0f);
  y += vk::amd::Rsq(abs(x) + 2.0f);
  y += vk::amd::Sin(x) + vk::amd::Cos(x);
  y += vk::amd::Log2(abs(x) + 1.0f) + vk::amd::Exp2(frac(x));
  y += vk::amd::Fract(x);
  Out[0] = asuint(y);
}
