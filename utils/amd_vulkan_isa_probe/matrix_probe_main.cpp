#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if !defined(VK_KHR_cooperative_matrix)
int main() {
  std::cerr << "amd_vulkan_matrix_probe: Vulkan SDK headers do not expose "
               "VK_KHR_cooperative_matrix\n";
  return 20;
}
#else

namespace {

struct Options {
  std::filesystem::path spirv;
  std::filesystem::path output;
  std::string a_type;
  std::string b_type;
  std::string c_type;
  std::string result_type;
  uint32_t m = 16;
  uint32_t n = 16;
  uint32_t k = 16;
  std::string entry = "main";
  std::optional<uint32_t> device_index;
};

[[noreturn]] void usage(const char *exe) {
  std::cerr << "Usage: " << exe
            << " <shader.spv> <disassembly.txt> --a-type TYPE --b-type TYPE"
               " --c-type TYPE --result-type TYPE [--m N --n N --k N]"
               " [--device N] [--entry NAME]\n"
               "TYPE: f16, f32, bf16, i8, u8, i32, u32\n";
  std::exit(2);
}

uint32_t parse_u32(std::string_view text, std::string_view what) {
  size_t consumed = 0;
  unsigned long value = 0;
  try {
    value = std::stoul(std::string(text), &consumed, 0);
  } catch (...) {
    throw std::runtime_error("invalid " + std::string(what));
  }
  if (consumed != text.size() || value > UINT32_MAX)
    throw std::runtime_error("invalid " + std::string(what));
  return static_cast<uint32_t>(value);
}

Options parse_args(int argc, char **argv) {
  if (argc < 3)
    usage(argv[0]);
  Options o;
  o.spirv = argv[1];
  o.output = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto next = [&]() -> std::string_view {
      if (++i >= argc)
        usage(argv[0]);
      return argv[i];
    };
    if (arg == "--a-type") o.a_type = next();
    else if (arg == "--b-type") o.b_type = next();
    else if (arg == "--c-type") o.c_type = next();
    else if (arg == "--result-type") o.result_type = next();
    else if (arg == "--m") o.m = parse_u32(next(), "M");
    else if (arg == "--n") o.n = parse_u32(next(), "N");
    else if (arg == "--k") o.k = parse_u32(next(), "K");
    else if (arg == "--device") o.device_index = parse_u32(next(), "device");
    else if (arg == "--entry") o.entry = next();
    else usage(argv[0]);
  }
  if (o.a_type.empty() || o.b_type.empty() || o.c_type.empty() ||
      o.result_type.empty())
    usage(argv[0]);
  return o;
}

std::vector<uint32_t> read_spirv(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::runtime_error("cannot open SPIR-V file: " + path.string());
  const auto end = file.tellg();
  if (end <= 0 || (end % 4) != 0)
    throw std::runtime_error("SPIR-V size is not a positive multiple of 4");
  std::vector<uint32_t> words(static_cast<size_t>(end) / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(words.data()), end);
  if (!file || words.front() != 0x07230203u)
    throw std::runtime_error("invalid SPIR-V input");
  return words;
}

bool has_extension(VkPhysicalDevice d, const char *name) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(d, nullptr, &count, nullptr) != VK_SUCCESS)
    throw std::runtime_error("cannot enumerate device extensions");
  std::vector<VkExtensionProperties> exts(count);
  if (vkEnumerateDeviceExtensionProperties(d, nullptr, &count, exts.data()) != VK_SUCCESS)
    throw std::runtime_error("cannot enumerate device extensions");
  return std::ranges::any_of(exts, [name](const auto &e) {
    return std::strcmp(e.extensionName, name) == 0;
  });
}

std::optional<uint32_t> compute_queue(VkPhysicalDevice d) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(d, &count, nullptr);
  std::vector<VkQueueFamilyProperties> props(count);
  vkGetPhysicalDeviceQueueFamilyProperties(d, &count, props.data());
  for (uint32_t i = 0; i < count; ++i)
    if (props[i].queueCount && (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
      return i;
  return std::nullopt;
}

VkComponentTypeKHR component_type(std::string_view name) {
  if (name == "f16") return VK_COMPONENT_TYPE_FLOAT16_KHR;
  if (name == "f32") return VK_COMPONENT_TYPE_FLOAT32_KHR;
  if (name == "i8") return VK_COMPONENT_TYPE_SINT8_KHR;
  if (name == "u8") return VK_COMPONENT_TYPE_UINT8_KHR;
  if (name == "i32") return VK_COMPONENT_TYPE_SINT32_KHR;
  if (name == "u32") return VK_COMPONENT_TYPE_UINT32_KHR;
#if defined(VK_KHR_shader_bfloat16)
  if (name == "bf16") return VK_COMPONENT_TYPE_BFLOAT16_KHR;
#endif
  throw std::runtime_error("unsupported component type in this Vulkan SDK: " +
                           std::string(name));
}

bool uses(std::string_view needle, const Options &o) {
  return o.a_type == needle || o.b_type == needle || o.c_type == needle ||
         o.result_type == needle;
}

void check(VkResult r, std::string_view what) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(what) + " failed with VkResult " +
                             std::to_string(static_cast<int>(r)));
}

