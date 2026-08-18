// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv %s | FileCheck %s

#include <vk/amd/math.h>

// This test protects semantic identity at the SPIR-V boundary. It deliberately
// does not require Radeon-native mnemonics; math_probe.py owns that proof.
// CHECK: OpExtInstImport "GLSL.std.450"
// CHECK: OpExtInst {{.*}} FindILsb
// CHECK: OpExtInst {{.*}} FindUMsb
// CHECK: OpExtInst {{.*}} FindSMsb
// CHECK: OpExtInst {{.*}} PackHalf2x16
// CHECK: OpExtInst {{.*}} UnpackHalf2x16
// CHECK: OpExtInst {{.*}} Log
// CHECK: OpExtInst {{.*}} Exp
// CHECK: OpExtInst {{.*}} Tanh
// CHECK: OpExtInst {{.*}} Pow
// CHECK: OpExtInst {{.*}} FrexpStruct
// CHECK: OpExtInst {{.*}} Exp2
// CHECK: OpFMul

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = tid.x * 0x9e3779b9u + 0x13579bdfu;
  int s = asint(u ^ 0x81234567u);
  float x = 0.625f + float(u & 511u) * (1.0f / 1024.0f);

  int low = vk::amd::FirstBitLow(u | 1u);
  int highU = vk::amd::FirstBitHigh(u | 1u);
  int highS = vk::amd::FirstBitHigh(s | 1);

  float y = vk::amd::Log(x + 1.0f) + vk::amd::Exp(x * 0.125f);
  y += vk::amd::Tanh(x - 0.75f);
  y += vk::amd::Pow(x + 0.5f, 1.25f + float(tid.x & 3u) * 0.125f);

  float exponent;
  float mantissa = vk::amd::Frexp(x, exponent);
  y += vk::amd::Ldexp(mantissa, exponent);

  uint packed = vk::amd::PackF16x2(float2(x, y));
  float2 unpacked = vk::amd::UnpackF16x2(packed);

  Out[tid.x] = uint4(asuint(low), asuint(highU) ^ asuint(highS), packed,
                     asuint(y + unpacked.x + unpacked.y));
}
