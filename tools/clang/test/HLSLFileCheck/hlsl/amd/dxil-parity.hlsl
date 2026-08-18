// RUN: %dxc -E main -T cs_6_4 %s | FileCheck %s

#include <dx/amd/intrinsics.h>
#include <dx/amd/dot.h>

// Preserve the first-class packed dot operations in final DXIL.
// CHECK: call i32 @dx.op.dot4AddPacked.i32(i32 163,
// CHECK: call i32 @dx.op.dot4AddPacked.i32(i32 164,
// Exercise wave transport/reduction through actual DXIL wave ops rather than
// scalarized lane emulation.
// CHECK: dx.op.waveReadLaneAt
// CHECK: dx.op.waveActiveOp

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x45d9f3bu + 0x89abcdefu;
  uint b = tid.x * 0x27d4eb2du + 0x13579bdfu;
  int sd = dx::amd::SDot4I8(a, b, int(tid.x));
  uint ud = dx::amd::UDot4U8(a, b, tid.x + 3u);
  uint lane = dx::amd::XorLane(a, 1u);
  uint reduced = dx::amd::ActiveSum(b);
  uint4 msad = dx::amd::Msad4(a, uint2(b, lane),
                              uint4(tid.x + 1u, tid.x + 3u,
                                    tid.x + 5u, tid.x + 7u));
  Out[tid.x] = uint4(asuint(sd), ud, lane ^ reduced,
                     msad.x ^ msad.y ^ msad.z ^ msad.w);
}
