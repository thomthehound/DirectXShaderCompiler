groupshared uint4 Shared[64];
RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 ltid : SV_GroupThreadID) {
  uint seed = tid.x * 0x9e3779b9u + 1u;
  Shared[ltid.x] = uint4(seed + 0u, seed + 1u, seed + 2u, seed + 3u);
  GroupMemoryBarrierWithGroupSync();
  Out[tid.x] = Shared[ltid.x];
}
