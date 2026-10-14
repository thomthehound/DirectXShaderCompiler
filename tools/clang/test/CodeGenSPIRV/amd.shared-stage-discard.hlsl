// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY %s | FileCheck %s --check-prefix=VALID
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY -DVERTEX_FIRST %s | FileCheck %s --check-prefix=VALID
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY -DUNREACHABLE %s | FileCheck %s --check-prefix=VALID
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv %s 2>&1 | FileCheck %s --check-prefix=SHARED
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -DVERTEX_FIRST %s 2>&1 | FileCheck %s --check-prefix=SHARED

// The emitter translates helpers once and assigns an ambient stage from the
// first caller. A helper containing discard is valid only when every calling
// entry is a pixel shader, regardless of declaration/call order.
// CHECK-DAG allows either module entry-point ordering. The two RUN lines
// intentionally reverse source declaration order; both stages must survive.
// VALID-DAG: OpEntryPoint Fragment
// VALID-DAG: OpEntryPoint Vertex
// SHARED: error: discard statement may only be used in pixel shaders

void fragmentOnly() { discard; }

#ifdef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
#ifndef PIXEL_ONLY
  fragmentOnly();
#endif
  return pos;
}
#endif

[shader("pixel")]
float4 pixelMain(float4 color : TEXCOORD0) : SV_Target {
#ifdef UNREACHABLE
  return color;
#endif
  fragmentOnly();
  return color;
}

#ifndef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
#ifndef PIXEL_ONLY
  fragmentOnly();
#endif
  return pos;
}
#endif
