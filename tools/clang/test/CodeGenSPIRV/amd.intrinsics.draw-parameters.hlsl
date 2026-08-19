// RUN: %dxc -T vs_6_8 -E main -fcgl -spirv -fspv-target-env=vulkan1.1 %s | FileCheck %s

// Vulkan 1.1 targets SPIR-V 1.3, where shader draw parameters are core. The
// capability and builtins are the contract; an extension declaration is not.
// CHECK: OpCapability DrawParameters
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
