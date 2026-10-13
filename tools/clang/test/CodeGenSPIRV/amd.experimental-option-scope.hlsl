// RUN: not %dxc -T vs_6_0 -E main -fcgl -fspv-enable-amd-intrinsics %s 2>&1 | FileCheck %s --check-prefix=DXIL-EXPERIMENTAL
// RUN: not %dxc -T vs_6_0 -E main -fcgl -fspv-require-native-intrinsics %s 2>&1 | FileCheck %s --check-prefix=DXIL-STRICT
// RUN: %dxc -T vs_6_0 -E main -fcgl -spirv -fspv-enable-amd-intrinsics %s | FileCheck %s --check-prefix=SPIRV
// RUN: %dxc -T vs_6_0 -E main -fcgl -spirv -fspv-require-native-intrinsics %s | FileCheck %s --check-prefix=SPIRV

// DXIL-EXPERIMENTAL: -fspv-enable-amd-intrinsics requires -spirv output
// DXIL-STRICT: -fspv-require-native-intrinsics requires -spirv output
// SPIRV: OpEntryPoint Vertex

float4 main(uint index : SV_VertexID) : SV_Position {
  return float4(float(index), 0.0f, 0.0f, 1.0f);
}
