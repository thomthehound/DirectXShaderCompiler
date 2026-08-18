#include <vk/amd/math.h>

RWStructuredBuffer<uint> Out : register(u0);

// Keep every operation data-dependent so the Vulkan compiler must lower it.
// The probe only creates the pipeline; it does not execute this shader.
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint seed = tid.x * 0x9e3779b9u + 0x7f4a7c15u;
  uint seed2 = seed ^ (tid.x * 0x45d9f3bu + 0x13579bdfu);
  uint seed3 = seed2 + tid.x * 17u + 3u;
  int signedSeed = asint(seed ^ 0x81234567u);

  uint ubfe = vk::amd::UBfe(seed, (tid.x & 7u) + 1u, 11u);
  int sbfe = vk::amd::SBfe(signedSeed, (tid.x & 3u) + 2u, 13u);
  uint rev = vk::amd::BitReverse(seed);
  uint bits = vk::amd::BitCount(seed ^ 0xa55aa55au);
  uint mulU24 = vk::amd::MulU24(seed, seed2);
  int mulI24 = vk::amd::MulI24(signedSeed, asint(seed2));
  uint mulHiU24 = vk::amd::MulHiU24(seed, seed2);
  int mulHiI24 = vk::amd::MulHiI24(signedSeed, asint(seed2));
  uint madU24 = vk::amd::MadU24(seed, seed2, seed3);
  int madI24 = vk::amd::MadI24(signedSeed, asint(seed2), asint(seed3));

  uint sadU8 = vk::amd::SadU8(seed, seed2, tid.x + 3u);
  uint sadHiU8 = vk::amd::SadHiU8(seed2, seed, tid.x + 5u);
  uint sadU16 = vk::amd::SadU16(seed, seed2, tid.x + 7u);
  uint sadU32 = vk::amd::SadU32(seed, seed2, tid.x + 11u);
  uint msadU8 = vk::amd::MsadU8(seed, seed2 | 0x00000100u, tid.x + 13u);

  uint64_t source64 = (uint64_t(seed3) << 32u) | uint64_t(seed);
  uint64_t accum16 = (uint64_t((tid.x + 7u) & 0xffffu) << 48u) |
                     (uint64_t((tid.x + 5u) & 0xffffu) << 32u) |
                     (uint64_t((tid.x + 3u) & 0xffffu) << 16u) |
                     uint64_t((tid.x + 1u) & 0xffffu);
  uint64_t qsad = vk::amd::QsadPkU16U8(source64, seed2, accum16);
  uint64_t mqsad = vk::amd::MqsadPkU16U8(source64, seed2 | 0x00010000u, accum16);
  uint4 mqsad32 = vk::amd::MqsadU32U8(
      source64, seed2 | 0x00000100u,
      uint4(tid.x + 17u, tid.x + 19u, tid.x + 23u, tid.x + 29u));

  uint lerp = vk::amd::LerpU8(seed, seed2, seed3);
  uint bfi = vk::amd::Bfi(seed, seed2, seed3);
  uint xad = vk::amd::XadU32(seed, seed2, seed3);
  uint lshlAdd = vk::amd::LshlAddU32(seed, seed2, seed3);
  uint addLshl = vk::amd::AddLshlU32(seed, seed2, seed3);
  uint add3 = vk::amd::Add3U32(seed, seed2, seed3);
  uint lshlOr = vk::amd::LshlOrB32(seed, seed2, seed3);
  uint andOr = vk::amd::AndOrB32(seed, seed2, seed3);
  uint or3 = vk::amd::Or3B32(seed, seed2, seed3);

  float x = 0.75f + float(seed & 1023u) * (1.0f / 2048.0f);
  float y = vk::amd::Rcp(x + 0.25f);
  y += vk::amd::Sqrt(x + 1.0f);
  y += vk::amd::Rsq(x + 2.0f);
  y += vk::amd::Sin(x) + vk::amd::Cos(x * 0.5f);
  y += vk::amd::Log2(x + 1.0f) + vk::amd::Exp2(x * 0.125f);
  y += vk::amd::Fract(x * 1.75f);
  y += vk::amd::FMed3(x, -x, y);

  Out[tid.x] = ubfe ^ asuint(sbfe) ^ rev ^ bits ^ mulU24 ^ asuint(mulI24) ^
               mulHiU24 ^ asuint(mulHiI24) ^ madU24 ^ asuint(madI24) ^
               sadU8 ^ sadHiU8 ^ sadU16 ^ sadU32 ^ msadU8 ^
               uint(qsad) ^ uint(qsad >> 32u) ^ uint(mqsad) ^ uint(mqsad >> 32u) ^
               mqsad32.x ^ mqsad32.y ^ mqsad32.z ^ mqsad32.w ^
               lerp ^ bfi ^ xad ^ lshlAdd ^ addLshl ^ add3 ^ lshlOr ^ andOr ^
               or3 ^ asuint(y);
}
