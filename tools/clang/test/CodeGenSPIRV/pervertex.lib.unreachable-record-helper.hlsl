// RUN: not %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s 2>&1 | FileCheck %s --check-prefix=PROTECTED
// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 -DNO_VERTEX_ACCESS %s | FileCheck %s --check-prefix=EMITTED

// After an early return, SpirvEmitter::doReturnStmt creates a disconnected
// basic block for later statements. Emitting the trailing call schedules
// deadHelper, but its call edge is absent from the CFG-reachable instruction
// walk. The emitted helper therefore has stage mask zero. Its SharedData
// parameter must still protect the record from pixel-only FieldDecl mutation.
// PROTECTED: error: cannot specialize per-vertex interpolation for a record shared with a non-pixel or exported function
// EMITTED: OpName %deadHelper "deadHelper"

struct SharedData {
  nointerpolation float4 color : TEXCOORD0;
};

float4 deadHelper(SharedData value) {
  return value.color;
}

[shader("pixel")]
float4 pixelMain(SharedData input) : SV_Target {
#ifndef NO_VERTEX_ACCESS
  return GetAttributeAtVertex(input.color, 1);
#else
  return input.color;
#endif

  return deadHelper(input);
}
