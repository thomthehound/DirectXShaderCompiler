#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::filesystem::path spirv;
  std::filesystem::path output;
  std::string entry = "main";
  std::optional<uint32_t> device_index;
  uint32_t descriptor_set = 0;
  uint32_t descriptor_binding = 0;
  uint32_t push_constant_bytes = 32;
};

[[noreturn]] void usage(const char* exe) {
  std::cerr
      << "Usage: " << exe
      << " <shader.spv> <disassembly.txt> [--device N] [--entry NAME]"
         " [--set N] [--binding N] [--push-constant-bytes N]\n\n"
         "Creates a compute pipeline and asks VK_AMD_shader_info for the AMD"
         " driver's own disassembly. The default layout matches the shaders"
         " shipped with this probe (set 0, binding 0 storage buffer, 32-byte"
         " compute push-constant range).\n";
  std::exit(2);
}

uint32_t parse_u32(std::string_view text, std::string_view what) {
  size_t consumed = 0;
  unsigned long value = 0;
  try {
    value = std::stoul(std::string(text), &consumed, 0);
  } catch (...) {
    throw std::runtime_error("invalid " + std::string(what) + ": " +
                             std::string(text));
  }
  if (consumed != text.size() || value > UINT32_MAX)
    throw std::runtime_error("invalid " + std::string(what) + ": " +
                             std::string(text));
  return static_cast<uint32_t>(value);
}

Options parse_args(int argc, char** argv) {
  if (argc < 3)
    usage(argv[0]);

  Options result;
  result.spirv = argv[1];
  result.output = argv[2];

  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto next = [&]() -> std::string_view {
      if (++i >= argc)
        usage(argv[0]);
      return argv[i];
    };

    if (arg == "--device")
      result.device_index = parse_u32(next(), "device index");
    else if (arg == "--entry")
      result.entry = next();
    else if (arg == "--set")
      result.descriptor_set = parse_u32(next(), "descriptor set");
    else if (arg == "--binding")
      result.descriptor_binding = parse_u32(next(), "descriptor binding");
    else if (arg == "--push-constant-bytes")
      result.push_constant_bytes = parse_u32(next(), "push-constant size");
    else
      usage(argv[0]);
  }

  if (result.descriptor_set != 0)
    throw std::runtime_error(
        "this first probe intentionally supports descriptor set 0 only");
  if ((result.push_constant_bytes & 3u) != 0)
    throw std::runtime_error("push-constant size must be a multiple of 4");
  return result;
}

template <typename T, void (*Destroy)(VkInstance, T, const VkAllocationCallbacks*)>
struct InstanceHandle {
  VkInstance instance = VK_NULL_HANDLE;
  T value = VK_NULL_HANDLE;
  ~InstanceHandle() {
    if (value != VK_NULL_HANDLE)
      Destroy(instance, value, nullptr);
  }
};

template <typename T, void (*Destroy)(VkDevice, T, const VkAllocationCallbacks*)>
struct DeviceHandle {
  VkDevice device = VK_NULL_HANDLE;
  T value = VK_NULL_HANDLE;
  ~DeviceHandle() {
    if (value != VK_NULL_HANDLE)
      Destroy(device, value, nullptr);
  }
};

std::vector<uint32_t> read_spirv(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::runtime_error("cannot open SPIR-V file: " + path.string());
  const auto end = file.tellg();
  if (end <= 0 || (end % 4) != 0)
    throw std::runtime_error("SPIR-V file size is not a positive multiple of 4");
  std::vector<uint32_t> words(static_cast<size_t>(end) / sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(words.data()), end);
  if (!file)
    throw std::runtime_error("failed reading SPIR-V file");
  if (words.front() != 0x07230203u)
    throw std::runtime_error("input does not have a SPIR-V magic number");
  return words;
}

