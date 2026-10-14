// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv %s | FileCheck %s --check-prefix=VALID
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DVERTEX_FIRST %s | FileCheck %s --check-prefix=VALID
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -DREAL_VERTEX_CALL %s 2>&1 | FileCheck %s --check-prefix=INVALID
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -DREAL_VERTEX_CALL -DVERTEX_FIRST %s 2>&1 | FileCheck %s --check-prefix=INVALID
// VALID: OpEntryPoint
// INVALID: error: discard statement may only be used in pixel shaders

// sizeof(expression) checks the type but does not execute its expression.
// A vertex shader may mention this function unevaluated without gaining a
// call edge to the fragment-only discard inside it.
float discardAndReturn() {
  discard;
  return 1.0;
}

#ifdef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
#ifdef REAL_VERTEX_CALL
  return pos * discardAndReturn();
#else
  return pos * sizeof(discardAndReturn());
#endif
}
#endif

[shader("pixel")]
float4 pixelMain(float4 color : TEXCOORD0) : SV_Target {
  return color * discardAndReturn();
}

#ifndef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
#ifdef REAL_VERTEX_CALL
  return pos * discardAndReturn();
#else
  return pos * sizeof(discardAndReturn());
#endif
}
#endif
