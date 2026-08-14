//===---- FeatureManager.cpp - SPIR-V Version/Extension Manager -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//===----------------------------------------------------------------------===//

#include "clang/SPIRV/FeatureManager.h"

#include <array>
#include <sstream>

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringSwitch.h"

namespace clang {
namespace spirv {
namespace {

constexpr std::array<std::pair<const char *, spv_target_env>, 6>
    kKnownTargetEnv = {{{"vulkan1.0", SPV_ENV_VULKAN_1_0},
                        {"vulkan1.1", SPV_ENV_VULKAN_1_1},
                        {"vulkan1.1spirv1.4", SPV_ENV_VULKAN_1_1_SPIRV_1_4},
                        {"vulkan1.2", SPV_ENV_VULKAN_1_2},
                        {"vulkan1.3", SPV_ENV_VULKAN_1_3},
                        {"universal1.5", SPV_ENV_UNIVERSAL_1_5}}};

constexpr std::array<std::pair<spv_target_env, const char *>, 6>
    kHumanReadableTargetEnv = {
        {{SPV_ENV_VULKAN_1_0, "Vulkan 1.0"},
         {SPV_ENV_VULKAN_1_1, "Vulkan 1.1"},
         {SPV_ENV_VULKAN_1_1_SPIRV_1_4, "Vulkan 1.1 with SPIR-V 1.4"},
         {SPV_ENV_VULKAN_1_2, "Vulkan 1.2"},
         {SPV_ENV_VULKAN_1_3, "Vulkan 1.3"},
         {SPV_ENV_UNIVERSAL_1_5, "SPIR-V 1.5"}}};

constexpr std::array<std::pair<spv_target_env, std::pair<uint32_t, uint32_t>>,
                     6>
    kTargetEnvToSpirvVersion = {{{SPV_ENV_VULKAN_1_0, {1, 0}},
                                 {SPV_ENV_VULKAN_1_1, {1, 3}},
                                 {SPV_ENV_VULKAN_1_1_SPIRV_1_4, {1, 4}},
                                 {SPV_ENV_VULKAN_1_2, {1, 5}},
                                 {SPV_ENV_VULKAN_1_3, {1, 6}},
                                 {SPV_ENV_UNIVERSAL_1_5, {1, 5}}}};

static_assert(
    kKnownTargetEnv.size() == kHumanReadableTargetEnv.size(),
    "kKnownTargetEnv and kHumanReadableTargetEnv should remain in sync.");

} // end namespace

llvm::Optional<spv_target_env>
FeatureManager::stringToSpvEnvironment(const std::string &target_env) {
  auto it =
      std::find_if(kKnownTargetEnv.cbegin(), kKnownTargetEnv.cend(),
                   [&](const auto &pair) { return pair.first == target_env; });

  return it == kKnownTargetEnv.end()
             ? llvm::None
             : llvm::Optional<spv_target_env>(it->second);
}

clang::VersionTuple FeatureManager::getSpirvVersion(spv_target_env env) {
  auto it = std::find_if(kTargetEnvToSpirvVersion.cbegin(),
                         kTargetEnvToSpirvVersion.cend(),
                         [&](const auto &pair) { return pair.first == env; });

  return it == kTargetEnvToSpirvVersion.end()
             ? clang::VersionTuple()
             : clang::VersionTuple(it->second.first, it->second.second);
}

llvm::Optional<std::string>
FeatureManager::spvEnvironmentToPrettyName(spv_target_env target_env) {
  auto it = std::find_if(
      kHumanReadableTargetEnv.cbegin(), kHumanReadableTargetEnv.cend(),
      [&](const auto &pair) { return pair.first == target_env; });
  return it == kHumanReadableTargetEnv.end()
             ? llvm::None
             : llvm::Optional<std::string>(it->second);
}

FeatureManager::FeatureManager(DiagnosticsEngine &de,
                               const SpirvCodeGenOptions &opts)
    : diags(de) {
  allowedExtensions.resize(static_cast<unsigned>(Extension::Unknown) + 1);

  targetEnvStr = opts.targetEnv;

  llvm::Optional<spv_target_env> targetEnvOpt =
      stringToSpvEnvironment(opts.targetEnv);
  if (!targetEnvOpt) {
    emitError("unknown SPIR-V target environment '%0'", {}) << opts.targetEnv;
    emitNote("allowed options are:\n vulkan1.0\n vulkan1.1\n "
             "vulkan1.1spirv1.4\n vulkan1.2\n vulkan1.3\n universal1.5",
             {});
    return;
  }
  targetEnv = *targetEnvOpt;

  if (opts.allowedExtensions.empty()) {
    // If no explicit extension control from command line, use the default mode:
    // allowing all extensions that are enabled by default.
    allowAllKnownExtensions();
  } else {
    for (auto ext : opts.allowedExtensions)
      allowExtension(ext);

    // The option to use the vulkan memory model implies the extension is
    // available.
    if (opts.useVulkanMemoryModel) {
      allowExtension("SPV_KHR_vulkan_memory_model");
    }
  }
}

bool FeatureManager::allowExtension(llvm::StringRef name) {
  // Family aliases deliberately enable a complete vendor/standards extension
  // surface. This is especially useful for AMD bring-up, where accepting only
  // a hand-maintained subset silently defeats the purpose of native-oriented
  // code generation.
  if (getExtensionSymbol(name) == Extension::KHR) {
    bool result = true;
    for (uint32_t i = 0; i < static_cast<uint32_t>(Extension::Unknown); ++i) {
      llvm::StringRef extName(getExtensionName(static_cast<Extension>(i)));
      if (isKHRExtension(extName))
        result = result && allowExtension(extName);
    }
    return result;
  }

  if (getExtensionSymbol(name) == Extension::AMD) {
    bool result = true;
    for (uint32_t i = 0; i < static_cast<uint32_t>(Extension::Unknown); ++i) {
      llvm::StringRef extName(getExtensionName(static_cast<Extension>(i)));
      if (extName.startswith("SPV_AMD") || extName.startswith("SPV_AMDX"))
        result = result && allowExtension(extName);
    }
    return result;
  }

  const auto symbol = getExtensionSymbol(name);
  if (symbol == Extension::Unknown) {
    emitError("unknown SPIR-V extension '%0'", {}) << name;
    emitNote("known extensions are\n%0", {})
        << getKnownExtensions("\n* ", "* ");
    return false;
  }

  allowedExtensions.set(static_cast<unsigned>(symbol));

  return true;
}

void FeatureManager::allowAllKnownExtensions() {
  allowedExtensions.set();
  const auto numExtensions = static_cast<uint32_t>(Extension::Unknown);
  for (uint32_t ext = 0; ext < numExtensions; ++ext)
    if (!enabledByDefault(static_cast<Extension>(ext)))
      allowedExtensions.reset(ext);
}

bool FeatureManager::requestExtension(Extension ext, llvm::StringRef target,
                                      SourceLocation srcLoc) {
  if (allowedExtensions.test(static_cast<unsigned>(ext)))
    return true;

  emitError("SPIR-V extension '%0' required for %1 but not permitted to use",
            srcLoc)
      << getExtensionName(ext) << target;
  return false;
}

bool FeatureManager::requestTargetEnv(spv_target_env requestedEnv,
                                      llvm::StringRef target,
                                      SourceLocation srcLoc) {
  if (targetEnv < requestedEnv) {
    auto envName = spvEnvironmentToPrettyName(requestedEnv);
    emitError("%0 is required for %1 but not permitted to use", srcLoc)
        << envName.getValueOr("unknown") << target;
    emitNote("please specify your target environment via command line option "
             "-fspv-target-env=",
             {});
    return false;
  }
  return true;
}

Extension FeatureManager::getExtensionSymbol(llvm::StringRef name) {
  return llvm::StringSwitch<Extension>(name)
      .Case("KHR", Extension::KHR)
      .Case("AMD", Extension::AMD)
      .Case("SPV_AMD", Extension::AMD)
      .Case("SPV_KHR_16bit_storage", Extension::KHR_16bit_storage)
      .Case("SPV_KHR_bfloat16", Extension::KHR_bfloat16)
      .Case("SPV_KHR_device_group", Extension::KHR_device_group)
      .Case("SPV_KHR_multiview", Extension::KHR_multiview)
      .Case("SPV_KHR_non_semantic_info", Extension::KHR_non_semantic_info)
      .Case("SPV_KHR_shader_draw_parameters",
            Extension::KHR_shader_draw_parameters)
      .Case("SPV_KHR_cooperative_matrix", Extension::KHR_cooperative_matrix)
      .Case("SPV_KHR_ray_tracing", Extension::KHR_ray_tracing)
      .Case("SPV_EXT_demote_to_helper_invocation",
            Extension::EXT_demote_to_helper_invocation)
      .Case("SPV_EXT_descriptor_indexing", Extension::EXT_descriptor_indexing)
      .Case("SPV_EXT_fragment_fully_covered",
            Extension::EXT_fragment_fully_covered)
      .Case("SPV_EXT_fragment_invocation_density",
            Extension::EXT_fragment_invocation_density)
      .Case("SPV_EXT_fragment_shader_interlock",
            Extension::EXT_fragment_shader_interlock)
      .Case("SPV_EXT_float8", Extension::EXT_float8)
      .Case("SPV_EXT_mesh_shader", Extension::EXT_mesh_shader)
      .Case("SPV_EXT_shader_stencil_export",
            Extension::EXT_shader_stencil_export)
      .Case("SPV_EXT_shader_viewport_index_layer",
            Extension::EXT_shader_viewport_index_layer)
      .Case("SPV_AMD_gcn_shader", Extension::AMD_gcn_shader)
      .Case("SPV_AMD_gpu_shader_half_float",
            Extension::AMD_gpu_shader_half_float)
      .Case("SPV_AMD_gpu_shader_half_float_fetch",
            Extension::AMD_gpu_shader_half_float_fetch)
      .Case("SPV_AMD_gpu_shader_int16", Extension::AMD_gpu_shader_int16)
      .Case("SPV_AMD_shader_ballot", Extension::AMD_shader_ballot)
      .Case("SPV_AMD_shader_early_and_late_fragment_tests",
            Extension::AMD_shader_early_and_late_fragment_tests)
      .Case("SPV_AMD_shader_explicit_vertex_parameter",
            Extension::AMD_shader_explicit_vertex_parameter)
      .Case("SPV_AMD_shader_fragment_mask",
            Extension::AMD_shader_fragment_mask)
      .Case("SPV_AMD_shader_image_load_store_lod",
            Extension::AMD_shader_image_load_store_lod)
      .Case("SPV_AMD_shader_trinary_minmax",
            Extension::AMD_shader_trinary_minmax)
      .Case("SPV_AMD_texture_gather_bias_lod",
            Extension::AMD_texture_gather_bias_lod)
      .Case("SPV_AMD_weak_linkage", Extension::AMD_weak_linkage)
      .Case("SPV_GOOGLE_hlsl_functionality1",
            Extension::GOOGLE_hlsl_functionality1)
      .Case("SPV_GOOGLE_user_type", Extension::GOOGLE_user_type)
      .Case("SPV_KHR_post_depth_coverage", Extension::KHR_post_depth_coverage)
      .Case("SPV_KHR_shader_clock", Extension::KHR_shader_clock)
      .Case("SPV_NV_ray_tracing", Extension::NV_ray_tracing)
      .Case("SPV_NV_mesh_shader", Extension::NV_mesh_shader)
      .Case("SPV_KHR_ray_query", Extension::KHR_ray_query)
      .Case("SPV_KHR_fragment_shading_rate",
            Extension::KHR_fragment_shading_rate)
      .Case("SPV_EXT_shader_image_int64", Extension::EXT_shader_image_int64)
      .Case("SPV_KHR_physical_storage_buffer",
            Extension::KHR_physical_storage_buffer)
      .Case("SPV_AMDX_shader_enqueue", Extension::AMD_shader_enqueue)
      .Case("SPV_KHR_vulkan_memory_model", Extension::KHR_vulkan_memory_model)
      .Case("SPV_KHR_compute_shader_derivatives",
            Extension::KHR_compute_shader_derivatives)
      .Case("SPV_NV_compute_shader_derivatives",
            Extension::NV_compute_shader_derivatives)
      .Case("SPV_KHR_fragment_shader_barycentric",
            Extension::KHR_fragment_shader_barycentric)
      .Case("SPV_KHR_maximal_reconvergence",
            Extension::KHR_maximal_reconvergence)
      .Case("SPV_KHR_float_controls", Extension::KHR_float_controls)
      .Case("SPV_NV_shader_subgroup_partitioned",
            Extension::NV_shader_subgroup_partitioned)
      .Case("SPV_KHR_quad_control", Extension::KHR_quad_control)
      .Case("SPV_EXT_descriptor_heap", Extension::EXT_descriptor_heap)
      .Case("SPV_KHR_untyped_pointers", Extension::KHR_untyped_pointers)
      .Default(Extension::Unknown);
}

const char *FeatureManager::getExtensionName(Extension symbol) {
  switch (symbol) {
  case Extension::KHR:
    return "KHR";
  case Extension::AMD:
    return "AMD";
  case Extension::KHR_16bit_storage:
    return "SPV_KHR_16bit_storage";
  case Extension::KHR_bfloat16:
    return "SPV_KHR_bfloat16";
  case Extension::KHR_device_group:
    return "SPV_KHR_device_group";
  case Extension::KHR_multiview:
    return "SPV_KHR_multiview";
  case Extension::KHR_non_semantic_info:
    return "SPV_KHR_non_semantic_info";
  case Extension::KHR_shader_draw_parameters:
    return "SPV_KHR_shader_draw_parameters";
  case Extension::KHR_post_depth_coverage:
    return "SPV_KHR_post_depth_coverage";
  case Extension::KHR_cooperative_matrix:
    return "SPV_KHR_cooperative_matrix";
  case Extension::KHR_ray_tracing:
    return "SPV_KHR_ray_tracing";
  case Extension::KHR_shader_clock:
    return "SPV_KHR_shader_clock";
  case Extension::EXT_demote_to_helper_invocation:
    return "SPV_EXT_demote_to_helper_invocation";
  case Extension::EXT_descriptor_indexing:
    return "SPV_EXT_descriptor_indexing";
  case Extension::EXT_fragment_fully_covered:
    return "SPV_EXT_fragment_fully_covered";
  case Extension::EXT_fragment_invocation_density:
    return "SPV_EXT_fragment_invocation_density";
  case Extension::EXT_fragment_shader_interlock:
    return "SPV_EXT_fragment_shader_interlock";
  case Extension::EXT_float8:
    return "SPV_EXT_float8";
  case Extension::EXT_mesh_shader:
    return "SPV_EXT_mesh_shader";
  case Extension::EXT_shader_stencil_export:
    return "SPV_EXT_shader_stencil_export";
  case Extension::EXT_shader_viewport_index_layer:
    return "SPV_EXT_shader_viewport_index_layer";
  case Extension::AMD_gcn_shader:
    return "SPV_AMD_gcn_shader";
  case Extension::AMD_gpu_shader_half_float:
    return "SPV_AMD_gpu_shader_half_float";
  case Extension::AMD_gpu_shader_half_float_fetch:
    return "SPV_AMD_gpu_shader_half_float_fetch";
  case Extension::AMD_gpu_shader_int16:
    return "SPV_AMD_gpu_shader_int16";
  case Extension::AMD_shader_ballot:
    return "SPV_AMD_shader_ballot";
  case Extension::AMD_shader_early_and_late_fragment_tests:
    return "SPV_AMD_shader_early_and_late_fragment_tests";
  case Extension::AMD_shader_explicit_vertex_parameter:
    return "SPV_AMD_shader_explicit_vertex_parameter";
  case Extension::AMD_shader_fragment_mask:
    return "SPV_AMD_shader_fragment_mask";
  case Extension::AMD_shader_image_load_store_lod:
    return "SPV_AMD_shader_image_load_store_lod";
  case Extension::AMD_shader_trinary_minmax:
    return "SPV_AMD_shader_trinary_minmax";
  case Extension::AMD_texture_gather_bias_lod:
    return "SPV_AMD_texture_gather_bias_lod";
  case Extension::AMD_weak_linkage:
    return "SPV_AMD_weak_linkage";
  case Extension::GOOGLE_hlsl_functionality1:
    return "SPV_GOOGLE_hlsl_functionality1";
  case Extension::GOOGLE_user_type:
    return "SPV_GOOGLE_user_type";
  case Extension::NV_ray_tracing:
    return "SPV_NV_ray_tracing";
  case Extension::NV_mesh_shader:
    return "SPV_NV_mesh_shader";
  case Extension::KHR_ray_query:
    return "SPV_KHR_ray_query";
  case Extension::KHR_fragment_shading_rate:
    return "SPV_KHR_fragment_shading_rate";
  case Extension::EXT_shader_image_int64:
    return "SPV_EXT_shader_image_int64";
  case Extension::KHR_physical_storage_buffer:
    return "SPV_KHR_physical_storage_buffer";
  case Extension::AMD_shader_enqueue:
    return "SPV_AMDX_shader_enqueue";
  case Extension::KHR_vulkan_memory_model:
    return "SPV_KHR_vulkan_memory_model";
  case Extension::KHR_compute_shader_derivatives:
    return "SPV_KHR_compute_shader_derivatives";
  case Extension::NV_compute_shader_derivatives:
    return "SPV_NV_compute_shader_derivatives";
  case Extension::KHR_fragment_shader_barycentric:
    return "SPV_KHR_fragment_shader_barycentric";
  case Extension::KHR_maximal_reconvergence:
    return "SPV_KHR_maximal_reconvergence";
  case Extension::KHR_float_controls:
    return "SPV_KHR_float_controls";
  case Extension::NV_shader_subgroup_partitioned:
    return "SPV_NV_shader_subgroup_partitioned";
  case Extension::KHR_quad_control:
    return "SPV_KHR_quad_control";
  case Extension::EXT_descriptor_heap:
    return "SPV_EXT_descriptor_heap";
  case Extension::KHR_untyped_pointers:
    return "SPV_KHR_untyped_pointers";
  default:
    break;
  }
  return "<unknown extension>";
}

bool FeatureManager::isKHRExtension(llvm::StringRef name) const {
  return name.startswith_lower("spv_khr_");
}

std::string FeatureManager::getKnownExtensions(const char *delimiter,
                                               const char *prefix,
                                               const char *postfix) {
  std::ostringstream oss;

  oss << prefix;

  const auto numExtensions = static_cast<uint32_t>(Extension::Unknown);
  for (uint32_t i = 0; i < numExtensions; ++i) {
    oss << getExtensionName(static_cast<Extension>(i));
    if (i + 1 < numExtensions)
      oss << delimiter;
  }

  oss << postfix;

  return oss.str();
}

bool FeatureManager::isExtensionRequiredForTargetEnv(Extension ext) const {
  bool required = true;
  if (targetEnv >= SPV_ENV_VULKAN_1_3) {
    // The following extensions are incorporated into Vulkan 1.3 or above, and
    // are therefore not required to be emitted for that target environment.
    switch (ext) {
    case Extension::KHR_non_semantic_info:
      required = false;
      break;
    default:
      break;
    }
  }
  if (required && targetEnv >= SPV_ENV_VULKAN_1_1) {
    // The following extensions are incorporated into Vulkan 1.1 or above, and
    // are therefore not required to be emitted for that target environment.
    // TODO: Also add the following extensions  if we start to support them.
    // * SPV_KHR_storage_buffer_storage_class
    // * SPV_KHR_variable_pointers
    switch (ext) {
    case Extension::KHR_16bit_storage:
    case Extension::KHR_device_group:
    case Extension::KHR_multiview:
    case Extension::KHR_shader_draw_parameters:
      required = false;
      break;
    default:
      // Only 1.1 or above extensions can be suppressed.
      break;
    }
  }

  return required;
}

bool FeatureManager::isExtensionEnabled(Extension ext) const {
  bool allowed = false;
  if (ext != Extension::Unknown &&
      allowedExtensions.test(static_cast<unsigned>(ext)))
    allowed = true;
  return allowed;
}

bool FeatureManager::enabledByDefault(Extension ext) {
  switch (ext) {
    // KHR_ray_tracing and NV_ray_tracing are mutually exclusive so enable only
    // KHR extension by default
  case Extension::NV_ray_tracing:
    return false;
    // KHR_compute_shader_derivatives and NV_compute_shader_derivatives are
    // mutually exclusive so enable only KHR extension by default
  case Extension::NV_compute_shader_derivatives:
    return false;
    // Enabling EXT_demote_to_helper_invocation changes the code generation
    // behavior for the 'discard' statement. Therefore we will only enable it if
    // the user explicitly asks for it.
  case Extension::EXT_demote_to_helper_invocation:
    return false;
  case Extension::EXT_mesh_shader:
    // Enabling EXT_mesh_shader only when the target environment is SPIR-V 1.4
    // or above
    return isTargetEnvSpirv1p4OrAbove();
  default:
    return true;
  }
}

bool FeatureManager::isTargetEnvVulkan1p1OrAbove() const {
  return targetEnv >= SPV_ENV_VULKAN_1_1;
}

bool FeatureManager::isTargetEnvSpirv1p4OrAbove() const {
  return targetEnv >= SPV_ENV_UNIVERSAL_1_4;
}

bool FeatureManager::isTargetEnvVulkan1p1Spirv1p4OrAbove() const {
  return targetEnv >= SPV_ENV_VULKAN_1_1_SPIRV_1_4;
}

bool FeatureManager::isTargetEnvVulkan1p2OrAbove() const {
  return targetEnv >= SPV_ENV_VULKAN_1_2;
}

bool FeatureManager::isTargetEnvVulkan1p3OrAbove() const {
  return targetEnv >= SPV_ENV_VULKAN_1_3;
}

bool FeatureManager::isTargetEnvVulkan() const {
  // This assert ensure that this list will be updated, if necessary, when
  // a new target environment is added.
  static_assert(SPV_ENV_VULKAN_1_4 + 1 == SPV_ENV_MAX);

  switch (targetEnv) {
  case SPV_ENV_VULKAN_1_0:
  case SPV_ENV_VULKAN_1_1:
  case SPV_ENV_VULKAN_1_2:
  case SPV_ENV_VULKAN_1_1_SPIRV_1_4:
  case SPV_ENV_VULKAN_1_3:
  case SPV_ENV_VULKAN_1_4:
    return true;
  default:
    return false;
  }
}

} // end namespace spirv
} // end namespace clang
