#include <vk/amd/dot.h>

RWStructuredBuffer<uint4> Out : register(u0);

#ifndef APUSR_DOT_CANDIDATE
#define APUSR_DOT_CANDIDATE 0
#endif

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x45d9f3bu + 0x89abcdefu;
  uint b = tid.x * 0x27d4eb2du + 0x13579bdfu;
  int si = int(tid.x * 31u + 7u);
  uint ui = tid.x * 29u + 11u;
  int satSAccum = (tid.x & 1u) != 0u ? 0x7ffffff0 : asint(0x80000010u);
  uint satUAccum = 0xfffffff0u - (tid.x & 7u);
  uint r = 0u;

#if APUSR_DOT_CANDIDATE == 0
  r = asuint(vk::amd::SDot4I8(a, b, si));
#elif APUSR_DOT_CANDIDATE == 1
  r = vk::amd::UDot4U8(a ^ 0x11111111u, b, ui);
#elif APUSR_DOT_CANDIDATE == 2
  r = asuint(vk::amd::SDot4I8AccSat(a ^ 0x80808080u, b, satSAccum));
#elif APUSR_DOT_CANDIDATE == 3
  r = vk::amd::UDot4U8AccSat(a, b ^ 0xffffffffu, satUAccum);
#elif APUSR_DOT_CANDIDATE == 4
  r = asuint(vk::amd::SUDot4I8U8(a ^ 0x44444444u, b, si + 7));
#elif APUSR_DOT_CANDIDATE == 5
  r = asuint(vk::amd::USDot4U8I8(a, b ^ 0x55555555u, si + 9));
#elif APUSR_DOT_CANDIDATE == 6
  r = asuint(vk::amd::SUDot4I8U8AccSat(
      a ^ 0x80808080u, b, satSAccum));
#elif APUSR_DOT_CANDIDATE == 7
  r = asuint(vk::amd::USDot4U8I8AccSat(
      a, b ^ 0x80808080u, satSAccum));
#elif APUSR_DOT_CANDIDATE == 8
  r = asuint(vk::amd::SDot2I16(a ^ 0x22222222u, b, si + 3));
#elif APUSR_DOT_CANDIDATE == 9
  r = vk::amd::UDot2U16(a, b ^ 0x33333333u, ui + 5u);
#elif APUSR_DOT_CANDIDATE == 10
  r = asuint(vk::amd::SDot8I4(a ^ 0x66666666u, b, si + 11));
#elif APUSR_DOT_CANDIDATE == 11
  r = vk::amd::UDot8U4(a, b ^ 0x77777777u, ui + 13u);
#elif APUSR_DOT_CANDIDATE == 12
  r = asuint(vk::amd::SUDot8I4U4(a ^ 0x88888888u, b, si + 15));
#elif APUSR_DOT_CANDIDATE == 13
  r = asuint(vk::amd::USDot8U4I4(a, b ^ 0x99999999u, si + 17));
#elif APUSR_DOT_CANDIDATE == 14
  // Packed half bits match resource/tensor boundary storage without requiring
  // first-class Float16 SPIR-V arithmetic in the probe device.
  uint packedHalfA = a & 0xfbfffbffu;
  uint packedHalfB = b & 0xfbfffbffu;
  r = asuint(vk::amd::FDot2F32F16Bits(
      packedHalfA, packedHalfB, float(si) * 0.0009765625f));
#else
#error Unknown APUSR_DOT_CANDIDATE
#endif

  Out[tid.x] = uint4(r, r ^ a, r + b, r ^ (a + b));
}
