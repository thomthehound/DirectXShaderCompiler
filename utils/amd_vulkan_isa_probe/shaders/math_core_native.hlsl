#include <vk/amd/math.h>

RWStructuredBuffer<uint> Out : register(u0);

// Keep every operation data-dependent so the Vulkan compiler must lower it.
// The probe only creates the pipeline; it does not execute this shader.
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint seed = tid.x * 0x9e3779b9u + 0x7f4a7c15u;
  uint seed2 = seed ^ (tid.x * 0x45d9f3bu + 0x13579bdfu);
  int signedSeed = asint(seed ^ 0x81234567u);

  uint ubfe = vk::amd::UBfe(seed, (tid.x & 7u) + 1u, 11u);
  int sbfe = vk::amd::SBfe(signedSeed, (tid.x & 3u) + 2u, 13u);
  uint rev = vk::amd::BitReverse(seed);
  uint bits = vk::amd::BitCount(seed ^ 0xa55aa55au);
  uint mulU24 = vk::amd::MulU24(seed, seed2);
  int mulI24 = vk::amd::MulI24(signedSeed, asint(seed2));

  uint sadU8 = vk::amd::SadU8(seed, seed2, tid.x + 3u);
  uint sadHiU8 = vk::amd::SadHiU8(seed2, seed, tid.x + 5u);
  uint sadU16 = vk::amd::SadU16(seed, seed2, tid.x + 7u);
  uint sadU32 = vk::amd::SadU32(seed, seed2, tid.x + 11u);
  uint msadU8 = vk::amd::MsadU8(seed, seed2 | 0x00000100u, tid.x + 13u);

  float x = 0.75f + float(seed & 1023u) * (1.0f / 2048.0f);
  float y = vk::amd::Rcp(x + 0.25f);
  y += vk::amd::Sqrt(x + 1.0f);
  y += vk::amd::Rsq(x + 2.0f);
  y += vk::amd::Sin(x) + vk::amd::Cos(x * 0.5f);
  y += vk::amd::Log2(x + 1.0f) + vk::amd::Exp2(x * 0.125f);
  y += vk::amd::Fract(x * 1.75f);
  y += vk::amd::FMed3(x, -x, y);

  Out[tid.x] = ubfe ^ asuint(sbfe) ^ rev ^ bits ^ mulU24 ^ asuint(mulI24) ^
               sadU8 ^ sadHiU8 ^ sadU16 ^ sadU32 ^ msadU8 ^ asuint(y);
}
