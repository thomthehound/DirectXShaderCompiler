// APUSR optical-flow fast-path arithmetic shape: 8 rows x 2 accumulated
// msad4 calls. Inputs are evolved from push constants so this remains
// compatible with amd_vulkan_isa_probe's one-storage-binding pipeline layout.

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
    sad = msad4(p0, uint2(a0, a1), sad);
    sad = msad4(p1, uint2(a1, a2), sad);

    a0 = (a0 >> 1) | (a0 << 31);
    a1 ^= 0x9e3779b9u;
    a2 += 0x7f4a7c15u;
    p0 ^= 0x01020408u;
    p1 += 0x10204081u;
  }

  output_buffer[dispatch_id.x] = sad;
}
