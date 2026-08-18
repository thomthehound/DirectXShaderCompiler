#include <vk/amd/bfloat16.h>

#ifndef MODE
#define MODE 0
#endif

RWByteAddressBuffer Data : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint byteBase = tid.x * 8u;
  uint outBase = 65536u + tid.x * 8u;

#if MODE == 0
  float2 value = asfloat(Data.Load2(byteBase));
  uint packed = vk::amd::F32x2ToBF16x2Bits(value);
  Data.Store(outBase, packed);
#elif MODE == 1
  uint packed = Data.Load(byteBase);
  float2 value = vk::amd::BF16x2BitsToF32x2(packed);
  Data.Store2(outBase, asuint(value));
#else
  #error unsupported BF16 conversion probe mode
#endif
}
