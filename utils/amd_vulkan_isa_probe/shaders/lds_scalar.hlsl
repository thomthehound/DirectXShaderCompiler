groupshared uint Shared[256];
RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 ltid : SV_GroupThreadID) {
  uint base = ltid.x * 4u;
  uint seed = tid.x * 0x9e3779b9u + 1u;
  Shared[base + 0u] = seed + 0u;
  Shared[base + 1u] = seed + 1u;
  Shared[base + 2u] = seed + 2u;
  Shared[base + 3u] = seed + 3u;
  GroupMemoryBarrierWithGroupSync();
  Out[tid.x] = uint4(Shared[base + 0u], Shared[base + 1u],
                     Shared[base + 2u], Shared[base + 3u]);
}
