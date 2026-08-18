#include <vulkan/vulkan.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kLanes = 64;
constexpr uint32_t kWordsPerLane = 20;
constexpr VkDeviceSize kBufferSize =
    VkDeviceSize{kLanes} * kWordsPerLane * sizeof(uint32_t);

void vkCheck(VkResult result, const char *what) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(what) + " failed: " +
                             std::to_string(result));
}

std::vector<uint32_t> readSpirv(const char *path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::runtime_error(std::string("cannot open SPIR-V: ") + path);
  const auto size = file.tellg();
  if (size <= 0 || (size % 4) != 0)
    throw std::runtime_error("SPIR-V size is invalid");
  std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(words.data()), size);
  if (!file)
    throw std::runtime_error("failed to read SPIR-V");
  return words;
}

uint32_t u8(uint32_t x, unsigned lane) { return (x >> (lane * 8)) & 0xffu; }
uint32_t u16(uint32_t x, unsigned lane) { return (x >> (lane * 16)) & 0xffffu; }
uint32_t absDiff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

uint32_t sadU8(uint32_t a, uint32_t b, uint32_t accum) {
  for (unsigned i = 0; i != 4; ++i)
    accum += absDiff(u8(a, i), u8(b, i));
  return accum;
}
uint32_t sadHiU8(uint32_t a, uint32_t b, uint32_t accum) {
  return accum + (sadU8(a, b, 0) << 16);
}
uint32_t sadU16(uint32_t a, uint32_t b, uint32_t accum) {
  return accum + absDiff(u16(a, 0), u16(b, 0)) +
         absDiff(u16(a, 1), u16(b, 1));
}
uint32_t sadU32(uint32_t a, uint32_t b, uint32_t accum) {
  return accum + absDiff(a, b);
}
uint32_t msadU8(uint32_t source, uint32_t reference, uint32_t accum) {
  for (unsigned i = 0; i != 4; ++i) {
    const uint32_t r = u8(reference, i);
    if (r != 0)
      accum += absDiff(u8(source, i), r);
  }
  return accum;
}

int32_t sign24(uint32_t x) {
  x &= 0x00ffffffu;
  if (x & 0x00800000u)
    x |= 0xff000000u;
  return std::bit_cast<int32_t>(x);
}
uint32_t mulU24(uint32_t a, uint32_t b) {
  return (a & 0x00ffffffu) * (b & 0x00ffffffu);
}
uint32_t mulI24Bits(uint32_t a, uint32_t b) {
  const int64_t product = int64_t(sign24(a)) * int64_t(sign24(b));
  return static_cast<uint32_t>(static_cast<uint64_t>(product));
}
uint32_t mulHiU24(uint32_t a, uint32_t b) {
  const uint64_t product = uint64_t(a & 0x00ffffffu) *
                           uint64_t(b & 0x00ffffffu);
  return static_cast<uint32_t>(product >> 32);
}
uint32_t mulHiI24Bits(uint32_t a, uint32_t b) {
  const int64_t product = int64_t(sign24(a)) * int64_t(sign24(b));
  return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32);
}
uint32_t lerpU8(uint32_t a, uint32_t b, uint32_t rounding) {
  uint32_t out = 0;
  for (unsigned i = 0; i != 4; ++i) {
    const uint32_t r = (rounding >> (i * 8)) & 1u;
    const uint32_t value = (u8(a, i) + u8(b, i) + r) >> 1;
    out |= value << (i * 8);
  }
  return out;
}
uint32_t bfi(uint32_t mask, uint32_t a, uint32_t b) {
  return (mask & a) | (~mask & b);
}

uint64_t packU16x4(const std::array<uint32_t, 4> &x) {
  return uint64_t(x[0] & 0xffffu) |
         (uint64_t(x[1] & 0xffffu) << 16) |
         (uint64_t(x[2] & 0xffffu) << 32) |
         (uint64_t(x[3] & 0xffffu) << 48);
}
uint32_t window32(uint64_t source, unsigned byteOffset) {
  return static_cast<uint32_t>(source >> (byteOffset * 8));
}
uint64_t qsadPk(uint64_t source, uint32_t reference, uint64_t accum,
                bool masked) {
  std::array<uint32_t, 4> out{};
  for (unsigned i = 0; i != 4; ++i) {
    const uint32_t a = static_cast<uint32_t>(accum >> (i * 16)) & 0xffffu;
    out[i] = masked ? msadU8(window32(source, i), reference, a)
                    : sadU8(window32(source, i), reference, a);
  }
  return packU16x4(out);
}
std::array<uint32_t, 4> mqsadU32(uint64_t source, uint32_t reference,
                                 std::array<uint32_t, 4> accum) {
  for (unsigned i = 0; i != 4; ++i)
    accum[i] = msadU8(window32(source, i), reference, accum[i]);
  return accum;
}

