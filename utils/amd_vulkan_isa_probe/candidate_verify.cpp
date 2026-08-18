#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kLanes = 64;
constexpr uint32_t kWordsPerLane = 4;
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
  const auto end = file.tellg();
  if (end <= 0)
    throw std::runtime_error("invalid SPIR-V size");
  const auto size = static_cast<std::streamsize>(end);
  if ((size % 4) != 0)
    throw std::runtime_error("SPIR-V size is not dword-aligned");
  std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(words.data()), size);
  if (!file)
    throw std::runtime_error("failed to read SPIR-V");
  return words;
}

uint32_t byteAt(uint32_t x, uint32_t i) { return (x >> (i * 8u)) & 0xffu; }
uint32_t pack4(uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3) {
  return (b0 & 0xffu) | ((b1 & 0xffu) << 8u) | ((b2 & 0xffu) << 16u) |
         ((b3 & 0xffu) << 24u);
}

uint32_t candidate(unsigned index, uint32_t a, uint32_t b, uint32_t c,
                   uint32_t tid) {
  switch (index) {
  case 0: {
    const uint32_t sh = ((tid % 3u) + 1u) * 8u;
    const uint64_t pair = uint64_t(a) | (uint64_t(b) << 32u);
    return uint32_t(pair >> sh);
  }
  case 1: {
    const uint32_t sh = ((tid % 3u) + 1u) * 8u;
    const uint64_t pair = uint64_t(b) | (uint64_t(a) << 32u);
    return uint32_t(pair >> sh);
  }
  case 2: {
    const uint32_t sh = ((tid % 3u) + 1u) * 8u;
    return (a >> sh) | (b << (32u - sh));
  }
  case 3: {
    const uint32_t sh = ((tid % 3u) + 1u) * 8u;
    return (b >> sh) | (a << (32u - sh));
  }
  case 4: {
    const uint32_t sh = (tid & 30u) + 1u;
    const uint64_t pair = uint64_t(a) | (uint64_t(b) << 32u);
    return uint32_t(pair >> sh);
  }
  case 5: {
    const uint32_t sh = (tid & 30u) + 1u;
    const uint64_t pair = uint64_t(b) | (uint64_t(a) << 32u);
    return uint32_t(pair >> sh);
  }
  case 6: {
    const uint32_t sh = (tid & 30u) + 1u;
    return (a >> sh) | (b << (32u - sh));
  }
  case 7: {
    const uint32_t sh = (tid & 30u) + 1u;
    return (b >> sh) | (a << (32u - sh));
  }
  case 8:
    return pack4(byteAt(a, 1), byteAt(a, 2), byteAt(a, 3), byteAt(b, 0));
  case 9:
    return pack4(byteAt(a, 2), byteAt(a, 3), byteAt(b, 0), byteAt(b, 1));
  case 10:
    return pack4(byteAt(a, 3), byteAt(b, 0), byteAt(b, 1), byteAt(b, 2));
  case 11:
    return pack4(byteAt(a, 3), byteAt(a, 2), byteAt(a, 1), byteAt(a, 0));
  case 12:
    return pack4(byteAt(a, 0), byteAt(b, 0), byteAt(a, 1), byteAt(b, 1));
  case 13:
    return pack4(byteAt(a, 2), byteAt(b, 2), byteAt(a, 3), byteAt(b, 3));
  case 14: {
    const uint32_t s0 = (c >> 0u) & 7u;
    const uint32_t s1 = (c >> 3u) & 7u;
    const uint32_t s2 = (c >> 6u) & 7u;
    const uint32_t s3 = (c >> 9u) & 7u;
    const uint64_t pair = uint64_t(a) | (uint64_t(b) << 32u);
    const auto at = [pair](uint32_t s) {
      return uint32_t((pair >> (s * 8u)) & 0xffu);
    };
    return pack4(at(s0), at(s1), at(s2), at(s3));
  }
  case 15:
    return pack4(byteAt(b, 3), byteAt(a, 0), byteAt(b, 1), byteAt(a, 2));
  case 16:
    return pack4(byteAt(a, 1), byteAt(b, 3), byteAt(a, 0), byteAt(b, 2));
  case 17:
    return (a & 0x00ff00ffu) | (b & 0xff00ff00u);
  case 18:
    return (a & b) | (~a & c);
  case 19: {
    const uint32_t r0 = (byteAt(a, 0) + byteAt(b, 0)) >> 1u;
    const uint32_t r1 = (byteAt(a, 1) + byteAt(b, 1) + 1u) >> 1u;
    const uint32_t r2 = (byteAt(a, 2) + byteAt(b, 2)) >> 1u;
    const uint32_t r3 = (byteAt(a, 3) + byteAt(b, 3) + 1u) >> 1u;
    return pack4(r0, r1, r2, r3);
  }
  default:
    throw std::runtime_error("unknown candidate index");
  }
}

