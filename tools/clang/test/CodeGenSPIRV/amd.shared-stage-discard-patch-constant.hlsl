// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv %s 2>&1 | FileCheck %s --check-prefix=SHARED
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -DHULL_FIRST %s 2>&1 | FileCheck %s --check-prefix=SHARED
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DNO_PATCH_CALL %s | FileCheck %s --check-prefix=VALID
// SHARED: error: discard statement may only be used in pixel shaders
// VALID: OpEntryPoint Fragment
// VALID: OpEntryPoint TessellationControl

struct ControlPoint {
  float4 value : TEXCOORD0;
};
struct PatchConstants {
  float edges[3] : SV_TessFactor;
  float inside : SV_InsideTessFactor;
};

void fragmentOnly() { discard; }

PatchConstants calculatePatchConstants() {
#ifndef NO_PATCH_CALL
  fragmentOnly();
#endif
  PatchConstants result;
  result.edges[0] = 1.0;
  result.edges[1] = 1.0;
  result.edges[2] = 1.0;
  result.inside = 1.0;
  return result;
}

#ifdef HULL_FIRST
[shader("hull")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("calculatePatchConstants")]
ControlPoint hullMain(InputPatch<ControlPoint, 3> input,
                      uint i : SV_OutputControlPointID) {
  return input[i];
}
#endif

[shader("pixel")]
float4 pixelMain(float4 color : TEXCOORD0) : SV_Target {
  fragmentOnly();
  return color;
}

#ifndef HULL_FIRST
[shader("hull")]
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("calculatePatchConstants")]
ControlPoint hullMain(InputPatch<ControlPoint, 3> input,
                      uint i : SV_OutputControlPointID) {
  return input[i];
}
#endif
