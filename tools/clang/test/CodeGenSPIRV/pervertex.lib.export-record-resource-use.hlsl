// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s 2>&1 | FileCheck %s

// A record used only as the result of a resource load in a library export
// still shares its Clang FieldDecl with the pixel input. Protect emitted
// record-valued expressions as well as declared function parameters/locals.
// CHECK: error: cannot specialize per-vertex interpolation for a record shared with a non-pixel or exported function

struct SharedData {
  nointerpolation float4 color : TEXCOORD0;
};

StructuredBuffer<SharedData> externalData : register(t0);

export float4 exportedRead(uint index) {
  return externalData[index].color;
}

[shader("pixel")]
float4 pixelMain(SharedData input) : SV_Target {
  return GetAttributeAtVertex(input.color, 1);
}
