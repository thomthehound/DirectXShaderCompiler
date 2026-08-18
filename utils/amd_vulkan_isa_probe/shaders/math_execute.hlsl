#include <vk/amd/math.h>

RWStructuredBuffer<uint> Out : register(u0);

static const uint WORDS_PER_LANE = 20u;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint lane = tid.x;
  uint base = lane * WORDS_PER_LANE;
  uint a = lane * 0x45d9f3bu + 0x13579bdfu;
  uint b = lane * 0x27d4eb2du + 0x2468ace1u;
  uint c = lane * 17u + 3u;
  uint reference = b ^ 0x00110022u;
  uint64_t source = (uint64_t(c ^ 0xa5a55a5au) << 32u) | uint64_t(a);
  uint64_t accum16 = (uint64_t((lane + 7u) & 0xffffu) << 48u) |
                     (uint64_t((lane + 5u) & 0xffffu) << 32u) |
                     (uint64_t((lane + 3u) & 0xffffu) << 16u) |
                     uint64_t((lane + 1u) & 0xffffu);
  uint4 accum32 = uint4(lane + 11u, lane + 13u, lane + 17u, lane + 19u);

  uint64_t q = vk::amd::QsadPkU16U8(source, reference, accum16);
  uint64_t mq = vk::amd::MqsadPkU16U8(source, reference, accum16);
  uint4 mq32 = vk::amd::MqsadU32U8(source, reference, accum32);

  Out[base + 0u] = vk::amd::MulU24(a, b);
  Out[base + 1u] = asuint(vk::amd::MulI24(asint(a), asint(b)));
  Out[base + 2u] = vk::amd::MulHiU24(a, b);
  Out[base + 3u] = asuint(vk::amd::MulHiI24(asint(a), asint(b)));
  Out[base + 4u] = vk::amd::MadU24(a, b, c);
  Out[base + 5u] = asuint(vk::amd::MadI24(asint(a), asint(b), asint(c)));
  Out[base + 6u] = vk::amd::LerpU8(a, b, c);
  Out[base + 7u] = vk::amd::Bfi(a, b, c);
  Out[base + 8u] = vk::amd::XadU32(a, b, c);
  Out[base + 9u] = vk::amd::SadU8(a, b, c);
  Out[base + 10u] = vk::amd::SadHiU8(a, b, c);
  Out[base + 11u] = vk::amd::SadU16(a, b, c);
  Out[base + 12u] = vk::amd::SadU32(a, b, c);
  Out[base + 13u] = vk::amd::MsadU8(a, reference, c);
  Out[base + 14u] = uint(q);
  Out[base + 15u] = uint(q >> 32u);
  Out[base + 16u] = uint(mq);
  Out[base + 17u] = uint(mq >> 32u);
  Out[base + 18u] = mq32.x ^ mq32.y;
  Out[base + 19u] = mq32.z ^ mq32.w;
}
