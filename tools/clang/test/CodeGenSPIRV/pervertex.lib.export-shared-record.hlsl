// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s 2>&1 | FileCheck %s

// A shared Clang record declaration must not have its FieldDecl types changed
// by pixel-only per-vertex lowering when an unrelated linkage export uses it.
// Until record representations can be specialized without mutating the shared
// AST, diagnose this conflict rather than emitting a broken exported ABI.
// CHECK: error: cannot specialize per-vertex interpolation for a record shared with a non-pixel or exported function

struct SharedData {
  nointerpolation float4 color : TEXCOORD0;
};

export float4 exportedRead(SharedData value) {
  return value.color;
}

[shader("pixel")]
float4 pixelMain(SharedData input) : SV_Target {
  return GetAttributeAtVertex(input.color, 1);
}
