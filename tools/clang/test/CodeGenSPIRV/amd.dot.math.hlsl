// RUN: %dxc -T cs_6_4 -E main -fcgl -spirv %s | FileCheck %s

#include <vk/amd/dot.h>

// The 4x8 same-signedness forms must use the standardized packed-dot contract.
// The 2x16 and mixed-signedness forms are still canonical recovery candidates.
// CHECK: OpCapability DotProduct
// CHECK: OpCapability DotProductInput4x8BitPacked
// CHECK: OpExtension "SPV_KHR_integer_dot_product"
// CHECK-COUNT-1: OpSDot
// CHECK-COUNT-1: OpUDot
// CHECK: OpBitFieldSExtract
// CHECK: OpBitFieldUExtract
// CHECK: OpIMul

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
  Out[tid.x] = asuint(r);
}