template <typename T, void (*Destroy)(VkDevice, T, const VkAllocationCallbacks *)>
struct DeviceHandle {
  VkDevice device = VK_NULL_HANDLE;
  T value = VK_NULL_HANDLE;
  ~DeviceHandle() {
    if (value != VK_NULL_HANDLE)
      Destroy(device, value, nullptr);
  }
};

} // namespace

int main(int argc, char **argv) {
  try {
    const Options o = parse_args(argc, argv);
    const auto spirv = read_spirv(o.spirv);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "amd_vulkan_matrix_probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
    struct InstanceGuard {
      VkInstance v;
      ~InstanceGuard() { if (v) vkDestroyInstance(v, nullptr); }
    } instance_guard{instance};

    uint32_t physical_count = 0;
    check(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr),
          "vkEnumeratePhysicalDevices(count)");
    std::vector<VkPhysicalDevice> devices(physical_count);
    check(vkEnumeratePhysicalDevices(instance, &physical_count, devices.data()),
          "vkEnumeratePhysicalDevices(list)");
    if (devices.empty())
      throw std::runtime_error("no Vulkan device found");

    uint32_t device_index = 0;
    if (o.device_index) {
      if (*o.device_index >= devices.size())
        throw std::runtime_error("device index out of range");
      device_index = *o.device_index;
    } else {
      auto it = std::find_if(devices.begin(), devices.end(), [](VkPhysicalDevice d) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        return p.vendorID == 0x1002;
      });
      if (it != devices.end())
        device_index = static_cast<uint32_t>(it - devices.begin());
    }
    VkPhysicalDevice physical = devices[device_index];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    std::cout << "device: " << props.deviceName << "\n";

    if (!has_extension(physical, VK_AMD_SHADER_INFO_EXTENSION_NAME)) {
      std::cout << "UNSUPPORTED: VK_AMD_shader_info\n";
      return 30;
    }
    if (!has_extension(physical, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) {
      std::cout << "UNSUPPORTED: VK_KHR_cooperative_matrix\n";
      return 31;
    }
#if defined(VK_KHR_shader_bfloat16)
    if (uses("bf16", o) &&
        !has_extension(physical, VK_KHR_SHADER_BFLOAT16_EXTENSION_NAME)) {
      std::cout << "UNSUPPORTED: VK_KHR_shader_bfloat16\n";
      return 32;
    }
#else
    if (uses("bf16", o)) {
      std::cout << "UNSUPPORTED: SDK lacks VK_KHR_shader_bfloat16\n";
      return 32;
    }
#endif

    auto get_matrix_props = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
    if (!get_matrix_props)
      throw std::runtime_error("vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR is null");

    uint32_t matrix_count = 0;
    check(get_matrix_props(physical, &matrix_count, nullptr),
          "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(count)");
    std::vector<VkCooperativeMatrixPropertiesKHR> matrix_props(matrix_count);
    for (auto &p : matrix_props)
      p.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
    check(get_matrix_props(physical, &matrix_count, matrix_props.data()),
          "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(list)");

    const auto a_type = component_type(o.a_type);
    const auto b_type = component_type(o.b_type);
    const auto c_type = component_type(o.c_type);
    const auto result_type = component_type(o.result_type);
    const bool property_match = std::ranges::any_of(matrix_props, [&](const auto &p) {
      return p.MSize == o.m && p.NSize == o.n && p.KSize == o.k &&
             p.AType == a_type && p.BType == b_type && p.CType == c_type &&
             p.ResultType == result_type && p.scope == VK_SCOPE_SUBGROUP_KHR;
    });
    if (!property_match) {
      std::cout << "UNSUPPORTED_PROPERTY: M=" << o.m << " N=" << o.n
                << " K=" << o.k << " A=" << o.a_type << " B=" << o.b_type
                << " C=" << o.c_type << " R=" << o.result_type << "\n";
      return 33;
    }
    std::cout << "PROPERTY_SUPPORTED\n";

    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat16Int8Features f16i8{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
#if defined(VK_KHR_shader_bfloat16)
    VkPhysicalDeviceShaderBfloat16FeaturesKHR bf16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR};
    coop.pNext = &f16i8;
    f16i8.pNext = &bf16;
#else
    coop.pNext = &f16i8;
#endif
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &coop;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!coop.cooperativeMatrix) {
      std::cout << "UNSUPPORTED_FEATURE: cooperativeMatrix\n";
      return 34;
    }
    if (uses("f16", o) && !f16i8.shaderFloat16) {
      std::cout << "UNSUPPORTED_FEATURE: shaderFloat16\n";
      return 35;
    }
    if ((uses("i8", o) || uses("u8", o)) && !f16i8.shaderInt8) {
      std::cout << "UNSUPPORTED_FEATURE: shaderInt8\n";
      return 36;
    }
