// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv %s | FileCheck %s --check-prefix=VALID
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -DSHARED %s 2>&1 | FileCheck %s --check-prefix=INVALID
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -DSHARED -DVERTEX_FIRST %s 2>&1 | FileCheck %s --check-prefix=INVALID
// VALID: OpExtension "SPV_KHR_fragment_shader_barycentric"
// INVALID: error: GetAttributeAtVertex only allowed in pixel shader

float4 readVertex(nointerpolation float4 value) {
  return GetAttributeAtVertex(value, 1);
}

#ifdef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(nointerpolation float4 pos : POSITION) : SV_Position {
#ifdef SHARED
  return readVertex(pos);
#else
  return pos;
#endif
}
#endif

[shader("pixel")]
float4 pixelMain(nointerpolation float4 input : TEXCOORD0) : SV_Target {
  return GetAttributeAtVertex(input, 1);
}

#ifndef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(nointerpolation float4 pos : POSITION) : SV_Position {
#ifdef SHARED
  return readVertex(pos);
#else
  return pos;
#endif
}
#endif
