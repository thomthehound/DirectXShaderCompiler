// RUN: %dxc -T cs_6_4 -E main -fcgl -spirv %s | FileCheck %s

#include <vk/amd/dot.h>

// The 4x8 same-signedness forms must use the standardized packed-dot contract.
// The 2x16, mixed-signedness, 8x4, and packed-half FP16 forms are canonical
// recovery candidates. I4 extraction must be fully unrolled so the Radeon
// combiner sees a fixed packed-lane graph rather than a dynamic loop.
// CHECK: OpCapability DotProduct
// CHECK: OpCapability DotProductInput4x8BitPacked
// CHECK: OpExtension "SPV_KHR_integer_dot_product"
// CHECK-COUNT-1: OpSDot
// CHECK-COUNT-1: OpUDot
// CHECK: OpBitFieldSExtract
// CHECK: OpBitFieldUExtract
// CHECK: OpIMul
// CHECK: OpExtInst {{.*}} UnpackHalf2x16
// CHECK: OpFMul
// CHECK: OpFAdd
// CHECK-NOT: OpLoopMerge

RWStructuredBuffer<uint> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x45d9f3bu + 0x89abcdefu;
  uint b = tid.x * 0x27d4eb2du + 0x13579bdfu;
  int accum = asint(tid.x * 31u + 7u);

  int r = vk::amd::SDot4I8(a, b, accum);
  r ^= int(vk::amd::UDot4U8(a, b, uint(accum)));
  r ^= vk::amd::SDot2I16(a, b, accum);
  r ^= int(vk::amd::UDot2U16(a, b, uint(accum)));
  r ^= vk::amd::SUDot4I8U8(a, b, accum);
  r ^= vk::amd::USDot4U8I8(a, b, accum);

  uint a4 = a ^ 0x13579bdfu;
  uint b4 = b ^ 0x2468ace0u;
  r ^= vk::amd::SDot8I4(a4, b4, accum + 1);
  r ^= int(vk::amd::UDot8U4(a4 ^ 0x11111111u, b4, uint(accum) + 2u));
  r ^= vk::amd::SUDot8I4U4(a4, b4 ^ 0x22222222u, accum + 3);
  r ^= vk::amd::USDot8U4I4(a4 ^ 0x44444444u, b4, accum + 4);

  float f = vk::amd::FDot2F32F16Bits(
      a & 0xfbfffbffu, b & 0xfbfffbffu, float(accum) * 0.0009765625f);
  Out[tid.x] = asuint(r) ^ asuint(f);
}
