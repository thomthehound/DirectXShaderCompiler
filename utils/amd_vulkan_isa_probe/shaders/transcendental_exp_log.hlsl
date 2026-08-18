#include <vk/amd/math.h>

RWStructuredBuffer<uint4> Out : register(u0);

// Candidate spelling only. Inputs are deliberately restricted to a finite,
// positive range where the identities are mathematically valid. Numerical
// equivalence is qualified separately before any APUSR consumer can use it.
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float x = 0.5f + float(tid.x & 63u) * (1.0f / 64.0f);
  float e = 0.75f + float(tid.x & 7u) * (1.0f / 16.0f);
  float t = x - 1.0f;

  float exp2t = vk::amd::Exp(2.0f * t);
  float a = (exp2t - 1.0f) / (exp2t + 1.0f);
  float b = vk::amd::Exp(vk::amd::Log(x) * e);
  float exponent;
  float mantissa = vk::amd::Frexp(x, exponent);
  float c = vk::amd::Ldexp(mantissa, exponent);
  Out[tid.x] = uint4(asuint(a), asuint(b), asuint(c), asuint(a + b + c));
}
