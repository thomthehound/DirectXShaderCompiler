// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_DX_AMD_DOT_H_
#define _HLSL_DX_AMD_DOT_H_

namespace dx {
namespace amd {

// These are first-class HLSL/DXIL operations. Do not rebuild them from scalar
// arithmetic: preserving the intrinsic gives the D3D driver the strongest
// possible AMD-native codegen opportunity.
int SDot4I8(uint a, uint b, int accum) {
  return dot4add_i8packed(a, b, accum);
}
uint UDot4U8(uint a, uint b, uint accum) {
  return dot4add_u8packed(a, b, accum);
}
uint4 Msad4(uint reference, uint2 source, uint4 accum) {
  return msad4(reference, source, accum);
}

// The remaining packed-dot shapes do not have equivalent first-class HLSL
// builtins in this compiler surface. Keep exact source semantics so the D3D
// driver can still recognize the pattern; native recovery is a qualification
// result, not an assumption.
int SDot2I16(uint a, uint b, int accum) {
  int a0 = int(a << 16) >> 16;
  int a1 = int(a) >> 16;
  int b0 = int(b << 16) >> 16;
  int b1 = int(b) >> 16;
  return accum + a0 * b0 + a1 * b1;
}
uint UDot2U16(uint a, uint b, uint accum) {
  return accum + (a & 0xffffu) * (b & 0xffffu) +
         (a >> 16u) * (b >> 16u);
}

int SUDot4I8U8(uint signedA, uint unsignedB, int accum) {
  int sum = accum;
  [unroll]
  for (uint i = 0u; i < 4u; ++i) {
    uint shift = i * 8u;
    int sa = int(signedA << (24u - shift)) >> 24;
    uint ub = (unsignedB >> shift) & 0xffu;
    sum += sa * int(ub);
  }
  return sum;
}

int USDot4U8I8(uint unsignedA, uint signedB, int accum) {
  int sum = accum;
  [unroll]
  for (uint i = 0u; i < 4u; ++i) {
    uint shift = i * 8u;
    uint ua = (unsignedA >> shift) & 0xffu;
    int sb = int(signedB << (24u - shift)) >> 24;
    sum += int(ua) * sb;
  }
  return sum;
}

} // namespace amd
} // namespace dx

#endif // _HLSL_DX_AMD_DOT_H_
