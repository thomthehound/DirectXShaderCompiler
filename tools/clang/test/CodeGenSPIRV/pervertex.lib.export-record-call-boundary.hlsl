// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s 2>&1 | FileCheck %s

// An expanded input record cannot be passed by pointer to an export that
// requires the ordinary ABI without explicitly constructing its ordinary
// value. The first dual-layout pass diagnoses this boundary.
// CHECK: error: cannot pass a per-vertex record to a non-pixel or exported function without record conversion

struct SharedData {
  nointerpolation float4 color : TEXCOORD0;
};

export float4 exportedRead(SharedData value) {
  return value.color;
}

[shader("pixel")]
float4 pixelMain(SharedData input) : SV_Target {
  return exportedRead(input) + GetAttributeAtVertex(input.color, 1);
}
