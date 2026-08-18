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

uint select_byte(uint a, uint b, uint selector) {
  uint source = selector < 4u ? a : b;
  return byte_at(source, selector & 3u);
}

uint rolling_byte_window_select(uint low, uint high, uint byte_shift) {
  // byte_shift is constrained to 1..3 by callers. This is the same 32-bit
  // window as concatenating high:low and shifting right byte_shift*8, but is
  // deliberately spelled as byte selection/packing to give the optimizer a
  // structurally different candidate without requiring shaderInt64.
  if (byte_shift == 1u)
    return pack4(byte_at(low, 1u), byte_at(low, 2u), byte_at(low, 3u),
                 byte_at(high, 0u));
  if (byte_shift == 2u)
    return pack4(byte_at(low, 2u), byte_at(low, 3u), byte_at(high, 0u),
                 byte_at(high, 1u));
  return pack4(byte_at(low, 3u), byte_at(high, 0u), byte_at(high, 1u),
               byte_at(high, 2u));
}

// Every candidate below has ordinary, fully specified HLSL semantics. No row
// claims to describe an AMD instruction. The installed Radeon compiler gets to
// prove equivalence by selecting a specialized instruction in the ISA dump.
uint candidate(uint a, uint b, uint c, uint tid) {
#if APUSR_CANDIDATE == 0
  // Byte-select/pack spelling of a dynamic 1..3 byte window over high=b, low=a.
  return rolling_byte_window_select(a, b, (tid % 3u) + 1u);
#elif APUSR_CANDIDATE == 1
  // Same byte-select/pack spelling with operand order reversed.
  return rolling_byte_window_select(b, a, (tid % 3u) + 1u);
#elif APUSR_CANDIDATE == 2
  // 32-bit shift/or byte window, byte shifts 8/16/24 only.
  uint sh = ((tid % 3u) + 1u) * 8u;
  return (a >> sh) | (b << (32u - sh));
#elif APUSR_CANDIDATE == 3
  // 32-bit shift/or spelling with reversed operands.
  uint sh = ((tid % 3u) + 1u) * 8u;
  return (b >> sh) | (a << (32u - sh));
#elif APUSR_CANDIDATE == 4
  // Bit-window spelling using XOR. Shifted fields are disjoint for sh 1..31,
  // so XOR is exactly equivalent to OR while presenting a different combine.
  uint sh = (tid & 30u) + 1u;
  return (a >> sh) ^ (b << (32u - sh));
#elif APUSR_CANDIDATE == 5
  // Reversed bit-window XOR spelling.
  uint sh = (tid & 30u) + 1u;
  return (b >> sh) ^ (a << (32u - sh));
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
  return pack4(select_byte(a, b, s0), select_byte(a, b, s1),
               select_byte(a, b, s2), select_byte(a, b, s3));
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
  // byte instructions while retaining exact lane arithmetic.
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
