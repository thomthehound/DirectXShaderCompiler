// RUN: not %dxc -T cs_6_6 -E main -fcgl -spirv -fspv-target-env=vulkan1.2 %s 2>&1 | FileCheck %s

// RWByteAddressBuffer is currently represented as 32-bit uint words in the
// SPIR-V backend. A correct 64-bit atomic requires a genuine 64-bit pointee;
// splitting the operation across two words would violate atomicity.
// TODO: add a legal 64-bit raw-buffer representation/alignment bridge, then
// replace this negative contract with Int64Atomics codegen checks.
// CHECK: error: intrinsic 'InterlockedAdd64' method unimplemented

RWByteAddressBuffer Data : register(u0);
RWStructuredBuffer<uint4> Out : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint base = tid.x * 64u;
  uint64_t value = (uint64_t(tid.x + 1u) << 32u) | uint64_t(tid.x ^ 0x55u);
  uint64_t oldAdd, oldAnd, oldOr, oldXor, oldMin, oldMax, oldExchange, oldCmp;

  Data.InterlockedAdd64(base + 0u, value, oldAdd);
  Data.InterlockedAnd64(base + 8u, value | uint64_t(1u), oldAnd);
  Data.InterlockedOr64(base + 16u, value, oldOr);
  Data.InterlockedXor64(base + 24u, value, oldXor);
  Data.InterlockedMin64(base + 32u, value, oldMin);
  Data.InterlockedMax64(base + 40u, value, oldMax);
  Data.InterlockedExchange64(base + 48u, value, oldExchange);
  Data.InterlockedCompareExchange64(base + 56u, value, value + uint64_t(1u), oldCmp);

  uint64_t fold0 = oldAdd ^ oldAnd ^ oldOr ^ oldXor;
  uint64_t fold1 = oldMin ^ oldMax ^ oldExchange ^ oldCmp;
  Out[tid.x] = uint4(uint(fold0), uint(fold0 >> 32u),
                     uint(fold1), uint(fold1 >> 32u));
}
