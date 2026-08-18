// Six compile modes cover the non-pull-model AGS barycentric modes.
//
// MODE 0: perspective center
// MODE 1: perspective centroid
// MODE 2: perspective sample
// MODE 3: linear/noperspective center
// MODE 4: linear/noperspective centroid
// MODE 5: linear/noperspective sample

#ifndef MODE
#define MODE 0
#endif

struct PSInput {
#if MODE == 0
  linear float3 bary : SV_Barycentrics;
#elif MODE == 1
  centroid float3 bary : SV_Barycentrics;
#elif MODE == 2
  sample float3 bary : SV_Barycentrics;
#elif MODE == 3
  noperspective float3 bary : SV_Barycentrics;
#elif MODE == 4
  centroid noperspective float3 bary : SV_Barycentrics;
#elif MODE == 5
  sample noperspective float3 bary : SV_Barycentrics;
#else
#error Unsupported MODE
#endif
};

float4 main(PSInput input) : SV_Target0 {
  return float4(input.bary, input.bary.x + input.bary.y + input.bary.z);
}
