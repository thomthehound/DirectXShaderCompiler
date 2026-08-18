#include <vk/amd/dot.h>

RWStructuredBuffer<uint4> Out : register(u0);

// Keep each packed-dot family independently data-dependent. This shader is only
// used to create a pipeline for Radeon ISA inspection; execution correctness is
// covered by compiler/math semantics and APUSR qualification separately.
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint a = tid.x * 0x45d9f3bu + 0x89abcdefu;
  uint b = tid.x * 0x27d4eb2du + 0x13579bdfu;
  int si = int(tid.x * 31u + 7u);
  uint ui = tid.x * 29u + 11u;

  int dot4s = vk::amd::SDot4I8(a, b, si);
  uint dot4u = vk::amd::UDot4U8(a ^ 0x11111111u, b, ui);

  // Accumulators deliberately live near the output boundaries so the clamp
  // modifier is semantically live and cannot be discarded as redundant.
  int satSAccum = (tid.x & 1u) != 0u ? 0x7ffffff0 : asint(0x80000010u);
  uint satUAccum = 0xfffffff0u - (tid.x & 7u);
  int dot4sSat = vk::amd::SDot4I8AccSat(a ^ 0x80808080u, b, satSAccum);
  uint dot4uSat = vk::amd::UDot4U8AccSat(a, b ^ 0xffffffffu, satUAccum);

  int dot2s = vk::amd::SDot2I16(a ^ 0x22222222u, b, si + 3);
  uint dot2u = vk::amd::UDot2U16(a, b ^ 0x33333333u, ui + 5u);
  int dot4su = vk::amd::SUDot4I8U8(a ^ 0x44444444u, b, si + 7);
  int dot4us = vk::amd::USDot4U8I8(a, b ^ 0x55555555u, si + 9);

  int dot8s = vk::amd::SDot8I4(a ^ 0x66666666u, b, si + 11);
  uint dot8u = vk::amd::UDot8U4(a, b ^ 0x77777777u, ui + 13u);
  int dot8su = vk::amd::SUDot8I4U4(a ^ 0x88888888u, b, si + 15);
  int dot8us = vk::amd::USDot8U4I4(a, b ^ 0x99999999u, si + 17);

  // Mask each half away from exponent-all-ones so the candidate remains finite
  // if it is later promoted into an execution oracle. The source still carries
  // arbitrary sign/mantissa data and remains fully data-dependent.
  uint packedHalfA = a & 0xfbfffbffu;
  uint packedHalfB = b & 0xfbfffbffu;
  float dot2f16 = vk::amd::FDot2F32F16Bits(
      packedHalfA, packedHalfB, float(si) * 0.0009765625f);

  Out[tid.x] = uint4(
      asuint(dot4s) ^ dot4u ^ asuint(dot4sSat) ^ dot4uSat,
      asuint(dot2s) ^ dot2u,
      asuint(dot4su) ^ asuint(dot4us),
      asuint(dot8s) ^ dot8u ^ asuint(dot8su) ^ asuint(dot8us) ^ asuint(dot2f16));
}
