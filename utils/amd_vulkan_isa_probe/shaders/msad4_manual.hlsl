struct ProbeArgs {
  uint reference;
  uint source0;
  uint source1;
  uint pad0;
  uint4 accum;
};

[[vk::push_constant]] ConstantBuffer<ProbeArgs> args;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint4> output_buffer;

uint byte_at(uint value, uint index) {
  return (value >> (index * 8u)) & 0xffu;
}

uint absdiff_u8(uint a, uint b) {
  return a >= b ? a - b : b - a;
}

uint masked_diff(uint ref_byte, uint src_byte) {
  return ref_byte == 0u ? 0u : absdiff_u8(ref_byte, src_byte);
}

uint source_byte(uint source_lo, uint source_hi, uint index) {
  return index < 4u ? byte_at(source_lo, index) : byte_at(source_hi, index - 4u);
}

uint msad_lane(uint reference, uint source_lo, uint source_hi, uint lane,
               uint accum) {
  uint sum = accum;
  [unroll]
  for (uint i = 0; i < 4u; ++i) {
    const uint ref_byte = byte_at(reference, i);
    const uint src_byte = source_byte(source_lo, source_hi, lane + i);
    sum += masked_diff(ref_byte, src_byte);
  }
  return sum;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint perturb = dispatch_id.x;
  const uint reference = args.reference ^ perturb;
  const uint source0 = args.source0;
  const uint source1 = args.source1 + perturb;
  uint4 result;
  result.x = msad_lane(reference, source0, source1, 0u, args.accum.x);
  result.y = msad_lane(reference, source0, source1, 1u, args.accum.y);
  result.z = msad_lane(reference, source0, source1, 2u, args.accum.z);
  result.w = msad_lane(reference, source0, source1, 3u, args.accum.w);
  output_buffer[dispatch_id.x] = result;
}
