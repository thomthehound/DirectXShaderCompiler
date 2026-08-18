// RUN: %dxc -T cs_6_2 -E main -enable-16bit-types -fcgl -spirv -fspv-target-env=vulkan1.3 %s | FileCheck %s

#include <vk/valve/mixed_float_dot_product.h>

// CHECK: OpCapability Float16
// CHECK: OpCapability DotProductFloat16AccFloat32VALVE
// CHECK: OpCapability DotProductFloat16AccFloat16VALVE
// CHECK: OpExtension "SPV_VALVE_mixed_float_dot_product"
// CHECK: OpFDot2MixAcc32VALVE
// CHECK: OpFDot2MixAcc16VALVE

RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main() {
  float16_t2 a = float16_t2(1.25h, -2.5h);
  float16_t2 b = float16_t2(3.0h, 0.5h);

  float acc32 = vk::valve::FDot2Acc32(a, b, 7.0f);
  float16_t acc16 = vk::valve::FDot2Acc16(a, b, float16_t(2.0h));

  Out[0] = acc32;
  Out[1] = float(acc16);
}
