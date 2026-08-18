// RUN: %dxc -T cs_6_6 -E main -fcgl -spirv -fspv-target-env=vulkan1.2 %s | FileCheck %s

// CHECK: OpCapability Int64
// CHECK: OpCapability Int64Atomics
// CHECK: OpAtomicIAdd
// CHECK: OpAtomicAnd
// CHECK: OpAtomicOr
// CHECK: OpAtomicXor
// CHECK: OpAtomicUMin
// CHECK: OpAtomicUMax
// CHECK: OpAtomicSMin
// CHECK: OpAtomicSMax
// CHECK: OpAtomicExchange
// CHECK: OpAtomicCompareExchange

RWStructuredBuffer<uint64_t> U64 : register(u0);
RWStructuredBuffer<int64_t> I64 : register(u1);
RWStructuredBuffer<uint4> Out : register(u2);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint base = tid.x * 10u;
  uint64_t value = uint64_t(tid.x + 1u) * 0x100000001ull;
  int64_t signedValue = int64_t(tid.x) - 31;

  uint64_t oldAdd, oldAnd, oldOr, oldXor, oldMin, oldMax, oldExchange, oldCmp;
  int64_t oldSMin, oldSMax;

  InterlockedAdd(U64[base + 0u], value, oldAdd);
  InterlockedAnd(U64[base + 1u], value | 1ull, oldAnd);
  InterlockedOr(U64[base + 2u], value, oldOr);
  InterlockedXor(U64[base + 3u], value, oldXor);
  InterlockedMin(U64[base + 4u], value, oldMin);
  InterlockedMax(U64[base + 5u], value, oldMax);
  InterlockedExchange(U64[base + 6u], value, oldExchange);
  InterlockedCompareExchange(U64[base + 7u], value, value + 1ull, oldCmp);
  InterlockedMin(I64[base + 8u], signedValue, oldSMin);
  InterlockedMax(I64[base + 9u], signedValue, oldSMax);

  uint64_t fold0 = oldAdd ^ oldAnd ^ oldOr ^ oldXor ^ oldMin;
  uint64_t fold1 = oldMax ^ oldExchange ^ oldCmp ^ asuint64(oldSMin) ^ asuint64(oldSMax);
  Out[tid.x] = uint4(uint(fold0), uint(fold0 >> 32u),
                     uint(fold1), uint(fold1 >> 32u));
}
