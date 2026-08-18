// RUN: %dxc -T cs_6_6 -E main %s | FileCheck %s

// CHECK: dx.op.atomicBinOp.i64
// CHECK: dx.op.atomicCompareExchange.i64

RWTexture1D<uint64_t> Image1D : register(u0);
RWTexture2D<uint64_t> Image2D : register(u1);
RWTexture3D<uint64_t> Image3D : register(u2);
RWStructuredBuffer<uint4> Out : register(u3);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint64_t value = (uint64_t(tid.x + 1u) << 32u) | uint64_t(tid.y + 1u);
  uint64_t old1, old2, old3;

  InterlockedAdd(Image1D[tid.x], value, old1);
  InterlockedMin(Image2D[tid.xy], value, old2);
  InterlockedCompareExchange(Image3D[tid], value, value + uint64_t(1u), old3);

  uint index = tid.y * 8u + tid.x;
  Out[index] = uint4(uint(old1), uint(old2), uint(old3),
                     uint((old1 ^ old2 ^ old3) >> 32u));
}
