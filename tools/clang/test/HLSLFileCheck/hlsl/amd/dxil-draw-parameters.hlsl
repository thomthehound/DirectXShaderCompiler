// RUN: %dxc -T vs_6_8 -E main %s | FileCheck %s

// CHECK: dx.op.startVertexLocation.i32
// CHECK: dx.op.startInstanceLocation.i32

struct VSOut {
  float4 position : SV_Position;
  nointerpolation uint2 values : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID,
           uint baseVertex : SV_StartVertexLocation,
           uint baseInstance : SV_StartInstanceLocation) {
  VSOut output;
  output.position = float4(float(vertexId & 1u), float((vertexId >> 1u) & 1u),
                           0.0f, 1.0f);
  output.values = uint2(baseVertex, baseInstance);
  return output;
}
