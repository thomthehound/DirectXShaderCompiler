#include <vk/khr/cooperative_matrix.h>

RWStructuredBuffer<uint> data : register(u0);

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

  // Read a dynamic input so backend compilation has a real matrix operation
  // to lower rather than only constant values.
  Component value = Component((data[0] & 7u) + 1u);
  MatrixA a = MatrixA::Splat(value);
  MatrixB b = MatrixB::Splat(value);
  MatrixC c = MatrixC::Splat(Component(0));
  MatrixC result = vk::khr::cooperativeMatrixMultiplyAdd(a, b, c);

  if (WaveIsFirstLane())
    data[0] = uint(result.Get(0));
}
