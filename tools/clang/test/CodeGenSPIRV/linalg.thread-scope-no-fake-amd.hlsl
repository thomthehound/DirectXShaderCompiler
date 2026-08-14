// RUN: not %dxc -T cs_6_10 -E main -fcgl -spirv -fspv-target-env=vulkan1.3 %s 2>&1 | FileCheck %s

// CHECK: error: dx::linalg thread-scope matrices require cooperative-vector semantics, but this SPIR-V target has no KHR/AMD cooperative-vector contract; refusing to map AMD execution to a vendor-incompatible extension

[numthreads(1, 1, 1)]
void main() {
  __builtin_LinAlgMatrix [[__LinAlgMatrix_Attributes(9, 16, 16, 0, 0)]] a;
  __builtin_LinAlg_FillMatrix(a, 1.0f);
}
