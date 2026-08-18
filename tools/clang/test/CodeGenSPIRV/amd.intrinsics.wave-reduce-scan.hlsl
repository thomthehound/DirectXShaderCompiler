// RUN: %dxc -T cs_6_0 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

#include <vk/amd/intrinsics.h>

// CHECK: OpCapability GroupNonUniformArithmetic
// CHECK: OpGroupNonUniformFAdd
// CHECK: OpGroupNonUniformIAdd
// CHECK: OpGroupNonUniformFMul
// CHECK: OpGroupNonUniformIMul
// CHECK: OpGroupNonUniformFMin
// CHECK: OpGroupNonUniformSMin
// CHECK: OpGroupNonUniformUMin
// CHECK: OpGroupNonUniformFMax
// CHECK: OpGroupNonUniformSMax
// CHECK: OpGroupNonUniformUMax
// CHECK: OpGroupNonUniformBitwiseAnd
// CHECK: OpGroupNonUniformBitwiseOr
// CHECK: OpGroupNonUniformBitwiseXor
// CHECK-COUNT-6: ExclusiveScan

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float f = float((tid.x & 15u) + 1u) * 0.125f;
  int i = int(tid.x) - 31;
  uint u = tid.x * 17u + 3u;

  float fReduce = vk::amd::ActiveSum(f) + vk::amd::ActiveProduct(f) +
                  vk::amd::ActiveMin(f) + vk::amd::ActiveMax(f);
  int iReduce = vk::amd::ActiveSum(i) + vk::amd::ActiveProduct(i) +
                vk::amd::ActiveMin(i) + vk::amd::ActiveMax(i) +
                vk::amd::ActiveBitAnd(i) + vk::amd::ActiveBitOr(i) +
                vk::amd::ActiveBitXor(i);
  uint uReduce = vk::amd::ActiveSum(u) + vk::amd::ActiveProduct(u) +
                 vk::amd::ActiveMin(u) + vk::amd::ActiveMax(u) +
                 vk::amd::ActiveBitAnd(u) + vk::amd::ActiveBitOr(u) +
                 vk::amd::ActiveBitXor(u);

  float fScan = vk::amd::PrefixSum(f) + vk::amd::PrefixProduct(f);
  int iScan = vk::amd::PrefixSum(i) + vk::amd::PrefixProduct(i);
  uint uScan = vk::amd::PrefixSum(u) + vk::amd::PrefixProduct(u);

  Out[tid.x] = uint4(asuint(fReduce + fScan), asuint(iReduce), uReduce,
                     asuint(iScan) ^ uScan);
}
