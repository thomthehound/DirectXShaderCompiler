// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv %s 2>&1 | FileCheck %s --check-prefix=SHARED
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -DPIXEL_ONLY %s | FileCheck %s --check-prefix=VALID
// SHARED: error: discard statement may only be used in pixel shaders
// VALID: OpEntryPoint Fragment

void fragmentOnly() { discard; }
uint globalInit() {
  fragmentOnly();
  return 1;
}
static uint initializedByCall = globalInit();

[shader("pixel")]
float4 pixelMain(float4 input : TEXCOORD0) : SV_Target {
  return input * initializedByCall;
}

#ifndef PIXEL_ONLY
[shader("vertex")]
float4 vertexMain(float4 pos : POSITION) : SV_Position {
  return pos * initializedByCall;
}
#endif
