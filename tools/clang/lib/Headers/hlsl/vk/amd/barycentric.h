// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// SPDX-License-Identifier: MIT

#ifndef _HLSL_VK_AMD_BARYCENTRIC_H_
#define _HLSL_VK_AMD_BARYCENTRIC_H_

#include <vk/spirv.h>

namespace vk {
namespace amd {

// SPV_AMD_shader_explicit_vertex_parameter defines BuiltIn
// BaryCoordPullModelAMD (4998) as the exact float3 (1/W, 1/I, 1/J) fragment-
// center value used by AGS PullModelBarycentricCoords. Inline SPIR-V lets the
// header expose that builtin directly even though DXC's string vk::builtin
// whitelist does not include this AMD builtin.
[[vk::ext_extension("SPV_AMD_shader_explicit_vertex_parameter")]]
[[vk::ext_builtin_input(4998)]]
static const float3 PullModelBarycentricInput;

float3 PullModelBarycentricCoords() { return PullModelBarycentricInput; }

} // namespace amd
} // namespace vk

#endif // _HLSL_VK_AMD_BARYCENTRIC_H_
