// RUN: %dxc -T vs_6_8 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

// CHECK: OpExtension "SPV_KHR_shader_draw_parameters"
// CHECK: BuiltIn BaseVertex
// CHECK: BuiltIn BaseInstance
// CHECK: BuiltIn DrawIndex

struct VSOut {
  float4 position : SV_Position;
  nointerpolation uint4 values : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID,
           uint baseVertex : SV_StartVertexLocation,
           uint baseInstance : SV_StartInstanceLocation,
           [[vk::builtin("DrawIndex")]] uint drawIndex : DRAW_INDEX) {
  VSOut output;
  output.position = float4(float(vertexId & 1u), float((vertexId >> 1u) & 1u),
                           0.0f, 1.0f);
  output.values = uint4(baseVertex, baseInstance, drawIndex, vertexId);
  return output;
}
