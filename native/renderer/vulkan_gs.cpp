#include "vulkan_gs.h"
#include "ps2_runtime.h"
#include "raylib.h" // PNG encoding only; no window or graphics calls.
#include "runtime/gs/ps2_gs_memory.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vulkan/vulkan.h>

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace {
constexpr uint32_t vramSize = 4u * 1024u * 1024u,
                   stagingSize = 16u * 1024u * 1024u;
using Params = std::array<uint32_t, 128>;
void check(VkResult r, const char *where) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(where) + ": Vulkan error " +
                             std::to_string(r));
}
uint32_t bits(float f) { return std::bit_cast<uint32_t>(f); }
uint32_t pixelBits(uint32_t fmt) {
  if (fmt == 20 || fmt == 36 || fmt == 44)
    return 4;
  if (fmt == 19 || fmt == 27)
    return 8;
  if ((fmt & 7) == 2)
    return 16;
  if (fmt == 1 || fmt == 49)
    return 24;
  return 32;
}
GSFrameReg displayFrame(uint64_t r) {
  return {uint32_t(r & 511), uint32_t((r >> 9) & 63), uint8_t((r >> 15) & 31),
          0};
}
} // namespace

struct VulkanGS::Impl {
  struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    void *mapped{};
    VkDeviceSize size{};
  };
  struct Image {
    VkImage handle{};
    VkDeviceMemory memory{};
    VkImageView view{};
    uint32_t w{}, h{};
  };
  mutable std::mutex mutex;
  SDL_Window *window{};
  SDL_GameController *controller{};
  bool fsr, deinterlace, closed = false;
  VkInstance instance{};
  VkSurfaceKHR surface{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family{};
  VkCommandPool pool{};
  VkCommandBuffer cmd{};
  VkFence fence{};
  VkDescriptorPool descriptors{};
  VkDescriptorSetLayout gsSetLayout{}, fsrSetLayout{};
  VkPipelineLayout gsLayout{}, fsrLayout{};
  VkPipeline gsPipeline{}, easuPipeline{}, rcasPipeline{};
  VkDescriptorSet gsSet{}, easuSet{}, rcasSet{};
  VkSampler sampler{};
  Buffer ram, parameters, input, scratch, palette, readback;
  Image nativeImage, scaledImage, finalImage;
  VkSwapchainKHR swapchain{};
  VkExtent2D extent{};
  std::vector<VkImage> swapImages;
  VkSemaphore acquired{};
  std::vector<VkSemaphore> ready;
  bool recording = false;
  uint32_t paramCursor = 0, inputCursor = 0;
  uint8_t *mirror{};
  std::string name;
  GSTransferCommand transfer{};
  uint32_t copied = 0, total = 0;
  std::vector<uint8_t> pending, download;
  size_t downloadPos = 0;
  std::array<uint32_t, 2> cbp{0xffffffffu, 0xffffffffu};
  std::array<uint8_t, 32> pad{};
  mutable std::mutex padMutex;
  std::atomic<uint64_t> draws{0};
  uint64_t presented = 0, lastTick = ~0ull;
  uint32_t srcWidth = 640, srcHeight = 448, outWidth = 640, outHeight = 448;
  uint32_t requestedWidth = 0, requestedHeight = 0;

  Impl(bool wantWindow, bool wantFsr, bool wantDeinterlace)
      : fsr(wantFsr), deinterlace(wantDeinterlace) {
    try {
      create(wantWindow);
    } catch (...) {
      destroy();
      throw;
    }
  }
  ~Impl() { destroy(); }
  uint32_t memoryType(uint32_t mask, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
      if ((mask & (1u << i)) &&
          (props.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("Vulkan: required memory type unavailable");
  }
  Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                bool host = false) {
    Buffer b{};
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    check(vkCreateBuffer(device, &ci, nullptr, &b.handle), "create buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.handle, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(
        req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &ai, nullptr, &b.memory), "allocate buffer");
    check(vkBindBufferMemory(device, b.handle, b.memory, 0), "bind buffer");
    if (host)
      check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped), "map buffer");
    return b;
  }
  void freeBuffer(Buffer &b) {
    if (b.mapped)
      vkUnmapMemory(device, b.memory);
    if (b.handle)
      vkDestroyBuffer(device, b.handle, nullptr);
    if (b.memory)
      vkFreeMemory(device, b.memory, nullptr);
    b = {};
  }
  Image image(uint32_t w, uint32_t h) {
    Image im{};
    im.w = w;
    im.h = h;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    check(vkCreateImage(device, &ci, nullptr, &im.handle), "create image");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, im.handle, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &ai, nullptr, &im.memory), "allocate image");
    check(vkBindImageMemory(device, im.handle, im.memory, 0), "bind image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.handle;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device, &vi, nullptr, &im.view), "image view");
    begin();
    transition(im.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return im;
  }
  void freeImage(Image &im) {
    if (im.view)
      vkDestroyImageView(device, im.view, nullptr);
    if (im.handle)
      vkDestroyImage(device, im.handle, nullptr);
    if (im.memory)
      vkFreeMemory(device, im.memory, nullptr);
    im = {};
  }
  void barrier() {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
  }
  void transition(VkImage im, VkImageLayout old, VkImageLayout next) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask =
        old == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = old;
    b.newLayout = next;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }
  void begin() {
    if (recording)
      return;
    check(vkResetCommandPool(device, pool, 0), "reset pool");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bi), "begin commands");
    recording = true;
  }
  void finish(VkSemaphore wait = {}, VkSemaphore signal = {}) {
    if (!recording)
      return;
    check(vkEndCommandBuffer(cmd), "end commands");
    check(vkResetFences(device, 1, &fence), "reset fence");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (wait) {
      si.waitSemaphoreCount = 1;
      si.pWaitSemaphores = &wait;
      si.pWaitDstStageMask = &stage;
    }
    if (signal) {
      si.signalSemaphoreCount = 1;
      si.pSignalSemaphores = &signal;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(queue, 1, &si, fence), "submit compute");
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
          "wait compute");
    recording = false;
    paramCursor = inputCursor = 0;
  }
  void dispatch(uint32_t op, const Params &p, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0)
      return;
    if (paramCursor + sizeof(p) > parameters.size)
      finish();
    begin();
    memcpy(static_cast<uint8_t *>(parameters.mapped) + paramCursor, p.data(),
           sizeof(p));
    std::array<uint32_t, 4> push = {paramCursor / 4, op, w, h};
    paramCursor += sizeof(p);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gsPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gsLayout, 0, 1,
                            &gsSet, 0, nullptr);
    vkCmdPushConstants(cmd, gsLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(push), push.data());
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    barrier();
  }
  VkPipeline pipeline(const char *file, VkPipelineLayout layout) {
    std::ifstream in(std::string(RATCHET_SHADER_DIR) + "/" + file,
                     std::ios::binary | std::ios::ate);
    if (!in)
      throw std::runtime_error(std::string("Missing Vulkan shader: ") + file);
    auto size = static_cast<size_t>(in.tellg());
    std::vector<uint32_t> code((size + 3) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char *>(code.data()), size);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size;
    mi.pCode = code.data();
    VkShaderModule module{};
    check(vkCreateShaderModule(device, &mi, nullptr, &module), "shader module");
    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.layout = layout;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_COMPUTE_BIT,
                module,
                "main",
                nullptr};
    VkPipeline result{};
    VkResult status =
        vkCreateComputePipelines(device, {}, 1, &ci, nullptr, &result);
    vkDestroyShaderModule(device, module, nullptr);
    check(status, "compute pipeline");
    return result;
  }
  VkDescriptorSet allocateSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set{};
    check(vkAllocateDescriptorSets(device, &ai, &set), "descriptor set");
    return set;
  }
  void imageDescriptors() {
    VkDescriptorImageInfo scan{{}, nativeImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = gsSet;
    wr.dstBinding = 5;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr.pImageInfo = &scan;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
    for (int stage = 0; stage < 2; stage++) {
      VkDescriptorSet set = stage ? rcasSet : easuSet;
      VkDescriptorImageInfo infos[2] = {
          {sampler, stage ? scaledImage.view : nativeImage.view,
           VK_IMAGE_LAYOUT_GENERAL},
          {{},
           stage ? finalImage.view : scaledImage.view,
           VK_IMAGE_LAYOUT_GENERAL}};
      VkWriteDescriptorSet writes[2]{};
      for (int j = 0; j < 2; j++) {
        writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[j].dstSet = set;
        writes[j].dstBinding = j;
        writes[j].descriptorCount = 1;
        writes[j].descriptorType =
            j ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[j].pImageInfo = &infos[j];
      }
      vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }
  }
  void resizeImages(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    if (nativeImage.w == w && nativeImage.h == h && finalImage.w == ow &&
        finalImage.h == oh)
      return;
    finish();
    check(vkDeviceWaitIdle(device), "resize idle");
    freeImage(nativeImage);
    freeImage(scaledImage);
    freeImage(finalImage);
    nativeImage = image(w, h);
    scaledImage = image(ow, oh);
    finalImage = image(ow, oh);
    finish();
    imageDescriptors();
    srcWidth = w;
    srcHeight = h;
    outWidth = ow;
    outHeight = oh;
  }
  void create(bool wantWindow) {
    std::vector<const char *> extensions;
    if (wantWindow) {
      if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
        throw std::runtime_error(SDL_GetError());
      window = SDL_CreateWindow(
          "Ratchet & Clank 3 — Vulkan", SDL_WINDOWPOS_CENTERED,
          SDL_WINDOWPOS_CENTERED, 960, 720,
          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
      if (!window)
        throw std::runtime_error(SDL_GetError());
      unsigned count = 0;
      if (!SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr))
        throw std::runtime_error(SDL_GetError());
      extensions.resize(count);
      if (!SDL_Vulkan_GetInstanceExtensions(window, &count, extensions.data()))
        throw std::runtime_error(SDL_GetError());
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Ratchet native Vulkan GS";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = extensions.size();
    ici.ppEnabledExtensionNames = extensions.data();
    check(vkCreateInstance(&ici, nullptr, &instance), "create instance");
    if (window && !SDL_Vulkan_CreateSurface(window, instance, &surface))
      throw std::runtime_error(SDL_GetError());
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr),
          "enumerate GPUs");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
          "enumerate GPUs");
    int best = -1;
    for (auto candidate : devices) {
      VkPhysicalDeviceProperties prop;
      vkGetPhysicalDeviceProperties(candidate, &prop);
      if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        continue;
      uint32_t qc = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qc, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qc);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qc, qs.data());
      for (uint32_t i = 0; i < qc; i++) {
        VkBool32 present = VK_TRUE;
        if (surface)
          check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface,
                                                     &present),
                "surface support");
        if (!(qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
            !(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present)
          continue;
        int score =
            prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > best) {
          best = score;
          physical = candidate;
          family = i;
          name = prop.deviceName;
        }
      }
    }
    if (!physical)
      throw std::runtime_error(
          "No hardware Vulkan GPU supporting compute and presentation");
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char *swapExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VkPhysicalDeviceFeatures available;
    vkGetPhysicalDeviceFeatures(physical, &available);
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderStorageImageExtendedFormats =
        available.shaderStorageImageExtendedFormats;
    di.pEnabledFeatures = &enabled;
    if (surface) {
      di.enabledExtensionCount = 1;
      di.ppEnabledExtensionNames = &swapExtension;
    }
    check(vkCreateDevice(physical, &di, nullptr, &device), "create device");
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(device, &pi, nullptr, &pool), "command pool");
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &ca, &cmd), "command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device, &fi, nullptr, &fence), "fence");
    ram = buffer(vramSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    scratch = buffer(vramSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    palette = buffer(2048, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    parameters = buffer(stagingSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    input = buffer(stagingSize,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   true);
    readback = buffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    std::array<VkDescriptorPoolSize, 3> sizes = {
        {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5},
         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}}};
    VkDescriptorPoolCreateInfo dpi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 3;
    dpi.poolSizeCount = sizes.size();
    dpi.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors),
          "descriptor pool");
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    for (uint32_t i = 0; i < 6; i++)
      bindings[i] = {i,
                     i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = bindings.size();
    li.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device, &li, nullptr, &gsSetLayout),
          "GS layout");
    std::array<VkDescriptorSetLayoutBinding, 2> fb = {
        {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
          VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
          nullptr}}};
    li.bindingCount = fb.size();
    li.pBindings = fb.data();
    check(vkCreateDescriptorSetLayout(device, &li, nullptr, &fsrSetLayout),
          "FSR layout");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &gsSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &pl, nullptr, &gsLayout),
          "GS pipeline layout");
    push.size = 80;
    pl.pSetLayouts = &fsrSetLayout;
    check(vkCreatePipelineLayout(device, &pl, nullptr, &fsrLayout),
          "FSR pipeline layout");
    gsPipeline = pipeline("gs.spv", gsLayout);
    easuPipeline = pipeline("easu.spv", fsrLayout);
    rcasPipeline = pipeline("rcas.spv", fsrLayout);
    gsSet = allocateSet(gsSetLayout);
    easuSet = allocateSet(fsrSetLayout);
    rcasSet = allocateSet(fsrSetLayout);
    std::array<VkDescriptorBufferInfo, 5> infos = {
        {{ram.handle, 0, ram.size},
         {parameters.handle, 0, parameters.size},
         {input.handle, 0, input.size},
         {scratch.handle, 0, scratch.size},
         {palette.handle, 0, palette.size}}};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t i = 0; i < 5; i++) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = gsSet;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = 0;
    check(vkCreateSampler(device, &sc, nullptr, &sampler), "sampler");
    resizeImages(640, 448, 640, 448);
    if (surface) {
      VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      check(vkCreateSemaphore(device, &s, nullptr, &acquired),
            "acquire semaphore");
      recreateSwapchain();
    }
    pad.fill(0);
    pad[0] = 1;
    pad[1] = 0x73;
    pad[2] = pad[3] = 255;
    pad[4] = pad[5] = pad[6] = pad[7] = 128;
    std::cout << "[ratchet:vulkan] device=" << name
              << " raster=compute fsr=" << (fsr ? "AMD FSR1 EASU+RCAS" : "off")
              << " deinterlace=" << (deinterlace ? "bob" : "off") << '\n'
              << std::flush;
  }
  void recreateSwapchain() {
    finish();
    check(vkDeviceWaitIdle(device), "swapchain idle");
    int w, h;
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    if (w < 1 || h < 1)
      return;
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps),
          "surface capabilities");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
      throw std::runtime_error("Vulkan surface cannot receive rendered image");
    uint32_t n = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr),
          "surface formats");
    std::vector<VkSurfaceFormatKHR> formats(n);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n,
                                               formats.data()),
          "surface formats");
    auto format = formats.front();
    for (auto f : formats)
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM ||
          f.format == VK_FORMAT_R8G8B8A8_UNORM) {
        format = f;
        break;
      }
    extent = caps.currentExtent.width == UINT32_MAX
                 ? VkExtent2D{uint32_t(w), uint32_t(h)}
                 : caps.currentExtent;
    extent.width = std::clamp(extent.width, caps.minImageExtent.width,
                              caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height,
                               caps.maxImageExtent.height);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount)
      ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
        caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR next{};
    check(vkCreateSwapchainKHR(device, &ci, nullptr, &next), "swapchain");
    if (swapchain)
      vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = next;
    for (auto s : ready)
      vkDestroySemaphore(device, s, nullptr);
    ready.clear();
    check(vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr),
          "swapchain images");
    swapImages.resize(n);
    check(vkGetSwapchainImagesKHR(device, swapchain, &n, swapImages.data()),
          "swapchain images");
    for (uint32_t i = 0; i < n; i++) {
      VkSemaphore s;
      VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      check(vkCreateSemaphore(device, &ci, nullptr, &s), "present semaphore");
      ready.push_back(s);
    }
  }
  void upscale() {
    if (!fsr)
      return;
    std::array<uint32_t, 20> con{};
    con[16] = outWidth;
    con[17] = outHeight;
    FsrEasuCon(con.data(), con.data() + 4, con.data() + 8, con.data() + 12,
               float(srcWidth), float(srcHeight), float(srcWidth),
               float(srcHeight), float(outWidth), float(outHeight));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, easuPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsrLayout, 0,
                            1, &easuSet, 0, nullptr);
    vkCmdPushConstants(cmd, fsrLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(con), con.data());
    vkCmdDispatch(cmd, (outWidth + 7) / 8, (outHeight + 7) / 8, 1);
    barrier();
    FsrRcasCon(con.data(), 0.5f);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rcasPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsrLayout, 0,
                            1, &rcasSet, 0, nullptr);
    vkCmdPushConstants(cmd, fsrLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(con), con.data());
    vkCmdDispatch(cmd, (outWidth + 7) / 8, (outHeight + 7) / 8, 1);
    barrier();
  }
  void display() {
    if (!swapchain) {
      finish();
      return;
    }
    uint32_t index = 0;
    VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                             acquired, {}, &index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
      finish();
      recreateSwapchain();
      return;
    }
    if (acquire != VK_SUBOPTIMAL_KHR)
      check(acquire, "acquire image");
    begin();
    auto &im = fsr ? finalImage : nativeImage;
    transition(im.handle, VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(swapImages[index], VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    black.float32[3] = 1;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, swapImages[index],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &range);
    barrier();
    // PAL pixels are non-square. Display the original 4:3 picture correctly.
    uint32_t dh = extent.height, dw = dh * 4 / 3;
    if (dw > extent.width) {
      dw = extent.width;
      dh = dw * 3 / 4;
    }
    int32_t left = (extent.width - dw) / 2, top = (extent.height - dh) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {int32_t(im.w), int32_t(im.h), 1};
    blit.dstOffsets[0] = {left, top, 0};
    blit.dstOffsets[1] = {left + int32_t(dw), top + int32_t(dh), 1};
    vkCmdBlitImage(cmd, im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
    transition(im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_GENERAL);
    transition(swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    finish(acquired, ready[index]);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &ready[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    VkResult status = vkQueuePresentKHR(queue, &present);
    if (status == VK_ERROR_OUT_OF_DATE_KHR || status == VK_SUBOPTIMAL_KHR ||
        acquire == VK_SUBOPTIMAL_KHR)
      recreateSwapchain();
    else
      check(status, "present");
  }
  void readBuffer(const Buffer &b, uint32_t bytes, uint32_t offset = 0) {
    begin();
    barrier();
    VkBufferCopy copy{offset, 0, bytes};
    vkCmdCopyBuffer(cmd, b.handle, readback.handle, 1, &copy);
    barrier();
    finish();
  }
  void destroy() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (acquired)
        vkDestroySemaphore(device, acquired, nullptr);
      for (auto s : ready)
        vkDestroySemaphore(device, s, nullptr);
      if (swapchain)
        vkDestroySwapchainKHR(device, swapchain, nullptr);
      freeImage(nativeImage);
      freeImage(scaledImage);
      freeImage(finalImage);
      freeBuffer(ram);
      freeBuffer(parameters);
      freeBuffer(input);
      freeBuffer(scratch);
      freeBuffer(palette);
      freeBuffer(readback);
      if (gsPipeline)
        vkDestroyPipeline(device, gsPipeline, nullptr);
      if (easuPipeline)
        vkDestroyPipeline(device, easuPipeline, nullptr);
      if (rcasPipeline)
        vkDestroyPipeline(device, rcasPipeline, nullptr);
      if (gsLayout)
        vkDestroyPipelineLayout(device, gsLayout, nullptr);
      if (fsrLayout)
        vkDestroyPipelineLayout(device, fsrLayout, nullptr);
      if (descriptors)
        vkDestroyDescriptorPool(device, descriptors, nullptr);
      if (gsSetLayout)
        vkDestroyDescriptorSetLayout(device, gsSetLayout, nullptr);
      if (fsrSetLayout)
        vkDestroyDescriptorSetLayout(device, fsrSetLayout, nullptr);
      if (sampler)
        vkDestroySampler(device, sampler, nullptr);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
      if (pool)
        vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (surface)
      vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance)
      vkDestroyInstance(instance, nullptr);
    if (controller)
      SDL_GameControllerClose(controller);
    if (window)
      SDL_DestroyWindow(window);
  }
};