std::array<uint32_t, kWordsPerLane> expected(uint32_t lane) {
  const uint32_t a = lane * 0x45d9f3bu + 0x13579bdfu;
  const uint32_t b = lane * 0x27d4eb2du + 0x2468ace1u;
  const uint32_t c = lane * 17u + 3u;
  const uint32_t reference = b ^ 0x00110022u;
  const uint64_t source = (uint64_t(c ^ 0xa5a55a5au) << 32) | a;
  const uint64_t accum16 = (uint64_t((lane + 7u) & 0xffffu) << 48) |
                           (uint64_t((lane + 5u) & 0xffffu) << 32) |
                           (uint64_t((lane + 3u) & 0xffffu) << 16) |
                           uint64_t((lane + 1u) & 0xffffu);
  const std::array<uint32_t, 4> accum32 = {
      lane + 11u, lane + 13u, lane + 17u, lane + 19u};
  const uint64_t q = qsadPk(source, reference, accum16, false);
  const uint64_t mq = qsadPk(source, reference, accum16, true);
  const auto mq32 = mqsadU32(source, reference, accum32);
  const uint64_t signedProduct = static_cast<uint64_t>(
      int64_t(sign24(a)) * int64_t(sign24(b)));

  return {
      mulU24(a, b),
      mulI24Bits(a, b),
      mulHiU24(a, b),
      static_cast<uint32_t>(signedProduct >> 32),
      mulU24(a, b) + c,
      mulI24Bits(a, b) + c,
      lerpU8(a, b, c),
      bfi(a, b, c),
      (a ^ b) + c,
      sadU8(a, b, c),
      sadHiU8(a, b, c),
      sadU16(a, b, c),
      sadU32(a, b, c),
      msadU8(a, reference, c),
      static_cast<uint32_t>(q),
      static_cast<uint32_t>(q >> 32),
      static_cast<uint32_t>(mq),
      static_cast<uint32_t>(mq >> 32),
      mq32[0] ^ mq32[1],
      mq32[2] ^ mq32[3],
  };
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                        VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred,
                        bool *coherent) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
  std::optional<uint32_t> fallback;
  for (uint32_t i = 0; i != props.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) == 0)
      continue;
    const auto flags = props.memoryTypes[i].propertyFlags;
    if ((flags & required) != required)
      continue;
    if ((flags & preferred) == preferred) {
      *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      return i;
    }
    if (!fallback)
      fallback = i;
  }
  if (!fallback)
    throw std::runtime_error("no host-visible Vulkan memory type");
  const auto flags = props.memoryTypes[*fallback].propertyFlags;
  *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  return *fallback;
}

struct VulkanState {
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkCommandPool commandPool = VK_NULL_HANDLE;

