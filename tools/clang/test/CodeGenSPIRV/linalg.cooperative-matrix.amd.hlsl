// RUN: %dxc -T cs_6_10 -E main -fcgl -spirv -fspv-target-env=vulkan1.3 %s | FileCheck %s

// This is only the minimal compiler-plumbing smoke test. Component ID 4 is
// I32 in the SM 6.10 LinAlg enum; do not treat this file as FP16/WMMA hardware
// evidence. APUSR-relevant F16/BF16/IU8 shapes live in the separate shape corpus.
//
// CHECK: OpCapability CooperativeMatrixKHR
// CHECK: OpExtension "SPV_KHR_cooperative_matrix"
// CHECK-DAG: OpTypeCooperativeMatrixKHR
// CHECK: OpCooperativeMatrixMulAddKHR

[numthreads(32, 1, 1)]
void main() {
  __builtin_LinAlgMatrix [[__LinAlgMatrix_Attributes(4, 16, 16, 0, 1)]] a;
  __builtin_LinAlgMatrix [[__LinAlgMatrix_Attributes(4, 16, 16, 1, 1)]] b;
  __builtin_LinAlgMatrix [[__LinAlgMatrix_Attributes(4, 16, 16, 2, 1)]] acc;
  __builtin_LinAlg_FillMatrix(a, 1);
  __builtin_LinAlg_FillMatrix(b, 2);
  __builtin_LinAlg_FillMatrix(acc, 3);
  __builtin_LinAlg_MatrixMatrixMultiplyAccumulate(acc, a, b, acc);
}