#if defined(VK_KHR_shader_bfloat16)
    if (uses("bf16", o) &&
        (!bf16.shaderBFloat16Type || !bf16.shaderBFloat16CooperativeMatrix)) {
      std::cout << "UNSUPPORTED_FEATURE: shaderBFloat16CooperativeMatrix\n";
      return 37;
    }
#endif

    VkPhysicalDeviceCooperativeMatrixFeaturesKHR enabled_coop{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    enabled_coop.cooperativeMatrix = VK_TRUE;
    VkPhysicalDeviceShaderFloat16Int8Features enabled_f16i8{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    enabled_f16i8.shaderFloat16 = uses("f16", o) ? VK_TRUE : VK_FALSE;
    enabled_f16i8.shaderInt8 = (uses("i8", o) || uses("u8", o)) ? VK_TRUE : VK_FALSE;
    enabled_coop.pNext = &enabled_f16i8;
#if defined(VK_KHR_shader_bfloat16)
    VkPhysicalDeviceShaderBfloat16FeaturesKHR enabled_bf16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR};
    if (uses("bf16", o)) {
      enabled_bf16.shaderBFloat16Type = VK_TRUE;
      enabled_bf16.shaderBFloat16CooperativeMatrix = VK_TRUE;
      enabled_f16i8.pNext = &enabled_bf16;
    }
#endif

    std::vector<const char *> extensions = {
        VK_AMD_SHADER_INFO_EXTENSION_NAME,
        VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
    };
#if defined(VK_KHR_shader_bfloat16)
    if (uses("bf16", o))
      extensions.push_back(VK_KHR_SHADER_BFLOAT16_EXTENSION_NAME);
#endif
    if (props.apiVersion < VK_API_VERSION_1_2 &&
        (uses("f16", o) || uses("i8", o) || uses("u8", o))) {
      if (!has_extension(physical, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) {
        std::cout << "UNSUPPORTED: VK_KHR_shader_float16_int8\n";
        return 38;
      }
      extensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    }

    const auto queue_family = compute_queue(physical);
    if (!queue_family)
      throw std::runtime_error("no compute queue");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = *queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &enabled_coop;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
    struct DeviceGuard {
      VkDevice v;
      ~DeviceGuard() { if (v) vkDestroyDevice(v, nullptr); }
    } device_guard{device};

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo slci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 1;
    slci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(device, &slci, nullptr, &set_layout),
          "vkCreateDescriptorSetLayout");
    DeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout> sl_guard{
        device, set_layout};

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &plci, nullptr, &layout),
          "vkCreatePipelineLayout");
    DeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout> layout_guard{device, layout};

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spirv.size() * sizeof(uint32_t);
    smci.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &smci, nullptr, &module),
          "vkCreateShaderModule");
    DeviceHandle<VkShaderModule, vkDestroyShaderModule> module_guard{device, module};

    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = o.entry.c_str();
    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pci.stage = stage;
    pci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pipeline_result = vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
    if (pipeline_result != VK_SUCCESS) {
      std::cout << "PIPELINE_REJECTED: " << static_cast<int>(pipeline_result) << "\n";
      return 39;
    }
    DeviceHandle<VkPipeline, vkDestroyPipeline> pipeline_guard{device, pipeline};

    const auto get_shader_info = reinterpret_cast<PFN_vkGetShaderInfoAMD>(
        vkGetDeviceProcAddr(device, "vkGetShaderInfoAMD"));
    if (!get_shader_info)
      throw std::runtime_error("vkGetShaderInfoAMD is null");
    size_t size = 0;
    VkResult r = get_shader_info(device, pipeline, VK_SHADER_STAGE_COMPUTE_BIT,
                                VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, &size,
                                nullptr);
    if (r != VK_SUCCESS || size == 0) {
      std::cout << "NO_DISASSEMBLY: " << static_cast<int>(r) << "\n";
      return 40;
    }
    std::vector<char> text(size);
    r = get_shader_info(device, pipeline, VK_SHADER_STAGE_COMPUTE_BIT,
                        VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, &size, text.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
      check(r, "vkGetShaderInfoAMD(data)");
    std::ofstream out(o.output, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(size));
    if (!out)
      throw std::runtime_error("failed writing disassembly");
    std::cout << "ISA_WRITTEN: " << o.output << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "amd_vulkan_matrix_probe: " << e.what() << "\n";
    return 1;
  }
}

#endif
