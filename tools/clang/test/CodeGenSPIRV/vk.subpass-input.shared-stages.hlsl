// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY %s | FileCheck %s --check-prefix=VALID
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY -DVERTEX_FIRST %s | FileCheck %s --check-prefix=VALID
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY -DUNREACHABLE %s | FileCheck %s --check-prefix=VALID
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DUNREACHABLE_VERTEX_LOAD %s | FileCheck %s --check-prefix=VALID
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DUNREACHABLE_VERTEX_LOAD -DVERTEX_FIRST %s | FileCheck %s --check-prefix=VALID
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

// The function executes on a vertex path, but its SubpassLoad belongs to a
// disconnected block omitted by SpirvFunction::invokeVisitor.
float4 deadTileRead() {
  return 0.0;
  return tile.SubpassLoad();
}

#ifdef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
#ifdef UNREACHABLE_VERTEX_LOAD
  return pos + deadTileRead();
#elif !defined(PIXEL_ONLY)
  return pos + readTile();
#else
  return pos;
#endif
}
#endif

[shader("pixel")]
float4 pixelMain() : SV_Target {
#ifdef UNREACHABLE
  return 0.0;
#endif
  return readTile();
}

#ifndef VERTEX_FIRST
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
#ifdef UNREACHABLE_VERTEX_LOAD
  return pos + deadTileRead();
#elif !defined(PIXEL_ONLY)
  return pos + readTile();
#else
  return pos;
#endif
}
#endif
