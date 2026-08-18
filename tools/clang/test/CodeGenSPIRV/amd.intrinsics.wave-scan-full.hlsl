// RUN: %dxc -T cs_6_0 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

#include <vk/amd/wave.h>

// CHECK: OpCapability GroupNonUniformArithmetic
// CHECK: OpGroupNonUniformFAdd {{.*}} InclusiveScan
// CHECK: OpGroupNonUniformIAdd {{.*}} InclusiveScan
// CHECK: OpGroupNonUniformFMul {{.*}} ExclusiveScan
// CHECK: OpGroupNonUniformIMul {{.*}} ExclusiveScan
// CHECK: OpGroupNonUniformFMin {{.*}} InclusiveScan
// CHECK: OpGroupNonUniformSMin {{.*}} ExclusiveScan
// CHECK: OpGroupNonUniformUMin {{.*}} InclusiveScan
// CHECK: OpGroupNonUniformFMax {{.*}} ExclusiveScan
// CHECK: OpGroupNonUniformSMax {{.*}} InclusiveScan
// CHECK: OpGroupNonUniformUMax {{.*}} ExclusiveScan
// CHECK: OpGroupNonUniformBitwiseAnd {{.*}} Reduce
// CHECK: OpGroupNonUniformBitwiseOr {{.*}} Reduce
// CHECK: OpGroupNonUniformBitwiseXor {{.*}} Reduce
// CHECK: OpTypeVector %float 2
// CHECK: OpTypeVector %int 3
// CHECK: OpTypeVector %uint 4

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint u = tid.x * 17u + 3u;
  int s = int(tid.x) - 31;
  float f = float(tid.x) * 0.125f + 0.25f;

  uint a = vk::amd::InclusiveSum(u);
  a ^= asuint(vk::amd::InclusiveSum(f));
  a ^= asuint(vk::amd::ExclusiveProduct(f));
  a ^= uint(vk::amd::ExclusiveProduct(s));
  a ^= asuint(vk::amd::InclusiveMin(f));
  a ^= uint(vk::amd::ExclusiveMin(s));
  a ^= vk::amd::InclusiveMin(u);
  a ^= asuint(vk::amd::ExclusiveMax(f));
  a ^= uint(vk::amd::InclusiveMax(s));
  a ^= vk::amd::ExclusiveMax(u);

  float2 fv = float2(f, f + 1.0f);
  int3 iv = int3(s, s + 1, s - 1);
  uint4 uv = uint4(u, u + 1u, u ^ 7u, u + 3u);
  float2 fs = vk::amd::InclusiveSum(fv);
  int3 im = vk::amd::ExclusiveMin(iv);
  uint4 ux = vk::amd::ReduceBitXor(uv);

  uint b = vk::amd::ReduceBitAnd(u) ^ vk::amd::ReduceBitOr(u) ^
           vk::amd::ReduceBitXor(u);
  b ^= asuint(fs.x) ^ asuint(fs.y);
  b ^= uint(im.x) ^ uint(im.y) ^ uint(im.z);
  b ^= ux.x ^ ux.y ^ ux.z ^ ux.w;

  Out[tid.x] = uint4(a, b, vk::amd::InclusiveProduct(u),
                     vk::amd::ExclusiveSum(u));
}
