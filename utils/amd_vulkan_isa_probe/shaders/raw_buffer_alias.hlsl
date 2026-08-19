// ByteAddressBuffer direct-alias contract probe.
// Exercises local resource aliases and function parameters without resource
// arrays or aggregate resource containers.

[[vk::binding(0, 0)]] ByteAddressBuffer input_buffer;
[[vk::binding(1, 0)]] RWByteAddressBuffer output_buffer;

uint4 load_alias(ByteAddressBuffer source) {
  return source.Load4(4u);
}

void store_alias(RWByteAddressBuffer target, uint4 value) {
  target.Store4(20u, value);
}

[numthreads(1, 1, 1)]
void main() {
  ByteAddressBuffer local_input = input_buffer;
  RWByteAddressBuffer local_output = output_buffer;
  store_alias(local_output, load_alias(local_input));
}
