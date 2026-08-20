ByteAddressBuffer input_buffer : register(t0);
RWByteAddressBuffer output_buffer : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  ByteAddressBuffer local_input = input_buffer;
  RWByteAddressBuffer local_output = output_buffer;
  uint byte_offset = tid.x * 16;
  uint value = local_input.Load4(byte_offset).x;
  local_output.Store4(
      byte_offset, uint4(value, value + 1, value + 2, value + 3));
}
