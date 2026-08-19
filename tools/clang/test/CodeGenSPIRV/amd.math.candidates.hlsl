// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv %s | FileCheck %s

#include <vk/amd/math.h>

// Canonical-recovery tests intentionally validate semantic structure rather
// than claiming native ISA. Unsigned MUL24 must explicitly discard upper input
// bits; signed MUL24 must explicitly sign-extend bit 23 before multiplication.
// CHECK-COUNT-2: OpIMul
// CHECK: OpBitwiseAnd
// CHECK: OpShiftLeftLogical
// CHECK: OpShiftRightArithmetic

RWStructuredBuffer<uint> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  // Keep both operands dynamic without adding unrelated multiplies to the
  // SPIR-V shape being counted below.
  uint a = (tid.x ^ 0x13579bdu) + 0x12fedcbau;
  uint b = ((tid.x << 7u) ^ 0x2468aceu) + 0xe156789au;
  uint u = vk::amd::MulU24(a, b);
  int s = vk::amd::MulI24(asint(a), asint(b));
  Out[tid.x] = u ^ asuint(s);
}
