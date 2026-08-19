// ByteAddressBuffer vector-memory contract probe.
// Offsets are 4-byte aligned but intentionally not 16-byte aligned.

[[vk::binding(0, 0)]] ByteAddressBuffer input_buffer;
[[vk::binding(1, 0)]] RWByteAddressBuffer output_buffer;

[numthreads(1, 1, 1)]
void main() {
  uint4 words = input_buffer.Load4(4u);
  float4 values = input_buffer.Load<float4>(20u);

  output_buffer.Store4(4u, words);
  output_buffer.Store(20u, values);
}
