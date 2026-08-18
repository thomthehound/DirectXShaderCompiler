// RUN: %dxc -T vs_6_4 -E main -fcgl -spirv %s | FileCheck %s
// RUN: %dxc -T vs_6_4 -E main -fcgl -spirv -fspv-require-native-intrinsics %s | FileCheck %s

// CHECK: OpCapability DotProduct
// CHECK: OpCapability DotProductInput4x8BitPacked
// CHECK: OpExtension "SPV_KHR_integer_dot_product"
// CHECK-COUNT-1: OpSDot
// CHECK-COUNT-1: OpUDot
// CHECK-NOT: OpIMul

int2 main(uint a : PACKED_A, uint b : PACKED_B, int signedAccum : SIGNED_ACCUM,
          uint unsignedAccum : UNSIGNED_ACCUM) : DOT_RESULT {
  int signedResult = dot4add_i8packed(a, b, signedAccum);
  uint unsignedResult = dot4add_u8packed(a, b, unsignedAccum);
  return int2(signedResult, (int)unsignedResult);
}
