// ByteAddressBuffer untyped-resource surface contract probe.
// Exercises GetDimensions and an existing 32-bit raw atomic through the same
// resource representation used by vector raw accesses.

[[vk::binding(0, 0)]] ByteAddressBuffer input_buffer;
[[vk::binding(1, 0)]] RWByteAddressBuffer output_buffer;

[numthreads(1, 1, 1)]
void main() {
  uint input_bytes;
  input_buffer.GetDimensions(input_bytes);

  uint original;
  output_buffer.InterlockedAdd(36u, input_bytes, original);
  output_buffer.Store(40u, original);
}
