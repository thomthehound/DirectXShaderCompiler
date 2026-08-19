// RUN: %dxc -T ps_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

#include <vk/amd/intrinsics.h>

// D3D prefix-stable packing places these two float2 arbitrary inputs in the
// same parameter register. Their explicit Vulkan locations deliberately do not
// match that D3D register number.
struct PSIn {
  [[vk::location(7)]] float2 lo : TEXCOORD0;
  [[vk::location(9)]] float2 hi : TEXCOORD1;
};

// CHECK: OpCapability InterpolationFunction
// CHECK: OpExtension "SPV_AMD_shader_explicit_vertex_parameter"
// CHECK: [[INTERP:%[0-9]+]] = OpExtInstImport "SPV_AMD_shader_explicit_vertex_parameter"
// CHECK-DAG: OpDecorate [[LO:%[a-zA-Z0-9_]+]] Location 7
// CHECK-DAG: OpDecorate [[HI:%[a-zA-Z0-9_]+]] Location 9
// CHECK: [[LO]] = OpVariable %_ptr_Input_v2float Input
// CHECK: [[HI]] = OpVariable %_ptr_Input_v2float Input
// CHECK-DAG: [[LO0:%[0-9]+]] = OpAccessChain %_ptr_Input_float [[LO]] %uint_0
// CHECK-DAG: [[LO1:%[0-9]+]] = OpAccessChain %_ptr_Input_float [[LO]] %uint_1
// CHECK-DAG: [[HI0:%[0-9]+]] = OpAccessChain %_ptr_Input_float [[HI]] %uint_0
// CHECK-DAG: [[HI1:%[0-9]+]] = OpAccessChain %_ptr_Input_float [[HI]] %uint_1
// CHECK-DAG: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD [[LO0]] %uint_1
// CHECK-DAG: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD [[LO1]] %uint_1
// CHECK-DAG: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD [[HI0]] %uint_1
// CHECK-DAG: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD [[HI1]] %uint_1

float4 main(PSIn input) : SV_Target {
  return vk::AmdVertexParameter(1u, 0u);
}