std::array<uint32_t, 4> expected(unsigned index, uint32_t lane) {
  const uint32_t a = lane * 0x9e3779b9u + 0x13579bdfu;
  const uint32_t b = lane * 0x45d9f3bu + 0x2468ace0u;
  const uint32_t c = lane * 0x27d4eb2du + 0x89abcdefu;
  const uint32_t r = candidate(index, a, b, c, lane);
  return {r, r ^ a, r + b, r ^ c};
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t bits,
                        VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred, bool *coherent) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(physical, &props);
  std::optional<uint32_t> fallback;
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) == 0)
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
    throw std::runtime_error("no suitable host-visible Vulkan memory type");
  const auto flags = props.memoryTypes[*fallback].propertyFlags;
  *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  return *fallback;
}

struct State {
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

  ~State() {
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
    if (argc != 3) {
      std::cerr << "usage: amd_vulkan_candidate_verify <shader.spv> <candidate-index>\n";
      return 2;
    }
    const unsigned candidateIndex = static_cast<unsigned>(std::stoul(argv[2]));
    if (candidateIndex > 19)
      throw std::runtime_error("candidate index out of range");
    const auto spirv = readSpirv(argv[1]);
    State s;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "amd_vulkan_candidate_verify";
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
    for (int pass = 0; pass < 2 && !physical; ++pass) {
      for (auto candidateDevice : physicals) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidateDevice, &properties);
        if (pass == 0 && properties.vendorID != 0x1002)
          continue;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidateDevice, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidateDevice, &count, queues.data());
        for (uint32_t i = 0; i < count; ++i) {
          if (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            physical = candidateDevice;
            queueFamily = i;
            std::cout << "device: " << properties.deviceName << '\n';
            break;
          }
        }
        if (physical)
          break;
      }
    }
    if (!physical)
      throw std::runtime_error("no Vulkan compute device");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
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

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;
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
    VkDescriptorBufferInfo dbi{};
    dbi.buffer = s.buffer;
    dbi.offset = 0;
    dbi.range = kBufferSize;
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
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(s.device, &cbai, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkCheck(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayout,
                            0, 1, &descriptor, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkCheck(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    vkCheck(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

    void *mapped = nullptr;
    vkCheck(vkMapMemory(s.device, s.memory, 0, req.size, 0, &mapped), "vkMapMemory");
    if (!coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = s.memory;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      vkCheck(vkInvalidateMappedMemoryRanges(s.device, 1, &range),
              "vkInvalidateMappedMemoryRanges");
    }
    const auto *actual = static_cast<const uint32_t *>(mapped);
    unsigned mismatches = 0;
    for (uint32_t lane = 0; lane < kLanes; ++lane) {
      const auto want = expected(candidateIndex, lane);
      for (uint32_t word = 0; word < kWordsPerLane; ++word) {
        const uint32_t got = actual[lane * kWordsPerLane + word];
        if (got == want[word])
          continue;
        if (mismatches < 16) {
          std::cerr << "mismatch candidate=" << candidateIndex << " lane=" << lane
                    << " word=" << word << " got=0x" << std::hex << got
                    << " expected=0x" << want[word] << std::dec << '\n';
        }
        ++mismatches;
      }
    }
    vkUnmapMemory(s.device, s.memory);
    if (mismatches) {
      std::cerr << "candidate " << candidateIndex << ": FAIL " << mismatches
                << " mismatched dword(s)\n";
      return 1;
    }
    std::cout << "candidate " << candidateIndex << ": runtime semantics PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 2;
  }
}
