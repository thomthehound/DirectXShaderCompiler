// RUN: not %dxc -T cs_6_6 -E main -fcgl -spirv -fspv-target-env=vulkan1.2 %s 2>&1 | FileCheck %s --check-prefix=TYPED
// RUN: %dxc -T cs_6_6 -E main -fcgl -spirv -fspv-target-env=vulkan1.2 -fspv-extension=SPV_KHR_untyped_pointers %s | FileCheck %s --check-prefix=UNTYPED

// The typed raw-buffer representation must reject 64-bit atomics rather
// than attempt non-atomic 32-bit emulation. Enabling untyped pointers must
// produce actual 64-bit SPIR-V atomics for all eight operations.
// TYPED: error: 64-bit RWByteAddressBuffer atomics require SPV_KHR_untyped_pointers
// UNTYPED-DAG: OpCapability Int64Atomics
// UNTYPED-DAG: OpCapability UntypedPointersKHR
// UNTYPED-DAG: OpExtension "SPV_KHR_untyped_pointers"
// UNTYPED-DAG: OpAtomicIAdd
// UNTYPED-DAG: OpAtomicAnd
// UNTYPED-DAG: OpAtomicOr
// UNTYPED-DAG: OpAtomicXor
// UNTYPED-DAG: OpAtomicUMin
// UNTYPED-DAG: OpAtomicUMax
// UNTYPED-DAG: OpAtomicExchange
// UNTYPED-DAG: OpAtomicCompareExchange

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
