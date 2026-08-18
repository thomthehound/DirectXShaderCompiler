// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv %s | FileCheck %s

#include <vk/amd/math.h>

// Keep the packed and fused integer candidates recognizable at the SPIR-V
// boundary. This is a shape test, not an ISA claim; Radeon recovery is checked
// by math_probe.py.
// CHECK: OpUMulExtended
// CHECK: OpSMulExtended
// CHECK: OpBitFieldUExtract
// CHECK: OpBitwiseAnd
// CHECK: OpBitwiseOr
// CHECK: OpBitwiseXor
// CHECK: OpNot
// CHECK: OpShiftRightLogical
// CHECK: OpShiftLeftLogical
// CHECK: OpIAdd
// CHECK: OpIMul

RWStructuredBuffer<uint> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x45d9f3bu + 0x13579bdfu;
  uint b = tid.x * 0x27d4eb2du + 0x2468ace1u;
  uint c = tid.x * 17u + 3u;

  uint r = vk::amd::MulHiU24(a, b);
  r ^= asuint(vk::amd::MulHiI24(asint(a), asint(b)));
  r ^= vk::amd::MadU24(a, b, c);
  r ^= asuint(vk::amd::MadI24(asint(a), asint(b), asint(c)));
  r ^= vk::amd::LerpU8(a, b, 0x01000100u ^ c);
  r ^= vk::amd::Bfi(a, b, c);
  r ^= vk::amd::XadU32(a, b, c);
  r ^= vk::amd::LshlAddU32(a, b, c);
  r ^= vk::amd::AddLshlU32(a, b, c);
  r ^= vk::amd::Add3U32(a, b, c);
  r ^= vk::amd::LshlOrB32(a, b, c);
  r ^= vk::amd::AndOrB32(a, b, c);
  r ^= vk::amd::Or3B32(a, b, c);
  Out[tid.x] = r;
}
