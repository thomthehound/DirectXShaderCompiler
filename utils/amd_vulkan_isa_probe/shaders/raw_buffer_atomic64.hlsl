// RWByteAddressBuffer native 64-bit atomic contract probe.
// The SPIR-V path must use genuine 64-bit atomics through untyped pointers;
// splitting an operation into 32-bit atomics is not a valid implementation.

[[vk::binding(0, 0)]] RWByteAddressBuffer output_buffer;

[numthreads(1, 1, 1)]
void main() {
  uint64_t old_add;
  output_buffer.InterlockedAdd64(0u, uint64_t(1), old_add);

  int64_t old_min;
  output_buffer.InterlockedMin64(8u, int64_t(-7), old_min);

  uint64_t old_umin;
  output_buffer.InterlockedMin64(16u, uint64_t(9), old_umin);

  uint64_t old_cmp;
  output_buffer.InterlockedCompareExchange64(
      24u, uint64_t(3), uint64_t(4), old_cmp);
}