bool has_extension(VkPhysicalDevice physical_device, const char* name) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count,
                                            nullptr) != VK_SUCCESS)
    throw std::runtime_error("vkEnumerateDeviceExtensionProperties(count) failed");
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count,
                                            extensions.data()) != VK_SUCCESS)
    throw std::runtime_error("vkEnumerateDeviceExtensionProperties(list) failed");
  return std::ranges::any_of(extensions, [name](const auto& ext) {
    return std::strcmp(ext.extensionName, name) == 0;
  });
}

std::optional<uint32_t> find_compute_queue(VkPhysicalDevice physical_device) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count,
                                            families.data());
  for (uint32_t i = 0; i < count; ++i) {
    if (families[i].queueCount != 0 &&
        (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
      return i;
  }
  return std::nullopt;
}

std::string version_string(uint32_t version) {
  return std::to_string(VK_VERSION_MAJOR(version)) + "." +
         std::to_string(VK_VERSION_MINOR(version)) + "." +
         std::to_string(VK_VERSION_PATCH(version));
}

void check(VkResult result, std::string_view what) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(what) + " failed with VkResult " +
                             std::to_string(static_cast<int>(result)));
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_args(argc, argv);
    const std::vector<uint32_t> spirv = read_spirv(options.spirv);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "amd_vulkan_isa_probe";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_ci.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&instance_ci, nullptr, &instance), "vkCreateInstance");

    struct InstanceGuard {
      VkInstance value = VK_NULL_HANDLE;
      ~InstanceGuard() {
        if (value != VK_NULL_HANDLE)
          vkDestroyInstance(value, nullptr);
      }
    } instance_guard{instance};

    uint32_t physical_count = 0;
    check(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr),
          "vkEnumeratePhysicalDevices(count)");
    if (physical_count == 0)
      throw std::runtime_error("no Vulkan physical devices found");
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    check(vkEnumeratePhysicalDevices(instance, &physical_count,
                                     physical_devices.data()),
          "vkEnumeratePhysicalDevices(list)");
    uint32_t device_index = 0;
    if (options.device_index) {
      if (*options.device_index >= physical_count)
        throw std::runtime_error("requested device index is out of range");
      device_index = *options.device_index;
    } else {
      const auto amd_device = std::find_if(
          physical_devices.begin(), physical_devices.end(), [](VkPhysicalDevice d) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(d, &p);
            return p.vendorID == 0x1002;
          });
      if (amd_device != physical_devices.end())
        device_index = static_cast<uint32_t>(amd_device - physical_devices.begin());
      else
        std::cerr << "warning: no AMD Vulkan device found; falling back to device 0\n";
    }

    const VkPhysicalDevice physical_device = physical_devices[device_index];
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    std::cout << "device[" << device_index << "]: "
              << properties.deviceName << "\n"
              << "vendor=0x" << std::hex << properties.vendorID << " device=0x"
              << properties.deviceID << std::dec << " api="
              << version_string(properties.apiVersion) << " driverVersion=0x"
              << std::hex << properties.driverVersion << std::dec << "\n";

    if (properties.vendorID != 0x1002)
      std::cerr << "warning: selected device is not an AMD PCI vendor device\n";

    if (!has_extension(physical_device, VK_AMD_SHADER_INFO_EXTENSION_NAME)) {
      std::cerr
          << "VK_AMD_shader_info is not exposed by this device/driver. This is"
             " not a shader failure; use RGA live-driver mode as the alternate"
             " proprietary-driver disassembly path.\n";
      return 3;
    }

    const auto queue_family = find_compute_queue(physical_device);
    if (!queue_family)
      throw std::runtime_error("selected device has no compute-capable queue");

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_ci.queueFamilyIndex = *queue_family;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &queue_priority;

    const char* extensions[] = {VK_AMD_SHADER_INFO_EXTENSION_NAME};
    VkDeviceCreateInfo device_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_ci.queueCreateInfoCount = 1;
    device_ci.pQueueCreateInfos = &queue_ci;
    device_ci.enabledExtensionCount = 1;
    device_ci.ppEnabledExtensionNames = extensions;

    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(physical_device, &device_ci, nullptr, &device),
          "vkCreateDevice");
    struct DeviceGuard {
      VkDevice value = VK_NULL_HANDLE;
      ~DeviceGuard() {
        if (value != VK_NULL_HANDLE)
          vkDestroyDevice(value, nullptr);
      }
    } device_guard{device};

    const auto get_shader_info = reinterpret_cast<PFN_vkGetShaderInfoAMD>(
        vkGetDeviceProcAddr(device, "vkGetShaderInfoAMD"));
    if (!get_shader_info)
      throw std::runtime_error(
          "VK_AMD_shader_info was advertised but vkGetShaderInfoAMD is null");

    VkDescriptorSetLayoutBinding storage_binding{};
    storage_binding.binding = options.descriptor_binding;
    storage_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storage_binding.descriptorCount = 1;
    storage_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout_ci.bindingCount = 1;
    set_layout_ci.pBindings = &storage_binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(device, &set_layout_ci, nullptr,
                                      &set_layout),
          "vkCreateDescriptorSetLayout");
    DeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout>
        set_layout_guard{device, set_layout};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = options.push_constant_bytes;

    VkPipelineLayoutCreateInfo pipeline_layout_ci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_ci.setLayoutCount = 1;
    pipeline_layout_ci.pSetLayouts = &set_layout;
    if (options.push_constant_bytes != 0) {
      pipeline_layout_ci.pushConstantRangeCount = 1;
      pipeline_layout_ci.pPushConstantRanges = &push_range;
    }

    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &pipeline_layout_ci, nullptr,
                                 &pipeline_layout),
          "vkCreatePipelineLayout");
    DeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout> pipeline_layout_guard{
        device, pipeline_layout};

    VkShaderModuleCreateInfo module_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_ci.codeSize = spirv.size() * sizeof(uint32_t);
    module_ci.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &module_ci, nullptr, &module),
          "vkCreateShaderModule");
    DeviceHandle<VkShaderModule, vkDestroyShaderModule> module_guard{device,
                                                                     module};

    VkPipelineShaderStageCreateInfo stage_ci{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage_ci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_ci.module = module;
    stage_ci.pName = options.entry.c_str();

    VkComputePipelineCreateInfo pipeline_ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_ci.stage = stage_ci;
    pipeline_ci.layout = pipeline_layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci,
                                   nullptr, &pipeline),
          "vkCreateComputePipelines");
    DeviceHandle<VkPipeline, vkDestroyPipeline> pipeline_guard{device, pipeline};

    size_t disassembly_size = 0;
    VkResult result = get_shader_info(device, pipeline, VK_SHADER_STAGE_COMPUTE_BIT,
                                      VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD,
                                      &disassembly_size, nullptr);
    if (result == VK_ERROR_FEATURE_NOT_PRESENT) {
      std::cerr << "driver exposes VK_AMD_shader_info but does not provide"
                   " compute-shader disassembly; use RGA live-driver mode.\n";
      return 4;
    }
    check(result, "vkGetShaderInfoAMD(size)");
    if (disassembly_size == 0)
      throw std::runtime_error("driver returned zero disassembly size");

    std::vector<char> disassembly(disassembly_size);
    result = get_shader_info(device, pipeline, VK_SHADER_STAGE_COMPUTE_BIT,
                             VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD,
                             &disassembly_size, disassembly.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
      check(result, "vkGetShaderInfoAMD(data)");

    std::ofstream output(options.output, std::ios::binary);
    if (!output)
      throw std::runtime_error("cannot open output: " + options.output.string());
    output.write(disassembly.data(), static_cast<std::streamsize>(disassembly_size));
    if (!output)
      throw std::runtime_error("failed writing disassembly");

    std::cout << "wrote AMD driver disassembly: " << options.output << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "amd_vulkan_isa_probe: " << e.what() << "\n";
    return 1;
  }
}
