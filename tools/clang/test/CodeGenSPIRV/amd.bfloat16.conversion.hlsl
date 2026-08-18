// RUN: %dxc -T cs_6_2 -E main -enable-16bit-types -fcgl -spirv -fspv-target-env=vulkan1.3 %s | FileCheck %s

#include <vk/amd/bfloat16.h>

// CHECK: OpCapability BFloat16TypeKHR
// CHECK: OpExtension "SPV_KHR_bfloat16"
// CHECK: [[BF16:%[0-9]+]] = OpTypeFloat 16 BFloat16KHR
// CHECK: [[V2BF16:%[0-9]+]] = OpTypeVector [[BF16]] 2
// CHECK-COUNT-4: OpFConvert
// CHECK: OpBitcast %ushort
// CHECK: OpBitcast %uint

RWByteAddressBuffer Data : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint index = tid.x * 16u;
  float scalar = asfloat(Data.Load(index + 0u));
  float2 pair = asfloat(Data.Load2(index + 4u));
  uint packedBF16 = Data.Load(index + 12u);

  uint scalarBits = vk::amd::F32ToBF16Bits(scalar);
  uint pairBits = vk::amd::F32x2ToBF16x2Bits(pair);
  float widenedScalar = vk::amd::BF16BitsToF32(packedBF16);
  float2 widenedPair = vk::amd::BF16x2BitsToF32x2(packedBF16);

  Data.Store(65536u, scalarBits);
  Data.Store(65540u, pairBits);
  Data.Store(65544u, asuint(widenedScalar));
  Data.Store2(65548u, asuint(widenedPair));
}
