// RUN: %dxc -T vs_6_0 -E main -fcgl -spirv -fspv-enable-amd-intrinsics %s | FileCheck %s --check-prefix=AMD
// RUN: not %dxc -T vs_6_0 -E main -fcgl -spirv -fspv-require-native-intrinsics %s 2>&1 | FileCheck %s --check-prefix=STRICT

// AMD: OpCapability DotProduct
// AMD: OpCapability DotProductInput4x8BitPacked
// AMD: OpExtension "SPV_KHR_integer_dot_product"
// AMD-COUNT-4: OpUDot
// AMD-NOT: SAbs

// STRICT: error: msad4 has a native AMD mapping to v_mqsad_u32_u8, but Vulkan SPIR-V exposes no core/KHR/AMD instruction with equivalent semantics

uint4 main(uint reference : REF, uint2 source : SOURCE, uint4 accum : ACCUM)
    : MSAD_RESULT {
  return msad4(reference, source, accum);
}
