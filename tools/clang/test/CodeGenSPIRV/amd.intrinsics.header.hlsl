// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

#include <vk/amd/intrinsics.h>

// Exercise every registered AMD extended instruction exposed by the header.
// Keep this exhaustive: adding an AMD extinst wrapper without adding it here
// leaves the public surface unvalidated.
//
// CHECK: OpExtension "SPV_AMD_shader_trinary_minmax"
// CHECK: OpExtension "SPV_AMD_shader_ballot"
// CHECK: OpExtension "SPV_AMD_gcn_shader"
// CHECK-DAG: [[MINMAX:%[0-9]+]] = OpExtInstImport "SPV_AMD_shader_trinary_minmax"
// CHECK-DAG: [[BALLOT:%[0-9]+]] = OpExtInstImport "SPV_AMD_shader_ballot"
// CHECK-DAG: [[GCN:%[0-9]+]] = OpExtInstImport "SPV_AMD_gcn_shader"
// CHECK: OpExtInst %float [[MINMAX]] FMin3AMD
// CHECK: OpExtInst %uint [[MINMAX]] UMin3AMD
// CHECK: OpExtInst %int [[MINMAX]] SMin3AMD
// CHECK: OpExtInst %float [[MINMAX]] FMax3AMD
// CHECK: OpExtInst %uint [[MINMAX]] UMax3AMD
// CHECK: OpExtInst %int [[MINMAX]] SMax3AMD
// CHECK: OpExtInst %float [[MINMAX]] FMid3AMD
// CHECK: OpExtInst %uint [[MINMAX]] UMid3AMD
// CHECK: OpExtInst %int [[MINMAX]] SMid3AMD
// CHECK: OpExtInst %uint [[BALLOT]] SwizzleInvocationsAMD
// CHECK: OpExtInst %uint [[BALLOT]] SwizzleInvocationsMaskedAMD
// CHECK: OpExtInst %uint [[BALLOT]] WriteInvocationAMD
// CHECK: OpExtInst %uint [[BALLOT]] MbcntAMD
// CHECK: OpExtInst %float [[GCN]] CubeFaceIndexAMD
// CHECK: OpExtInst %v2float [[GCN]] CubeFaceCoordAMD
// CHECK: OpExtInst %ulong [[GCN]] TimeAMD

RWStructuredBuffer<uint> outBuffer : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float fmin3 = vk::amd::FMin3(1.0f, 2.0f, 3.0f);
  uint umin3 = vk::amd::UMin3(1u, 2u, 3u);
  int smin3 = vk::amd::SMin3(-1, 2, 3);
  float fmax3 = vk::amd::FMax3(1.0f, 2.0f, 3.0f);
  uint umax3 = vk::amd::UMax3(1u, 2u, 3u);
  int smax3 = vk::amd::SMax3(-1, 2, 3);
  float fmid3 = vk::amd::FMid3(1.0f, 2.0f, 3.0f);
  uint umid3 = vk::amd::UMid3(1u, 2u, 3u);
  int smid3 = vk::amd::SMid3(-1, 2, 3);

  uint swizzle = vk::amd::SwizzleInvocations(tid.x, uint4(0, 1, 2, 3));
  uint swizzleMasked =
      vk::amd::SwizzleInvocationsMasked(tid.x, uint3(31, 0, 0));
  uint written = vk::amd::WriteInvocation(tid.x, 0x12345678u, 0u);
  uint mbcnt = vk::amd::Mbcnt(0xffffffffu);

  float3 cubeP = float3(1.0f, 0.25f, -0.5f);
  float cubeFace = vk::amd::CubeFaceIndex(cubeP);
  float2 cubeCoord = vk::amd::CubeFaceCoord(cubeP);
  uint64_t clock = vk::amd::Time();

  uint fold = asuint(fmin3 + fmax3 + fmid3 + cubeFace + cubeCoord.x +
                     cubeCoord.y);
  fold += umin3 + umax3 + umid3 + asuint(smin3 + smax3 + smid3);
  fold += swizzle + swizzleMasked + written + mbcnt;
  fold += uint(clock) ^ uint(clock >> 32);
  outBuffer[tid.x] = fold;
}