VulkanGS::VulkanGS(bool window, bool fsr, bool deinterlace)
    : m(std::make_unique<Impl>(window, fsr, deinterlace)) {}
VulkanGS::~VulkanGS() = default;
const std::string &VulkanGS::deviceName() const { return m->name; }
uint64_t VulkanGS::drawCount() const { return m->draws.load(); }
void VulkanGS::setOutputSize(uint32_t width, uint32_t height) {
  if (width < 1 || height < 1 || uint64_t(width) * height * 4 > stagingSize)
    throw std::runtime_error("Invalid Vulkan output size");
  std::lock_guard lock(m->mutex);
  m->requestedWidth = width;
  m->requestedHeight = height;
}
void VulkanGS::open(PS2Runtime &runtime, const char *title) {
  if (m->window)
    SDL_SetWindowTitle(m->window, title);
  runtime.padBackend().setInputProvider(
      [this](int port, int slot, uint8_t *dst, size_t size) {
        if (!dst || size < 32 || port != 0 || slot != 0)
          return false;
        std::lock_guard lock(m->padMutex);
        memcpy(dst, m->pad.data(), 32);
        return true;
      });
}
bool VulkanGS::frame(PS2Runtime &runtime) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT)
      m->closed = true;
    if (event.type == SDL_WINDOWEVENT &&
        (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
         event.window.event == SDL_WINDOWEVENT_RESTORED)) {
      std::lock_guard lock(m->mutex);
      m->recreateSwapchain();
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED && !m->controller)
      m->controller = SDL_GameControllerOpen(event.cdevice.which);
    if (event.type == SDL_CONTROLLERDEVICEREMOVED && m->controller &&
        !SDL_GameControllerGetAttached(m->controller)) {
      SDL_GameControllerClose(m->controller);
      m->controller = nullptr;
    }
  }
  uint16_t buttons = 0xffff;
  const uint8_t *keys = SDL_GetKeyboardState(nullptr);
  auto key = [&](uint16_t mask, std::initializer_list<SDL_Scancode> codes) {
    for (auto c : codes)
      if (keys[c])
        buttons &= ~mask;
  };
  if (SDL_GetWindowFlags(m->window) & SDL_WINDOW_INPUT_FOCUS) {
    key(0x80, {SDL_SCANCODE_LEFT, SDL_SCANCODE_A});
    key(0x40, {SDL_SCANCODE_DOWN, SDL_SCANCODE_S});
    key(0x20, {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D});
    key(0x10, {SDL_SCANCODE_UP, SDL_SCANCODE_W});
    key(8, {SDL_SCANCODE_RETURN});
    key(1, {SDL_SCANCODE_TAB});
    key(0x4000, {SDL_SCANCODE_X, SDL_SCANCODE_SPACE});
    key(0x2000, {SDL_SCANCODE_C, SDL_SCANCODE_ESCAPE});
    key(0x8000, {SDL_SCANCODE_Z});
    key(0x1000, {SDL_SCANCODE_V});
    key(0x400, {SDL_SCANCODE_Q});
    key(0x800, {SDL_SCANCODE_E});
    key(0x100, {SDL_SCANCODE_LSHIFT});
    key(0x200, {SDL_SCANCODE_RSHIFT});
  }
  std::array<uint8_t, 4> sticks{128, 128, 128, 128};
  if (m->controller) {
    static constexpr std::array<std::pair<SDL_GameControllerButton, uint16_t>,
                                14>
        mapping = {{{SDL_CONTROLLER_BUTTON_A, 0x4000},
                    {SDL_CONTROLLER_BUTTON_B, 0x2000},
                    {SDL_CONTROLLER_BUTTON_X, 0x8000},
                    {SDL_CONTROLLER_BUTTON_Y, 0x1000},
                    {SDL_CONTROLLER_BUTTON_START, 8},
                    {SDL_CONTROLLER_BUTTON_BACK, 1},
                    {SDL_CONTROLLER_BUTTON_LEFTSTICK, 2},
                    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 4},
                    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x400},
                    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x800},
                    {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x10},
                    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x20},
                    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x40},
                    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x80}}};
    for (auto [button, mask] : mapping)
      if (SDL_GameControllerGetButton(m->controller, button))
        buttons &= ~mask;
    if (SDL_GameControllerGetAxis(m->controller,
                                  SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192)
      buttons &= ~0x100;
    if (SDL_GameControllerGetAxis(m->controller,
                                  SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192)
      buttons &= ~0x200;
    sticks = {uint8_t((int(SDL_GameControllerGetAxis(
                           m->controller, SDL_CONTROLLER_AXIS_RIGHTX)) +
                       32768) >>
                      8),
              uint8_t((int(SDL_GameControllerGetAxis(
                           m->controller, SDL_CONTROLLER_AXIS_RIGHTY)) +
                       32768) >>
                      8),
              uint8_t((int(SDL_GameControllerGetAxis(
                           m->controller, SDL_CONTROLLER_AXIS_LEFTX)) +
                       32768) >>
                      8),
              uint8_t((int(SDL_GameControllerGetAxis(
                           m->controller, SDL_CONTROLLER_AXIS_LEFTY)) +
                       32768) >>
                      8)};
  }
  {
    std::lock_guard lock(m->padMutex);
    m->pad[2] = buttons & 255;
    m->pad[3] = buttons >> 8;
    std::copy(sticks.begin(), sticks.end(), m->pad.begin() + 4);
  }
  if (m->closed)
    return false;
  if (SDL_GetWindowFlags(m->window) & SDL_WINDOW_MINIMIZED) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return true;
  }
  uint64_t tick = runtime.memory().gs().vsyncTick.load();
  if (tick != m->lastTick) {
    m->lastTick = tick;
    runtime.gs().latchHostPresentationFrame();
  } else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return true;
}
void VulkanGS::Initialize(uint8_t *vram, uint32_t size) {
  if (!vram || size != vramSize)
    throw std::runtime_error("Invalid GS memory");
  std::lock_guard lock(m->mutex);
  m->mirror = vram;
  m->finish();
  memcpy(m->input.mapped, vram, size);
  m->begin();
  VkBufferCopy copy{0, 0, size};
  vkCmdCopyBuffer(m->cmd, m->input.handle, m->ram.handle, 1, &copy);
  m->barrier();
  m->finish();
}
void VulkanGS::Reset() {
  std::lock_guard lock(m->mutex);
  m->begin();
  vkCmdFillBuffer(m->cmd, m->palette.handle, 0, 2048, 0);
  m->barrier();
  m->finish();
  m->pending.clear();
  m->download.clear();
  m->transfer = {};
  m->copied = m->total = 0;
  m->cbp.fill(0u);
  m->lastTick = ~0ull;
}
void VulkanGS::Submit(const GSPrimitiveBatch &batch) {
  if (!batch.vertexCount)
    return;
  std::lock_guard lock(m->mutex);
  const auto &s = batch.state;
  const auto &c = s.context;
  Params p{};
  p[0] = s.prim.tme | s.prim.iip << 1 | s.prim.fst << 2 | s.prim.abe << 3 |
         s.prim.fge << 4;
  p[1] = s.prim.type;
  p[4] = c.frame.fbp * 32;
  p[5] = std::max(c.frame.fbw, 1u);
  p[6] = c.frame.psm;
  p[7] = c.frame.fbmsk;
  p[8] = c.zbuf.zbp * 32;
  p[9] = c.zbuf.psm;
  p[10] = c.zbuf.zmask;
  p[12] = c.test;
  p[13] = c.alpha;
  p[14] = c.alpha >> 32;
  p[15] = c.fba & 1;
  p[16] = s.pabe;
  p[17] = s.scanmsk & 3;
  p[18] = s.fogR | s.fogG << 8 | s.fogB << 16;
  p[19] = s.colclamp;
  p[20] = c.tex0.tbp0;
  p[21] = c.tex0.tbw;
  p[22] = c.tex0.psm;
  p[23] = c.tex0.tfx;
  p[24] = c.tex0.tcc;
  p[25] = c.tex0.cpsm;
  p[26] = c.tex0.csm;
  p[27] = c.tex0.csa;
  p[29] = std::max<uint32_t>(s.textureWidth, 1);
  p[30] = std::max<uint32_t>(s.textureHeight, 1);
  p[31] = s.linearFilter;
  p[32] = s.texa.ta0;
  p[33] = s.texa.aem;
  p[34] = s.texa.ta1;
  p[35] = c.clamp;
  p[36] = c.clamp >> 32;
  float minX = 4096, minY = 4096, maxX = -1, maxY = -1;
  for (uint32_t i = 0; i < 3; i++) {
    const auto &v = batch.vertices[i];
    uint32_t k = 64 + i * 16;
    float x = v.x - float(c.xyoffset.ofx >> 4),
          y = v.y - float(c.xyoffset.ofy >> 4);
    p[k] = bits(x);
    p[k + 1] = bits(y);
    p[k + 2] = uint32_t(v.z);
    p[k + 3] = v.r | v.g << 8 | v.b << 16 | v.a << 24;
    p[k + 5] = bits(v.q);
    p[k + 6] = bits(v.s);
    p[k + 7] = bits(v.t);
    p[k + 8] = v.u;
    p[k + 9] = v.v;
    p[k + 10] = v.fog;
    if (i < batch.vertexCount) {
      minX = std::min(minX, x);
      minY = std::min(minY, y);
      maxX = std::max(maxX, x);
      maxY = std::max(maxY, y);
    }
  }
  int x0 = std::max<int>(c.scissor.x0, std::floor(minX)),
      y0 = std::max<int>(c.scissor.y0, std::floor(minY));
  int x1 = std::min<int>(c.scissor.x1, std::ceil(maxX)),
      y1 = std::min<int>(c.scissor.y1, std::ceil(maxY));
  if (s.prim.type == GS_PRIM_SPRITE) {
    x1 = std::min<int>(c.scissor.x1, int(maxX) - 1);
    y1 = std::min<int>(c.scissor.y1, int(maxY) - 1);
  }
  if (x1 < x0 || y1 < y0)
    return;
  p[40] = x0;
  p[41] = y0;
  m->dispatch(0, p, x1 - x0 + 1, y1 - y0 + 1);
  ++m->draws;
}
void VulkanGS::LoadClut(const GSTex0Reg &t, const GSTexClutReg &c) {
  if (t.psm != 19 && t.psm != 20 && t.psm != 27 && t.psm != 36 && t.psm != 44)
    return;
  std::lock_guard lock(m->mutex);
  if (t.cld == 0 || t.cld >= 6)
    return;
  if (t.cld >= 2) {
    uint32_t slot = (t.cld == 3 || t.cld == 5) ? 1 : 0;
    if (t.cld >= 4 && m->cbp[slot] == t.cbp)
      return;
    m->cbp[slot] = t.cbp;
  }
  Params p{};
  p[0] = t.psm;
  p[1] = t.cbp;
  p[3] = t.cpsm;
  p[4] = t.csm;
  p[5] = t.csa;
  p[6] = std::max<uint32_t>(c.cbw, 1);
  p[7] = c.cou;
  p[8] = c.cov;
  m->dispatch(2, p, 256, 1);
}
void VulkanGS::BeginTransfer(const GSTransferCommand &c) {
  std::lock_guard lock(m->mutex);
  m->transfer = c;
  m->copied = 0;
  m->total = uint32_t(c.trxreg.rrw) * c.trxreg.rrh;
  m->pending.clear();
  m->download.clear();
  m->downloadPos = 0;
  if (!m->total || c.direction == 0 || c.direction == 3)
    return;
  Params p{};
  p[0] = c.bitbltbuf.spsm;
  p[1] = c.bitbltbuf.sbp;
  p[2] = std::max<uint32_t>(c.bitbltbuf.sbw, 1);
  p[3] = c.trxpos.ssax;
  p[4] = c.trxpos.ssay;
  m->dispatch(3, p, c.trxreg.rrw, c.trxreg.rrh);
  if (c.direction == 2) {
    p[0] = c.bitbltbuf.dpsm;
    p[1] = c.bitbltbuf.dbp;
    p[2] = std::max<uint32_t>(c.bitbltbuf.dbw, 1);
    p[3] = c.trxpos.dsax;
    p[4] = c.trxpos.dsay;
    m->dispatch(4, p, c.trxreg.rrw, c.trxreg.rrh);
  } else {
    m->readBuffer(m->scratch, m->total * 4);
    auto *values = static_cast<uint32_t *>(m->readback.mapped);
    uint32_t bits = pixelBits(c.bitbltbuf.spsm);
    m->download.assign((m->total * bits + 7) / 8, 0);
    for (uint32_t i = 0; i < m->total; i++) {
      uint32_t at = i * bits;
      uint64_t v = uint64_t(values[i]) << (at & 7);
      for (uint32_t j = 0; j < (bits + (at & 7) + 7) / 8; j++)
        m->download[at / 8 + j] |= v >> (j * 8);
    }
  }
  m->copied = m->total;
}
void VulkanGS::UploadImage(const uint8_t *data, uint32_t bytes) {
  std::lock_guard lock(m->mutex);
  auto &c = m->transfer;
  if (c.direction != 0 || m->copied >= m->total || !bytes)
    return;
  m->pending.insert(m->pending.end(), data, data + bytes);
  uint32_t b = pixelBits(c.bitbltbuf.dpsm),
           count = std::min<uint32_t>(m->pending.size() * 8 / b,
                                      m->total - m->copied);
  if (b == 4 && count % 2 && count != m->total - m->copied)
    --count;
  if (!count)
    return;
  uint32_t used = (count * b + 7) / 8, aligned = (used + 7) & ~3u;
  if (m->inputCursor + aligned > m->input.size ||
      m->paramCursor + sizeof(Params) > m->parameters.size)
    m->finish();
  if (aligned > m->input.size)
    throw std::runtime_error("GS upload exceeds staging capacity");
  memcpy(static_cast<uint8_t *>(m->input.mapped) + m->inputCursor,
         m->pending.data(), used);
  memset(static_cast<uint8_t *>(m->input.mapped) + m->inputCursor + used, 0,
         aligned - used);
  Params p{};
  p[0] = c.bitbltbuf.dpsm;
  p[1] = c.bitbltbuf.dbp;
  p[2] = std::max<uint32_t>(c.bitbltbuf.dbw, 1);
  p[3] = c.trxpos.dsax;
  p[4] = c.trxpos.dsay;
  p[5] = c.trxreg.rrw;
  p[6] = m->copied;
  p[8] = m->inputCursor / 4;
  m->inputCursor += aligned;
  m->dispatch(1, p, count, 1);
  m->copied += count;
  m->pending.erase(m->pending.begin(), m->pending.begin() + used);
}
void VulkanGS::Flush() {}
void VulkanGS::TextureFlush() {
  std::lock_guard lock(m->mutex);
  m->begin();
  m->barrier();
}
void VulkanGS::Sync(GSSyncReason reason) {
  if (reason == GSSyncReason::Presentation)
    return;
  std::lock_guard lock(m->mutex);
  m->finish();
  if (reason == GSSyncReason::DebugReadback && m->mirror) {
    m->readBuffer(m->ram, vramSize);
    memcpy(m->mirror, m->readback.mapped, vramSize);
  }
}
PresentationFrame VulkanGS::Present(const GSPresentationRequest &r) {
  std::lock_guard lock(m->mutex);
  PresentationFrame frame{};
  bool crt1 = (r.pmode & 1) != 0;
  if (!(r.pmode & 3))
    return frame;
  uint64_t disp = crt1 ? r.display1 : r.display2,
           fb = crt1 ? r.dispfb1 : r.dispfb2;
  GSFrameReg source = displayFrame(fb);
  frame.displayFbp = source.fbp;
  uint32_t w = uint32_t((disp >> 32) & 4095) + 1;
  w /= uint32_t((disp >> 23) & 15) + 1;
  uint32_t h =
      (uint32_t((disp >> 44) & 2047) + 1) / (uint32_t((disp >> 27) & 3) + 1);
  if (w < 64 || h < 64) {
    w = 640;
    h = 448;
  }
  w = std::min(w, 1024u);
  h = std::min(h, 1024u);
  uint32_t ow = w, oh = h;
  if (m->window) {
    ow = m->extent.width;
    oh = m->extent.height;
    if (ow > oh * 4 / 3)
      ow = oh * 4 / 3;
    else
      oh = ow * 3 / 4;
    ow = std::max(ow, w);
    oh = std::max(oh, h);
  }
  if (m->requestedWidth) {
    ow = std::max(w, m->requestedWidth);
    oh = std::max(h, m->requestedHeight);
  }
  if (!m->fsr) {
    ow = w;
    oh = h;
  }
  m->resizeImages(w, h, ow, oh);
  Params p{};
  p[0] = source.psm;
  p[1] = source.fbp * 32;
  p[2] = std::max(source.fbw, 1u);
  p[3] = (fb >> 32) & 2047;
  p[4] = (fb >> 43) & 2047;
  p[6] = m->deinterlace && (r.smode2 & 1);
  p[7] = r.vsyncTick & 1;
  auto scan = [&]() {
    m->begin();
    vkCmdFillBuffer(m->cmd, m->scratch.handle, vramSize - 4, 4, 0);
    m->barrier();
    m->dispatch(5, p, w, h);
  };
  if (r.hasPreferredSource && r.preferredDestFbp == source.fbp) {
    source = r.preferredSource;
    p[0] = source.psm;
    p[1] = source.fbp * 32;
    p[2] = std::max(source.fbw, 1u);
    p[3] = p[4] = 0;
    frame.usedPreferred = true;
  }
  scan();
  // Keep the original runtime's fallback for SDKs leaving DISPLAY at zero.
  // Only a four-byte GPU counter is read; the frame stays on the device.
  if (!frame.usedPreferred && source.fbp == 0) {
    m->readBuffer(m->scratch, 4, vramSize - 4);
    if (*static_cast<uint32_t *>(m->readback.mapped) == 0)
      for (const auto &candidate : r.contextFrames) {
        if (candidate.fbp == source.fbp || !candidate.fbw)
          continue;
        source = candidate;
        p[0] = source.psm;
        p[1] = source.fbp * 32;
        p[2] = source.fbw;
        p[3] = p[4] = 0;
        scan();
        m->readBuffer(m->scratch, 4, vramSize - 4);
        if (*static_cast<uint32_t *>(m->readback.mapped))
          break;
      }
  }
  m->begin();
  m->upscale();
  m->display();
  frame.gpuPresented = true;
  frame.width = w;
  frame.height = h;
  frame.sourceFbp = source.fbp;
  ++m->presented;
  return frame;
}
bool VulkanGS::ClearFramebuffer(const GSContext &c, uint32_t rgba) {
  if (!c.frame.fbw || (c.frame.psm != 0 && c.frame.psm != 1 &&
                       c.frame.psm != 2 && c.frame.psm != 10))
    return false;
  std::lock_guard lock(m->mutex);
  Params p{};
  p[0] = c.frame.psm;
  p[1] = c.frame.fbp * 32;
  p[2] = std::max(c.frame.fbw, 1u);
  p[3] = c.scissor.x0;
  p[4] = c.scissor.y0;
  p[5] = rgba;
  if ((c.fba & 1) && c.frame.psm != 1)
    p[5] |= 0x80000000u;
  if ((c.frame.psm & 7) == 2)
    p[5] = ((p[5] >> 3) & 31) | ((p[5] >> 6) & 992) | ((p[5] >> 9) & 31744) |
           ((p[5] >> 24) >= 64 ? 32768u : 0u);
  p[6] = c.frame.fbmsk;
  m->dispatch(6, p,
              std::max<int>(c.scissor.x1, c.scissor.x0) - c.scissor.x0 + 1,
              std::max<int>(c.scissor.y1, c.scissor.y0) - c.scissor.y0 + 1);
  return true;
}
uint32_t VulkanGS::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t bytes) {
  std::lock_guard lock(m->mutex);
  uint32_t n = std::min<size_t>(bytes, m->download.size() - m->downloadPos);
  if (n)
    memcpy(dst, m->download.data() + m->downloadPos, n);
  m->downloadPos += n;
  return n;
}
uint32_t VulkanGS::ReadVram(uint32_t fmt, uint32_t base, uint32_t bw,
                            uint32_t x, uint32_t y) const {
  std::lock_guard lock(m->mutex);
  Params p{};
  p[0] = fmt;
  p[1] = base;
  p[2] = bw;
  p[3] = x;
  p[4] = y;
  m->dispatch(3, p, 1, 1);
  m->readBuffer(m->scratch, 4);
  return *static_cast<uint32_t *>(m->readback.mapped);
}
void VulkanGS::WriteVram(uint32_t fmt, uint32_t base, uint32_t bw, uint32_t x,
                         uint32_t y, uint32_t value) {
  std::lock_guard lock(m->mutex);
  Params p{};
  p[0] = fmt;
  p[1] = base;
  p[2] = bw;
  p[3] = x;
  p[4] = y;
  p[5] = value;
  m->dispatch(6, p, 1, 1);
}
void VulkanGS::SnapshotVram(std::vector<uint8_t> &out) const {
  std::lock_guard lock(m->mutex);
  m->readBuffer(m->ram, vramSize);
  out.resize(vramSize);
  memcpy(out.data(), m->readback.mapped, vramSize);
  if (m->mirror)
    memcpy(m->mirror, out.data(), vramSize);
}
GSTransferSnapshot VulkanGS::GetTransferSnapshot() const {
  std::lock_guard lock(m->mutex);
  return {m->transfer.trxreg.rrw ? m->copied % m->transfer.trxreg.rrw : 0,
          m->transfer.trxreg.rrw ? m->copied / m->transfer.trxreg.rrw : 0,
          m->total,
          m->copied,
          m->transfer.direction,
          m->download.size() - m->downloadPos};
}
void VulkanGS::capture(const std::string &path) {
  std::lock_guard lock(m->mutex);
  auto &im = m->fsr ? m->finalImage : m->nativeImage;
  m->begin();
  m->transition(im.handle, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {im.w, im.h, 1};
  if (uint64_t(im.w) * im.h * 4 > m->readback.size)
    throw std::runtime_error("Screenshot exceeds staging capacity");
  vkCmdCopyImageToBuffer(m->cmd, im.handle,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         m->readback.handle, 1, &copy);
  m->transition(im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL);
  m->finish();
  Image image{m->readback.mapped, int(im.w), int(im.h), 1,
              PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
  if (!ExportImage(image, path.c_str()))
    throw std::runtime_error("Failed to export Vulkan screenshot");
}
