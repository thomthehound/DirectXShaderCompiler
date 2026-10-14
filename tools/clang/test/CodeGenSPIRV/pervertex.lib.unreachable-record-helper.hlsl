// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s 2>&1 | FileCheck %s --check-prefix=PROTECTED
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 -DNO_VERTEX_ACCESS %s | FileCheck %s --check-prefix=EMITTED

// Both conditional arms return, so the trailing call is generated in a
// CFG-unreachable merge block. The callee can still be emitted into the module
// but is absent from the call graph of CFG-reachable instructions (stage mask
// zero). Its SharedData parameter must protect the record from in-place
// FieldDecl expansion performed for the pixel input.
// PROTECTED: error: cannot specialize per-vertex interpolation for a record shared with a non-pixel or exported function
// EMITTED: OpName %deadHelper "deadHelper"

struct SharedData {
  nointerpolation float4 color : TEXCOORD0;
};

float4 deadHelper(SharedData value) {
  return value.color;
}

[shader("pixel")]
float4 pixelMain(SharedData input, float selector : TEXCOORD1) : SV_Target {
  if (selector > 0.0) {
#ifndef NO_VERTEX_ACCESS
    return GetAttributeAtVertex(input.color, 1);
#else
    return input.color;
#endif
  } else {
#ifndef NO_VERTEX_ACCESS
    return GetAttributeAtVertex(input.color, 2);
#else
    return input.color;
#endif
  }
  return deadHelper(input);
}
