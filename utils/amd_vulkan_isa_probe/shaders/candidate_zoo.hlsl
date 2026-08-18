#include <vk/amd/math.h>

RWStructuredBuffer<uint4> Out : register(u0);

#ifndef APUSR_CANDIDATE
#define APUSR_CANDIDATE 0
#endif

uint byte_at(uint x, uint byte_index) {
  return vk::amd::UBfe(x, byte_index * 8u, 8u);
}

uint pack4(uint b0, uint b1, uint b2, uint b3) {
  return (b0 & 0xffu) | ((b1 & 0xffu) << 8u) |
         ((b2 & 0xffu) << 16u) | ((b3 & 0xffu) << 24u);
}

// Every candidate below has ordinary, fully specified HLSL semantics. No row
// claims to describe an AMD instruction. The installed Radeon compiler gets to
// prove equivalence by selecting a specialized instruction in the ISA dump.
uint candidate(uint a, uint b, uint c, uint tid) {
#if APUSR_CANDIDATE == 0
  // 64-bit concatenation: low=a, high=b, dynamic byte window 1..3.
  uint sh = ((tid % 3u) + 1u) * 8u;
  uint64_t pair = uint64_t(a) | (uint64_t(b) << 32u);
  return uint(pair >> sh);
#elif APUSR_CANDIDATE == 1
  // Reversed concatenation: low=b, high=a.
  uint sh = ((tid % 3u) + 1u) * 8u;
  uint64_t pair = uint64_t(b) | (uint64_t(a) << 32u);
  return uint(pair >> sh);
#elif APUSR_CANDIDATE == 2
  // 32-bit shift/or spelling of candidate 0, byte shifts 8/16/24 only.
  uint sh = ((tid % 3u) + 1u) * 8u;
  return (a >> sh) | (b << (32u - sh));
#elif APUSR_CANDIDATE == 3
  // 32-bit shift/or spelling with reversed operands.
  uint sh = ((tid % 3u) + 1u) * 8u;
  return (b >> sh) | (a << (32u - sh));
#elif APUSR_CANDIDATE == 4
  // Bit-granularity 64-bit window, dynamic shift 1..31.
  uint sh = (tid & 30u) + 1u;
  uint64_t pair = uint64_t(a) | (uint64_t(b) << 32u);
  return uint(pair >> sh);
#elif APUSR_CANDIDATE == 5
  // Reversed bit-granularity 64-bit window.
  uint sh = (tid & 30u) + 1u;
  uint64_t pair = uint64_t(b) | (uint64_t(a) << 32u);
  return uint(pair >> sh);
#elif APUSR_CANDIDATE == 6
  // Bit-granularity 32-bit shift/or spelling.
  uint sh = (tid & 30u) + 1u;
  return (a >> sh) | (b << (32u - sh));
#elif APUSR_CANDIDATE == 7
  // Reversed bit-granularity 32-bit shift/or spelling.
  uint sh = (tid & 30u) + 1u;
  return (b >> sh) | (a << (32u - sh));
#elif APUSR_CANDIDATE == 8
  // Fixed 1-byte rolling window.
  return pack4(byte_at(a, 1u), byte_at(a, 2u), byte_at(a, 3u), byte_at(b, 0u));
#elif APUSR_CANDIDATE == 9
  // Fixed 2-byte rolling window.
  return pack4(byte_at(a, 2u), byte_at(a, 3u), byte_at(b, 0u), byte_at(b, 1u));
#elif APUSR_CANDIDATE == 10
  // Fixed 3-byte rolling window.
  return pack4(byte_at(a, 3u), byte_at(b, 0u), byte_at(b, 1u), byte_at(b, 2u));
#elif APUSR_CANDIDATE == 11
  // Reverse bytes within one dword.
  return pack4(byte_at(a, 3u), byte_at(a, 2u), byte_at(a, 1u), byte_at(a, 0u));
#elif APUSR_CANDIDATE == 12
  // Interleave low bytes from two dwords.
  return pack4(byte_at(a, 0u), byte_at(b, 0u), byte_at(a, 1u), byte_at(b, 1u));
#elif APUSR_CANDIDATE == 13
  // Interleave high bytes from two dwords.
  return pack4(byte_at(a, 2u), byte_at(b, 2u), byte_at(a, 3u), byte_at(b, 3u));
#elif APUSR_CANDIDATE == 14
  // Runtime byte selection from two sources. Selector lanes are independently
  // 0..7 and therefore always valid.
  uint s0 = (c >> 0u) & 7u;
  uint s1 = (c >> 3u) & 7u;
  uint s2 = (c >> 6u) & 7u;
  uint s3 = (c >> 9u) & 7u;
  uint64_t pair = uint64_t(a) | (uint64_t(b) << 32u);
  uint r0 = uint((pair >> (s0 * 8u)) & 0xffu);
  uint r1 = uint((pair >> (s1 * 8u)) & 0xffu);
  uint r2 = uint((pair >> (s2 * 8u)) & 0xffu);
  uint r3 = uint((pair >> (s3 * 8u)) & 0xffu);
  return pack4(r0, r1, r2, r3);
#elif APUSR_CANDIDATE == 15
  // Fixed cross-source byte permutation.
  return pack4(byte_at(b, 3u), byte_at(a, 0u), byte_at(b, 1u), byte_at(a, 2u));
#elif APUSR_CANDIDATE == 16
  // Another fixed cross-source permutation chosen to defeat simple rotations.
  return pack4(byte_at(a, 1u), byte_at(b, 3u), byte_at(a, 0u), byte_at(b, 2u));
#elif APUSR_CANDIDATE == 17
  // Four-way Boolean select written as masks; candidate for BFI/bitop3/permute
  // style combines depending on the backend.
  uint m0 = 0x00ff00ffu;
  uint m1 = 0xff00ff00u;
  return (a & m0) | (b & m1);
#elif APUSR_CANDIDATE == 18
  // Three-input Boolean expression with no undefined arithmetic.
  return (a & b) | (~a & c);
#elif APUSR_CANDIDATE == 19
  // Byte-wise average-ish packing graph, another chance for specialized packed
  // byte instructions while retaining exact modulo-256 source semantics.
  uint r0 = (byte_at(a, 0u) + byte_at(b, 0u)) >> 1u;
  uint r1 = (byte_at(a, 1u) + byte_at(b, 1u) + 1u) >> 1u;
  uint r2 = (byte_at(a, 2u) + byte_at(b, 2u)) >> 1u;
  uint r3 = (byte_at(a, 3u) + byte_at(b, 3u) + 1u) >> 1u;
  return pack4(r0, r1, r2, r3);
#else
#error Unknown APUSR_CANDIDATE
#endif
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x9e3779b9u + 0x13579bdfu;
  uint b = tid.x * 0x45d9f3bu + 0x2468ace0u;
  uint c = tid.x * 0x27d4eb2du + 0x89abcdefu;
  uint r = candidate(a, b, c, tid.x);
  Out[tid.x] = uint4(r, r ^ a, r + b, r ^ c);
}
