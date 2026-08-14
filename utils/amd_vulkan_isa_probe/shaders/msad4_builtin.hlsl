struct ProbeArgs {
  uint reference;
  uint source0;
  uint source1;
  uint pad0;
  uint4 accum;
};

[[vk::push_constant]] ConstantBuffer<ProbeArgs> args;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint4> output_buffer;

[numthreads(1, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint perturb = dispatch_id.x;
  const uint reference = args.reference ^ perturb;
  const uint2 source = uint2(args.source0, args.source1 + perturb);
  output_buffer[dispatch_id.x] = msad4(reference, source, args.accum);
}
