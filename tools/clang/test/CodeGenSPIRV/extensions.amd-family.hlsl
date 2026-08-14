// RUN: %dxc -T vs_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

// Verify the AMD family alias permits a registered AMD extinst and that the
// capability pass infers the module extension from the instruction set.
// CHECK: OpExtension "SPV_AMD_shader_trinary_minmax"
// CHECK: [[SET:%[0-9]+]] = OpExtInstImport "SPV_AMD_shader_trinary_minmax"
// CHECK: OpExtInst %float [[SET]] FMin3AMD

template <typename T>
[[vk::ext_instruction(1, "SPV_AMD_shader_trinary_minmax")]]
T FMin3AMD(T x, T y, T z);

float4 main(float4 x : POSITION) : SV_Position {
  x.x = FMin3AMD(x.x, x.y, x.z);
  return x;
}
