// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s --check-prefix=VALID
// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 -DEXPORTED %s 2>&1 | FileCheck %s --check-prefix=INVALID
// VALID: OpEntryPoint Fragment
// INVALID: error: discard statement may only be used in pixel shaders

void fragmentOnly() { discard; }

#ifdef EXPORTED
export
#endif
float4 fragmentHelper(float4 color) {
  fragmentOnly();
  return color;
}

[shader("pixel")]
float4 pixelMain(float4 color : TEXCOORD0) : SV_Target {
  return fragmentHelper(color);
}
