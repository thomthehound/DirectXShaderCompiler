// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

#include <vk/amd/packing.h>

// CHECK: [[GLSL:%[0-9]+]] = OpExtInstImport "GLSL.std.450"
// CHECK: OpExtInst %uint [[GLSL]] PackSnorm2x16
// CHECK: OpExtInst %uint [[GLSL]] PackUnorm2x16
// CHECK: OpExtInst %uint [[GLSL]] PackSnorm4x8
// CHECK: OpExtInst %uint [[GLSL]] PackUnorm4x8
// CHECK: OpExtInst {{.*}} SClamp
// CHECK: OpExtInst {{.*}} UMin
// CHECK: OpBitwiseAnd
// CHECK: OpShiftLeftLogical
// CHECK: OpBitwiseOr

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float x = float(int(tid.x & 31u) - 15) * (1.0f / 16.0f);
  float y = float(int((tid.x * 7u) & 31u) - 15) * (1.0f / 16.0f);
  float2 n2 = float2(x, y);
  float4 n4 = float4(x, y, 0.25f - x * 0.125f, 0.75f + y * 0.125f);

  int2 si = int2(int(tid.x) * 4097 - 70000,
                 int(tid.x) * -8191 + 90000);
  uint2 ui = uint2(tid.x * 4097u, tid.x * 8191u + 60000u);

  Out[tid.x] = uint4(vk::amd::PackSnorm2x16(n2),
                     vk::amd::PackUnorm2x16(n2 * 0.5f + 0.5f),
                     vk::amd::PackI16x2(si) ^ vk::amd::PackU16x2(ui),
                     vk::amd::PackSnorm4x8(n4) ^
                         vk::amd::PackUnorm4x8(n4 * 0.5f + 0.5f));
}
