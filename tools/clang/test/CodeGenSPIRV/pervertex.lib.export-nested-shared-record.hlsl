// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s 2>&1 | FileCheck %s

// The first dual-layout lowering slice handles flat scalar/vector records.
// Nested shared records must fail explicitly rather than silently mutate the
// Clang AST or produce incompatible pointer/composite types.
// CHECK: error: cannot specialize per-vertex interpolation for a record shared with a non-pixel or exported function

struct InnerData {
  nointerpolation float4 color : TEXCOORD0;
};

struct SharedData {
  InnerData nested;
};

export float4 exportedRead(SharedData value) {
  return value.nested.color;
}

[shader("pixel")]
float4 pixelMain(SharedData input) : SV_Target {
  return GetAttributeAtVertex(input.nested.color, 1);
}