  ~VulkanState() {
    if (!device) {
      if (instance)
        vkDestroyInstance(instance, nullptr);
      return;
    }
    if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (shader) vkDestroyShaderModule(device, shader, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
  }
};

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2) {
      std::cerr << "usage: amd_vulkan_math_verify <shader.spv>\n";
      return 2;
    }
    const auto spirv = readSpirv(argv[1]);
    VulkanState s;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "amd_vulkan_math_verify";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    vkCheck(vkCreateInstance(&ici, nullptr, &s.instance), "vkCreateInstance");

    uint32_t physicalCount = 0;
    vkCheck(vkEnumeratePhysicalDevices(s.instance, &physicalCount, nullptr),
            "vkEnumeratePhysicalDevices(count)");
    if (!physicalCount)
      throw std::runtime_error("no Vulkan physical devices");
    std::vector<VkPhysicalDevice> physicals(physicalCount);
    vkCheck(vkEnumeratePhysicalDevices(s.instance, &physicalCount, physicals.data()),
            "vkEnumeratePhysicalDevices");

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t queueFamily = ~0u;
    for (int pass = 0; pass != 2 && !physical; ++pass) {
      for (auto candidate : physicals) {
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        vkGetPhysicalDeviceFeatures(candidate, &features);
        if (!features.shaderInt64 || (pass == 0 && properties.vendorID != 0x1002))
          continue;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
        for (uint32_t i = 0; i != count; ++i) {
          if (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            physical = candidate;
            queueFamily = i;
            std::cout << "device: " << properties.deviceName << "\n";
            break;
          }
        }
        if (physical) break;
      }
    }
    if (!physical)
      throw std::runtime_error("no Vulkan compute device with shaderInt64");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt64 = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enabled;
    vkCheck(vkCreateDevice(physical, &dci, nullptr, &s.device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(s.device, queueFamily, 0, &queue);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = kBufferSize;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(s.device, &bci, nullptr, &s.buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(s.device, s.buffer, &req);
    bool coherent = false;
    const uint32_t memoryType = findMemoryType(
        physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &coherent);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType;
    vkCheck(vkAllocateMemory(s.device, &mai, nullptr, &s.memory), "vkAllocateMemory");
    vkCheck(vkBindBufferMemory(s.device, s.buffer, s.memory, 0), "vkBindBufferMemory");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 1;
    dlci.pBindings = &binding;
    vkCheck(vkCreateDescriptorSetLayout(s.device, &dlci, nullptr, &s.descriptorLayout),
            "vkCreateDescriptorSetLayout");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s.descriptorLayout;
    vkCheck(vkCreatePipelineLayout(s.device, &plci, nullptr, &s.pipelineLayout),
            "vkCreatePipelineLayout");

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spirv.size() * sizeof(uint32_t);
    smci.pCode = spirv.data();
    vkCheck(vkCreateShaderModule(s.device, &smci, nullptr, &s.shader),
            "vkCreateShaderModule");
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = s.shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = stage;
    cpci.layout = s.pipelineLayout;
    vkCheck(vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                     &s.pipeline),
            "vkCreateComputePipelines");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    vkCheck(vkCreateDescriptorPool(s.device, &dpci, nullptr, &s.descriptorPool),
            "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = s.descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &s.descriptorLayout;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    vkCheck(vkAllocateDescriptorSets(s.device, &dsai, &descriptor),
            "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo dbi{s.buffer, 0, kBufferSize};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(s.device, 1, &write, 0, nullptr);

    VkCommandPoolCreateInfo cpool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpool.queueFamilyIndex = queueFamily;
    vkCheck(vkCreateCommandPool(s.device, &cpool, nullptr, &s.commandPool),
            "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = s.commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(s.device, &cbai, &command),
            "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkCheck(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            s.pipelineLayout, 0, 1, &descriptor, 0, nullptr);
    vkCmdDispatch(command, 1, 1, 1);
    vkCheck(vkEndCommandBuffer(command), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    vkCheck(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    vkCheck(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

    void *mapped = nullptr;
    vkCheck(vkMapMemory(s.device, s.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
            "vkMapMemory");
    if (!coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = s.memory;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      vkCheck(vkInvalidateMappedMemoryRanges(s.device, 1, &range),
              "vkInvalidateMappedMemoryRanges");
    }
    const auto *actual = static_cast<const uint32_t *>(mapped);
    for (uint32_t lane = 0; lane != kLanes; ++lane) {
      const auto want = expected(lane);
      for (uint32_t word = 0; word != kWordsPerLane; ++word) {
        const uint32_t got = actual[lane * kWordsPerLane + word];
        if (got != want[word]) {
          std::cerr << "FAIL lane=" << lane << " word=" << word
                    << " expected=0x" << std::hex << want[word]
                    << " actual=0x" << got << std::dec << "\n";
          vkUnmapMemory(s.device, s.memory);
          return 1;
        }
      }
    }
    vkUnmapMemory(s.device, s.memory);
    std::cout << "PASS AMD Vulkan math semantics: " << kLanes
              << " lanes x " << kWordsPerLane << " values\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 3;
  }
}
