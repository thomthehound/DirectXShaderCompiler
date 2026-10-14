// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY %s | FileCheck %s --check-prefix=VALID
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY -DVERTEX_FIRST %s | FileCheck %s --check-prefix=VALID
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv %s 2>&1 | FileCheck %s --check-prefix=INVALID
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -DVERTEX_FIRST %s 2>&1 | FileCheck %s --check-prefix=INVALID
// VALID-DAG: OpEntryPoint Fragment
// VALID-DAG: OpEntryPoint Vertex
// INVALID: error: SubpassInput(MS) only allowed in pixel shader

// The image read is emitted in one shared helper. Shader-stage validation
// must follow its actual callers, not inherit the first caller's stage.
[[vk::input_attachment_index(0)]] SubpassInput<float4> tile;

float4 readTile() {
  return tile.SubpassLoad();
}

#ifdef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
#ifndef PIXEL_ONLY
  return pos + readTile();
#else
  return pos;
#endif
}
#endif

[shader("pixel")]
float4 pixelMain() : SV_Target {
  return readTile();
}

#ifndef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
#ifndef PIXEL_ONLY
  return pos + readTile();
#else
  return pos;
#endif
}
#endif
