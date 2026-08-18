// RUN: %dxc -T cs_6_2 -E main -enable-16bit-types -fcgl -spirv -fspv-target-env=vulkan1.3 %s | FileCheck %s

#include <vk/amd/mixed_dot.h>

// CHECK: OpCapability Float16
// CHECK: OpCapability BFloat16TypeKHR
// CHECK: OpCapability BFloat16DotProductKHR
// CHECK: OpCapability DotProductFloat16AccFloat32VALVE
// CHECK: OpCapability DotProductFloat16AccFloat16VALVE
// CHECK: OpCapability DotProductBFloat16AccVALVE
// CHECK: OpExtension "SPV_KHR_bfloat16"
// CHECK: OpExtension "SPV_VALVE_mixed_float_dot_product"
// CHECK: [[BF16:%[0-9A-Za-z_]+]] = OpTypeFloat 16 BFloat16KHR
// CHECK: [[BF16X2:%[0-9A-Za-z_]+]] = OpTypeVector [[BF16]] 2
// CHECK-COUNT-2: OpFDot2MixAcc32VALVE
// CHECK-COUNT-2: OpFDot2MixAcc16VALVE
// CHECK: OpDot [[BF16]]

RWStructuredBuffer<uint2> InBits : register(t0);
RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint2 packed = InBits[tid.x];
  float acc32 = float(tid.x) * 0.125f + 0.25f;
  uint16_t acc16 = uint16_t((tid.x * 257u + 0x3c00u) & 0xffffu);

  float f16f32 = vk::amd::Dot2F16PackedAccF32(packed.x, packed.y, acc32);
  uint f16f16 =
      vk::amd::Dot2F16PackedAccF16Bits(packed.x, packed.y, acc16);
  float bf16f32 = vk::amd::Dot2BF16AccF32(packed.y, packed.x, acc32);
  uint bf16bf16 =
      vk::amd::Dot2BF16AccBF16Bits(packed.y, packed.x, acc16);
  uint bf16dot = vk::amd::Dot2BF16Bits(packed.x, packed.y);

  Out[tid.x] = uint4(asuint(f16f32), f16f16,
                     asuint(bf16f32), bf16bf16 ^ bf16dot);
}
