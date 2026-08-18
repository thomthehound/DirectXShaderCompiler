// RUN: %dxc -T cs_6_6 -E main %s | FileCheck %s

// CHECK: dx.op.atomicBinOp.i64
// CHECK: dx.op.atomicCompareExchange.i64

RWStructuredBuffer<uint64_t> U64 : register(u0);
RWStructuredBuffer<int64_t> I64 : register(u1);
RWStructuredBuffer<uint4> Out : register(u2);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint base = tid.x * 10u;
  uint64_t wideOne = (uint64_t(1u) << 32u) | uint64_t(1u);
  uint64_t value = uint64_t(tid.x + 1u) * wideOne;
  int64_t signedValue = int64_t(tid.x) - 31;

  uint64_t oldAdd, oldAnd, oldOr, oldXor, oldMin, oldMax, oldExchange, oldCmp;
  int64_t oldSMin, oldSMax;

  InterlockedAdd(U64[base + 0u], value, oldAdd);
  InterlockedAnd(U64[base + 1u], value | uint64_t(1u), oldAnd);
  InterlockedOr(U64[base + 2u], value, oldOr);
  InterlockedXor(U64[base + 3u], value, oldXor);
  InterlockedMin(U64[base + 4u], value, oldMin);
  InterlockedMax(U64[base + 5u], value, oldMax);
  InterlockedExchange(U64[base + 6u], value, oldExchange);
  InterlockedCompareExchange(U64[base + 7u], value, value + uint64_t(1u), oldCmp);
  InterlockedMin(I64[base + 8u], signedValue, oldSMin);
  InterlockedMax(I64[base + 9u], signedValue, oldSMax);

  uint64_t fold0 = oldAdd ^ oldAnd ^ oldOr ^ oldXor ^ oldMin;
  uint64_t fold1 = oldMax ^ oldExchange ^ oldCmp ^ uint64_t(oldSMin) ^ uint64_t(oldSMax);
  Out[tid.x] = uint4(uint(fold0), uint(fold0 >> 32u),
                     uint(fold1), uint(fold1 >> 32u));
}
