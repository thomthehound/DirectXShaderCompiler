#include <vk/amd/mixed_dot.h>

#ifndef MODE
#define MODE 0
#endif

RWByteAddressBuffer Data : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint byteBase = tid.x * 8u;
  uint aBits = Data.Load(byteBase + 0u);
  uint bBits = Data.Load(byteBase + 4u);
  uint outOffset = 65536u + tid.x * 4u;

#if MODE == 0
  float accumulator = float(tid.x) * 0.125f + 0.25f;
  float result = vk::amd::Dot2F16PackedAccF32(aBits, bBits, accumulator);
  Data.Store(outOffset, asuint(result));
#elif MODE == 1
  uint16_t accumulatorBits = uint16_t((tid.x * 257u + 0x3c00u) & 0xffffu);
  uint result =
      vk::amd::Dot2F16PackedAccF16Bits(aBits, bBits, accumulatorBits);
  Data.Store(outOffset, result);
#elif MODE == 2
  float accumulator = float(tid.x) * 0.125f + 0.25f;
  float result = vk::amd::Dot2BF16AccF32(aBits, bBits, accumulator);
  Data.Store(outOffset, asuint(result));
#elif MODE == 3
  uint16_t accumulatorBits = uint16_t((tid.x * 257u + 0x3f80u) & 0xffffu);
  uint result =
      vk::amd::Dot2BF16AccBF16Bits(aBits, bBits, accumulatorBits);
  Data.Store(outOffset, result);
#elif MODE == 4
  uint result = vk::amd::Dot2BF16Bits(aBits, bBits);
  Data.Store(outOffset, result);
#else
  #error unsupported mixed-dot probe mode
#endif
}
