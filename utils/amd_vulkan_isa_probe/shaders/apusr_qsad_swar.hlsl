// Exact arithmetic shape of APUSR's current Vulkan optical-flow fallback:
// 8 rows x 2 rolling QSad calls, SWAR byte absolute difference, packed UDOT.
// Inputs use the same probe-compatible push-constant evolution as the msad4
// comparison shader.

struct ProbeArgs {
  uint source0;
  uint source1;
  uint source2;
  uint reference0;
  uint reference1;
  uint salt;
  uint pad0;
  uint pad1;
};

[[vk::push_constant]] ConstantBuffer<ProbeArgs> args;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint4> output_buffer;

uint packed_byte_absdiff(uint a, uint b) {
  const uint high = 0x80808080u;
  const uint low = 0x7f7f7f7fu;
  const uint xor_value = a ^ b;
  const uint a_high = a & high;
  const uint high_diff = xor_value & high;
  const uint low_ge = ((((a & low) | high) - (b & low)) & high);
  const uint ge_bit = low_ge ^ (high_diff & (low_ge ^ a_high));
  const uint ge_mask = (ge_bit >> 7) * 0xffu;
  const uint swap = xor_value & ge_mask;
  const uint max_value = b ^ swap;
  const uint min_value = a ^ swap;
  return max_value - min_value;
}

uint packed_sad(uint a, uint b) {
  return dot4add_u8packed(packed_byte_absdiff(a, b), 0x01010101u, 0u);
}

uint4 qsad(uint a0, uint a1, uint b) {
  uint4 sad;
  sad.x = packed_sad(a0, b);

  a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
  a1 >>= 8;
  sad.y = packed_sad(a0, b);

  a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
  a1 >>= 8;
  sad.z = packed_sad(a0, b);

  a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
  sad.w = packed_sad(a0, b);
  return sad;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint perturb = dispatch_id.x ^ args.salt;
  uint a0 = args.source0 ^ perturb;
  uint a1 = args.source1 + perturb;
  uint a2 = args.source2 ^ (perturb << 8);
  uint p0 = args.reference0 ^ (perturb << 16);
  uint p1 = args.reference1 + (perturb << 24);
  uint4 sad = uint4(0, 0, 0, 0);

  [unroll]
  for (uint dy = 0; dy < 8; ++dy) {
    sad += qsad(a0, a1, p0);
    sad += qsad(a1, a2, p1);

    a0 = (a0 >> 1) | (a0 << 31);
    a1 ^= 0x9e3779b9u;
    a2 += 0x7f4a7c15u;
    p0 ^= 0x01020408u;
    p1 += 0x10204081u;
  }

  output_buffer[dispatch_id.x] = sad;
}
