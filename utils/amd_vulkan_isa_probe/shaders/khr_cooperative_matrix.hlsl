#include <vk/khr/cooperative_matrix.h>

RWStructuredBuffer<int> data : register(u0);

#if CONTROL_F16
using Component = float16_t;
#else
using Component = int;
#endif

[numthreads(64, 1, 1)]
void main() {
  using MatrixA = vk::khr::CooperativeMatrixA<Component, vk::ScopeSubgroup, 16, 16>;
  using MatrixB = vk::khr::CooperativeMatrixB<Component, vk::ScopeSubgroup, 16, 16>;
  using MatrixC =
      vk::khr::CooperativeMatrixAccumulator<Component, vk::ScopeSubgroup, 16, 16>;

#if CONTROL_F16
  // Keep a dynamic FP16 multiply-add live without requiring 16-bit storage.
  Component value = Component((data[0] & 7) + 1);
  MatrixA a = MatrixA::Splat(value);
  MatrixB b = MatrixB::Splat(value);
  MatrixC c = MatrixC::Splat(Component(0));
  MatrixC result = vk::khr::cooperativeMatrixMultiplyAdd(a, b, c);
  if (WaveIsFirstLane())
    data[0] = int(result.Get(0));
#else
  // Exercise the upstream structured-buffer pointer lowering independently
  // of the unfinished dx::linalg ByteAddressBuffer pointer bridge.
  MatrixA a = MatrixA::Load<vk::CooperativeMatrixLayoutRowMajorKHR>(data, 0, 64);
  MatrixB b = MatrixB::Load<vk::CooperativeMatrixLayoutRowMajorKHR>(data, 256, 64);
  MatrixC c = MatrixC::Load<vk::CooperativeMatrixLayoutRowMajorKHR>(data, 512, 64);
  MatrixC result = vk::khr::cooperativeMatrixMultiplyAdd(a, b, c);
  result.Store<vk::CooperativeMatrixLayoutRowMajorKHR>(data, 768, 64);
#endif
}
