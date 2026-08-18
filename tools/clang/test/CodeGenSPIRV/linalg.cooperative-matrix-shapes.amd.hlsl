// RUN: %dxc -T cs_6_10 -E main -fcgl -spirv -fspv-target-env=vulkan1.3 -DMODE=0 %s | FileCheck %s --check-prefix=COMMON
// RUN: %dxc -T cs_6_10 -E main -fcgl -spirv -fspv-target-env=vulkan1.3 -DMODE=2 %s | FileCheck %s --check-prefix=COMMON
// RUN: %dxc -T cs_6_10 -E main -fcgl -spirv -fspv-target-env=vulkan1.3 -DMODE=4 %s | FileCheck %s --check-prefix=COMMON

// COMMON: OpCapability CooperativeMatrixKHR
// COMMON: OpExtension "SPV_KHR_cooperative_matrix"
// COMMON: OpTypeCooperativeMatrixKHR
// COMMON: OpCooperativeMatrixLoadKHR
// COMMON: OpCooperativeMatrixMulAddKHR
// COMMON: OpCooperativeMatrixStoreKHR

#ifndef MODE
#define MODE 0
#endif

// SM 6.10 / dx::linalg component IDs in this fork:
// F16=8, F32=9, I8=19, U8=20, BFloat16=23.
// Every case deliberately uses the same 16x16x16 wave-scoped shape because
// gfx11 exposes native WMMA forms for F16, BF16 and IU8 at this shape.
#if MODE == 0
  #define ACOMP 8
  #define BCOMP 8
  #define CCOMP 9
  #define A_STRIDE 32
  #define B_STRIDE 32
  #define C_STRIDE 64
#elif MODE == 1
  #define ACOMP 8
  #define BCOMP 8
  #define CCOMP 8
  #define A_STRIDE 32
  #define B_STRIDE 32
  #define C_STRIDE 32
#elif MODE == 2
  #define ACOMP 23
  #define BCOMP 23
  #define CCOMP 9
  #define A_STRIDE 32
  #define B_STRIDE 32
  #define C_STRIDE 64
#elif MODE == 3
  #define ACOMP 23
  #define BCOMP 23
  #define CCOMP 23
  #define A_STRIDE 32
  #define B_STRIDE 32
  #define C_STRIDE 32
#elif MODE == 4
  #define ACOMP 19
  #define BCOMP 19
  #define CCOMP 4
  #define A_STRIDE 16
  #define B_STRIDE 16
  #define C_STRIDE 64
#elif MODE == 5
  #define ACOMP 20
  #define BCOMP 20
  #define CCOMP 5
  #define A_STRIDE 16
  #define B_STRIDE 16
  #define C_STRIDE 64
#elif MODE == 6
  #define ACOMP 19
  #define BCOMP 20
  #define CCOMP 4
  #define A_STRIDE 16
  #define B_STRIDE 16
  #define C_STRIDE 64
#elif MODE == 7
  #define ACOMP 20
  #define BCOMP 19
  #define CCOMP 4
  #define A_STRIDE 16
  #define B_STRIDE 16
  #define C_STRIDE 64
#else
  #error unsupported cooperative-matrix qualification mode
#endif

RWByteAddressBuffer Data : register(u0);

[numthreads(32, 1, 1)]
void main() {
  __builtin_LinAlgMatrix [[__LinAlgMatrix_Attributes(ACOMP, 16, 16, 0, 1)]] a;
  __builtin_LinAlgMatrix [[__LinAlgMatrix_Attributes(BCOMP, 16, 16, 1, 1)]] b;
  __builtin_LinAlgMatrix [[__LinAlgMatrix_Attributes(CCOMP, 16, 16, 2, 1)]] acc;

  __builtin_LinAlg_MatrixLoadFromDescriptor(a, Data, 0, A_STRIDE, 0, 128);
  __builtin_LinAlg_MatrixLoadFromDescriptor(b, Data, 4096, B_STRIDE, 0, 128);
  __builtin_LinAlg_MatrixLoadFromDescriptor(acc, Data, 8192, C_STRIDE, 0, 128);
  __builtin_LinAlg_MatrixMatrixMultiplyAccumulate(acc, a, b, acc);
  __builtin_LinAlg_MatrixStoreToDescriptor(acc, Data, 12288, C_STRIDE, 0, 128);
}
