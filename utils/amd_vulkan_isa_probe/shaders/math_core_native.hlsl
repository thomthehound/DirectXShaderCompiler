#include <vk/amd/math.h>

RWStructuredBuffer<uint> Out : register(u0);

// Keep every operation data-dependent so the Vulkan compiler must lower it.
// The probe only creates the pipeline; it does not execute this shader.
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint seed = tid.x * 0x9e3779b9u + 0x7f4a7c15u;
  int signedSeed = asint(seed ^ 0x81234567u);

  uint ubfe = vk::amd::UBfe(seed, (tid.x & 7u) + 1u, 11u);
  int sbfe = vk::amd::SBfe(signedSeed, (tid.x & 3u) + 2u, 13u);
  uint rev = vk::amd::BitReverse(seed);
  uint bits = vk::amd::BitCount(seed ^ 0xa55aa55au);

  float x = 0.75f + float(seed & 1023u) * (1.0f / 2048.0f);
  float y = vk::amd::Rcp(x + 0.25f);
  y += vk::amd::Sqrt(x + 1.0f);
  y += vk::amd::Rsq(x + 2.0f);
  y += vk::amd::Sin(x) + vk::amd::Cos(x * 0.5f);
  y += vk::amd::Log2(x + 1.0f) + vk::amd::Exp2(x * 0.125f);
  y += vk::amd::Fract(x * 1.75f);
  y += vk::amd::FMed3(x, -x, y);

  Out[tid.x] = ubfe ^ asuint(sbfe) ^ rev ^ bits ^ asuint(y);
}
