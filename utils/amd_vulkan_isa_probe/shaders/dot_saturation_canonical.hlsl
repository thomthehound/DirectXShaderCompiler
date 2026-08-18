#include <vk/amd/dot.h>

RWStructuredBuffer<uint4> Out : register(u0);

int ClampI64ToI32(int64_t value) {
  int64_t hi = int64_t(2147483647);
  int64_t lo = int64_t(-2147483647) - 1;
  return value > hi ? 2147483647 : (value < lo ? asint(0x80000000u) : int(value));
}

uint ClampU64ToU32(uint64_t value) {
  return value > uint64_t(0xffffffffu) ? 0xffffffffu : uint(value);
}

int CanonicalSDot4AccSat(uint a, uint b, int accum) {
  int64_t sum = int64_t(accum);
  [unroll]
  for (uint lane = 0u; lane < 4u; ++lane) {
    uint shift = lane * 8u;
    sum += int64_t(vk::amd::SBfe(asint(a), shift, 8u)) *
           int64_t(vk::amd::SBfe(asint(b), shift, 8u));
  }
  return ClampI64ToI32(sum);
}

uint CanonicalUDot4AccSat(uint a, uint b, uint accum) {
  uint64_t sum = uint64_t(accum);
  [unroll]
  for (uint lane = 0u; lane < 4u; ++lane) {
    uint shift = lane * 8u;
    sum += uint64_t(vk::amd::UBfe(a, shift, 8u)) *
           uint64_t(vk::amd::UBfe(b, shift, 8u));
  }
  return ClampU64ToU32(sum);
}

int CanonicalSUDot4AccSat(uint signedA, uint unsignedB, int accum) {
  int64_t sum = int64_t(accum);
  [unroll]
  for (uint lane = 0u; lane < 4u; ++lane) {
    uint shift = lane * 8u;
    sum += int64_t(vk::amd::SBfe(asint(signedA), shift, 8u)) *
           int64_t(vk::amd::UBfe(unsignedB, shift, 8u));
  }
  return ClampI64ToI32(sum);
}

int CanonicalUSDot4AccSat(uint unsignedA, uint signedB, int accum) {
  int64_t sum = int64_t(accum);
  [unroll]
  for (uint lane = 0u; lane < 4u; ++lane) {
    uint shift = lane * 8u;
    sum += int64_t(vk::amd::UBfe(unsignedA, shift, 8u)) *
           int64_t(vk::amd::SBfe(asint(signedB), shift, 8u));
  }
  return ClampI64ToI32(sum);
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x45d9f3bu + 0x89abcdefu;
  uint b = tid.x * 0x27d4eb2du + 0x13579bdfu;
  int sa = (tid.x & 1u) != 0u ? 0x7ffffff0 : asint(0x80000010u);
  uint ua = 0xfffffff0u - (tid.x & 7u);

  int ss = CanonicalSDot4AccSat(a ^ 0x80808080u, b, sa);
  uint uu = CanonicalUDot4AccSat(a, b ^ 0xffffffffu, ua);
  int su = CanonicalSUDot4AccSat(a ^ 0x80808080u, b, sa);
  int us = CanonicalUSDot4AccSat(a, b ^ 0x80808080u, sa);
  Out[tid.x] = uint4(asuint(ss), uu, asuint(su), asuint(us));
}
