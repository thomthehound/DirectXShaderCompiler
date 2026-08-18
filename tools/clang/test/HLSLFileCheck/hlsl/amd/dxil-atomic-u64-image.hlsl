// RUN: %dxc -T cs_6_6 -E main %s | FileCheck %s

// Shader Model 6.6 defines typed 64-bit atomics on RWTexture resources when the
// corresponding device capability is present. DXIL keeps them as i64 atomic
// operations; resource capability is checked by the runtime/device.
// CHECK: dx.op.atomicBinOp.i64
// CHECK: dx.op.atomicCompareExchange.i64

RWTexture2D<uint64_t> Image : register(u0);
RWStructuredBuffer<uint4> Out : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint2 p = tid.xy;
  uint64_t value = (uint64_t(tid.x + 1u) << 32u) | uint64_t(tid.y + 1u);
  uint64_t oldAdd, oldAnd, oldOr, oldXor, oldMin, oldMax, oldExchange, oldCmp;

  InterlockedAdd(Image[p], value, oldAdd);
  InterlockedAnd(Image[p], value | uint64_t(1u), oldAnd);
  InterlockedOr(Image[p], value, oldOr);
  InterlockedXor(Image[p], value, oldXor);
  InterlockedMin(Image[p], value, oldMin);
  InterlockedMax(Image[p], value, oldMax);
  InterlockedExchange(Image[p], value, oldExchange);
  InterlockedCompareExchange(Image[p], value, value + uint64_t(1u), oldCmp);

  uint64_t fold0 = oldAdd ^ oldAnd ^ oldOr ^ oldXor;
  uint64_t fold1 = oldMin ^ oldMax ^ oldExchange ^ oldCmp;
  uint index = tid.y * 8u + tid.x;
  Out[index] = uint4(uint(fold0), uint(fold0 >> 32u),
                     uint(fold1), uint(fold1 >> 32u));
}
