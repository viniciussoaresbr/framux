// VK_LAYER_FRAMUX_overlay_<arch> — camada Vulkan implícita (§C.3.1).
//
// Ativa só com FRAMUX=1 (enable_environment do manifesto). No
// vkQueuePresentKHR: timestamp → ring, overlay por vkCmdClearAttachments
// (sem pipeline, shader ou descriptor) e cópia da imagem para screenshot.
// Qualquer falha desativa só a funcionalidade afetada; o present do jogo
// sempre segue para a próxima camada.

// Plataformas antes de qualquer cabeçalho Vulkan, para o dispatch table
// incluir vkCreateXlibSurfaceKHR/vkCreateXcbSurfaceKHR.
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#include <X11/Xlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "client/runtime.h"
#include "common/log.h"

#define FRAMUX_EXPORT extern "C" __attribute__((visibility("default")))

namespace framux::vk {

using client::Runtime;

namespace {

void *key(const void *dispatchable) { return *(void *const *)dispatchable; }

struct InstanceData {
  VkInstance instance;
  VkuInstanceDispatchTable vtable;
  PFN_vkGetInstanceProcAddr next_gipa;
  uint32_t api_version;
  std::string engine;
};

struct DeviceData {
  VkDevice device;
  VkPhysicalDevice physical;
  InstanceData *inst;
  VkuDeviceDispatchTable vtable;
  PFN_vkGetDeviceProcAddr next_gdpa;
  PFN_vkSetDeviceLoaderData set_loader_data;
  std::vector<VkQueueFamilyProperties> families;
  VkPhysicalDeviceMemoryProperties mem_props;
  std::unordered_map<VkQueue, uint32_t> queue_family;
};

struct PendingCapture {
  client::CaptureRequest req;
  uint32_t image;
  uint64_t present_ns;
};

struct SwapchainData {
  DeviceData *dev;
  VkSwapchainKHR handle;
  uint32_t logical_id;
  VkFormat format;
  VkExtent2D extent;
  VkImageUsageFlags usage;
  std::vector<VkImage> images;

  // Recursos do overlay/captura, criados no primeiro present que precisar.
  bool init_tried = false;
  bool ready = false;
  uint32_t family = UINT32_MAX;
  bool can_draw = false;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  std::vector<VkImageView> views;
  std::vector<VkFramebuffer> framebuffers;
  std::vector<VkCommandBuffer> cmds;
  std::vector<VkFence> fences;
  std::vector<VkSemaphore> semaphores;
  std::vector<bool> fence_in_flight;

  VkBuffer cap_buffer = VK_NULL_HANDLE;
  VkDeviceMemory cap_memory = VK_NULL_HANDLE;
  VkDeviceSize cap_size = 0;
  bool cap_coherent = false;
  std::deque<PendingCapture> cap_inflight;  // no máximo um, na prática
  std::deque<client::CaptureRequest> cap_waiting;
};

std::mutex g_mu;
std::unordered_map<void *, std::unique_ptr<InstanceData>> g_instances;  // chave: dispatch
std::unordered_map<void *, std::unique_ptr<DeviceData>> g_devices;      // chave: dispatch
std::unordered_map<VkSwapchainKHR, std::unique_ptr<SwapchainData>> g_swapchains;
std::unordered_map<VkSurfaceKHR, uint32_t> g_surface_window;
std::unordered_map<uint32_t, int> g_logical_refs;

InstanceData *get_instance(const void *h) {
  std::lock_guard lk(g_mu);
  auto it = g_instances.find(key(h));
  return it == g_instances.end() ? nullptr : it->second.get();
}

DeviceData *get_device(const void *h) {
  std::lock_guard lk(g_mu);
  auto it = g_devices.find(key(h));
  return it == g_devices.end() ? nullptr : it->second.get();
}

uint32_t bytes_per_pixel(VkFormat f) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM: return 8;
    default: return 0;
  }
}

int32_t find_memory_type(const DeviceData &d, uint32_t bits, VkMemoryPropertyFlags want, bool *coherent) {
  for (int pass = 0; pass < 2; ++pass) {
    const VkMemoryPropertyFlags flags =
        pass == 0 ? want | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : want;
    for (uint32_t i = 0; i < d.mem_props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (d.mem_props.memoryTypes[i].propertyFlags & flags) == flags) {
        *coherent = d.mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        return int32_t(i);
      }
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Recursos por swapchain

void destroy_resources(SwapchainData &sc) {
  DeviceData &d = *sc.dev;
  auto &vt = d.vtable;
  if (!sc.fences.empty()) {
    std::vector<VkFence> busy;
    for (size_t i = 0; i < sc.fences.size(); ++i)
      if (sc.fence_in_flight[i]) busy.push_back(sc.fences[i]);
    if (!busy.empty()) vt.WaitForFences(d.device, uint32_t(busy.size()), busy.data(), VK_TRUE, 1'000'000'000ull);
  }
  for (VkFence f : sc.fences) vt.DestroyFence(d.device, f, nullptr);
  for (VkSemaphore s : sc.semaphores) vt.DestroySemaphore(d.device, s, nullptr);
  for (VkFramebuffer fb : sc.framebuffers) vt.DestroyFramebuffer(d.device, fb, nullptr);
  for (VkImageView v : sc.views) vt.DestroyImageView(d.device, v, nullptr);
  if (sc.render_pass) vt.DestroyRenderPass(d.device, sc.render_pass, nullptr);
  if (sc.pool) vt.DestroyCommandPool(d.device, sc.pool, nullptr);
  if (sc.cap_buffer) vt.DestroyBuffer(d.device, sc.cap_buffer, nullptr);
  if (sc.cap_memory) vt.FreeMemory(d.device, sc.cap_memory, nullptr);
  sc.fences.clear();
  sc.semaphores.clear();
  sc.framebuffers.clear();
  sc.views.clear();
  sc.cmds.clear();
  sc.fence_in_flight.clear();
  sc.render_pass = VK_NULL_HANDLE;
  sc.pool = VK_NULL_HANDLE;
  sc.cap_buffer = VK_NULL_HANDLE;
  sc.cap_memory = VK_NULL_HANDLE;
  sc.ready = false;
}

bool init_resources(SwapchainData &sc, uint32_t family) {
  DeviceData &d = *sc.dev;
  auto &vt = d.vtable;
  sc.family = family;
  const VkQueueFlags qf = family < d.families.size() ? d.families[family].queueFlags : 0;
  sc.can_draw = (qf & VK_QUEUE_GRAPHICS_BIT) && (sc.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if (!(qf & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) return false;

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = family;
  if (vt.CreateCommandPool(d.device, &pci, nullptr, &sc.pool) != VK_SUCCESS) return false;

  const uint32_t n = uint32_t(sc.images.size());
  sc.cmds.resize(n);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = sc.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = n;
  if (vt.AllocateCommandBuffers(d.device, &cai, sc.cmds.data()) != VK_SUCCESS) return false;
  for (VkCommandBuffer cb : sc.cmds) {
    // Handles despachaveis criados pela camada precisam do ponteiro do loader.
    if (d.set_loader_data)
      d.set_loader_data(d.device, cb);
    else
      *(void **)cb = *(void **)d.device;
  }

  sc.fences.resize(n);
  sc.semaphores.resize(n);
  sc.fence_in_flight.assign(n, false);
  for (uint32_t i = 0; i < n; ++i) {
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vt.CreateFence(d.device, &fci, nullptr, &sc.fences[i]) != VK_SUCCESS ||
        vt.CreateSemaphore(d.device, &sci, nullptr, &sc.semaphores[i]) != VK_SUCCESS)
      return false;
  }

  if (sc.can_draw) {
    VkAttachmentDescription att{};
    att.format = sc.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    if (vt.CreateRenderPass(d.device, &rpci, nullptr, &sc.render_pass) != VK_SUCCESS) {
      sc.can_draw = false;
    } else {
      sc.views.resize(n, VK_NULL_HANDLE);
      sc.framebuffers.resize(n, VK_NULL_HANDLE);
      for (uint32_t i = 0; i < n && sc.can_draw; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = sc.images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sc.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        if (vt.CreateImageView(d.device, &vci, nullptr, &sc.views[i]) != VK_SUCCESS) {
          sc.can_draw = false;
          break;
        }
        fci.renderPass = sc.render_pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &sc.views[i];
        fci.width = sc.extent.width;
        fci.height = sc.extent.height;
        fci.layers = 1;
        if (vt.CreateFramebuffer(d.device, &fci, nullptr, &sc.framebuffers[i]) != VK_SUCCESS) sc.can_draw = false;
      }
    }
    if (!sc.can_draw) FRAMUX_LOG_ONCE("overlay Vulkan indisponível neste swapchain (falha ao criar render pass)");
  }
  sc.ready = true;
  return true;
}

bool ensure_capture_buffer(SwapchainData &sc, VkDeviceSize size) {
  if (sc.cap_buffer && sc.cap_size >= size) return true;
  DeviceData &d = *sc.dev;
  auto &vt = d.vtable;
  if (sc.cap_buffer) vt.DestroyBuffer(d.device, sc.cap_buffer, nullptr);
  if (sc.cap_memory) vt.FreeMemory(d.device, sc.cap_memory, nullptr);
  sc.cap_buffer = VK_NULL_HANDLE;
  sc.cap_memory = VK_NULL_HANDLE;

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vt.CreateBuffer(d.device, &bci, nullptr, &sc.cap_buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vt.GetBufferMemoryRequirements(d.device, sc.cap_buffer, &req);
  const int32_t type = find_memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &sc.cap_coherent);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = uint32_t(type);
  if (type < 0 || vt.AllocateMemory(d.device, &mai, nullptr, &sc.cap_memory) != VK_SUCCESS ||
      vt.BindBufferMemory(d.device, sc.cap_buffer, sc.cap_memory, 0) != VK_SUCCESS) {
    vt.DestroyBuffer(d.device, sc.cap_buffer, nullptr);
    sc.cap_buffer = VK_NULL_HANDLE;
    return false;
  }
  sc.cap_size = size;
  return true;
}

// Envia ao daemon a captura cujo command buffer já terminou.
void finish_capture(SwapchainData &sc, const PendingCapture &pc) {
  DeviceData &d = *sc.dev;
  auto &vt = d.vtable;
  Runtime &rt = Runtime::get();
  const uint32_t bpp = bytes_per_pixel(sc.format);
  const size_t size = size_t(sc.extent.width) * sc.extent.height * bpp;
  void *ptr = nullptr;
  if (vt.MapMemory(d.device, sc.cap_memory, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) {
    rt.send_capture_error(pc.req.request_id, "falha ao mapear a memória da captura");
    return;
  }
  if (!sc.cap_coherent) {
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, sc.cap_memory, 0, VK_WHOLE_SIZE};
    vt.InvalidateMappedMemoryRanges(d.device, 1, &r);
  }
  const int fd = memfd_create("framux-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  bool ok = fd >= 0 && ftruncate(fd, off_t(size)) == 0;
  size_t done = 0;
  while (ok && done < size) {
    const ssize_t w = pwrite(fd, static_cast<const char *>(ptr) + done, size - done, off_t(done));
    if (w <= 0)
      ok = false;
    else
      done += size_t(w);
  }
  vt.UnmapMemory(d.device, sc.cap_memory);
  if (!ok) {
    if (fd >= 0) close(fd);
    rt.send_capture_error(pc.req.request_id, "falha ao copiar o quadro");
    return;
  }
  fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
  ipc::FrameImage img{};
  img.request_id = pc.req.request_id;
  img.present_ns = pc.present_ns;
  img.width = sc.extent.width;
  img.height = sc.extent.height;
  img.vk_format = uint32_t(sc.format);
  img.stride = sc.extent.width * bpp;
  img.offset = 0;
  img.size = size;
  rt.send_frame_image(img, fd);
}

void poll_captures(SwapchainData &sc, bool wait_image, uint32_t image) {
  auto &vt = sc.dev->vtable;
  while (!sc.cap_inflight.empty()) {
    PendingCapture pc = sc.cap_inflight.front();
    const VkFence f = sc.fences[pc.image];
    VkResult r = vt.GetFenceStatus(sc.dev->device, f);
    if (r == VK_NOT_READY && wait_image && pc.image == image)
      r = vt.WaitForFences(sc.dev->device, 1, &f, VK_TRUE, 1'000'000'000ull);
    if (r != VK_SUCCESS) return;
    sc.cap_inflight.pop_front();
    finish_capture(sc, pc);
  }
}

void copy_to_buffer(SwapchainData &sc, VkCommandBuffer cb, uint32_t image) {
  auto &vt = sc.dev->vtable;
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = sc.images[image];
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vt.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                        nullptr, 1, &b);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {sc.extent.width, sc.extent.height, 1};
  vt.CmdCopyImageToBuffer(cb, sc.images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc.cap_buffer, 1, &region);
  std::swap(b.oldLayout, b.newLayout);
  b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  vt.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                        nullptr, 1, &b);
  VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.buffer = sc.cap_buffer;
  bb.size = VK_WHOLE_SIZE;
  vt.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0,
                        nullptr);
}

void clear_rects(SwapchainData &sc, VkCommandBuffer cb, const std::vector<ORect> &rects, OColor color,
                 std::vector<VkClearRect> &scratch) {
  scratch.clear();
  const int32_t W = int32_t(sc.extent.width), H = int32_t(sc.extent.height);
  for (const ORect &r : rects) {
    const int32_t x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int32_t x1 = std::min(W, r.x + r.w), y1 = std::min(H, r.y + r.h);
    if (x1 <= x0 || y1 <= y0) continue;
    scratch.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
  }
  if (scratch.empty()) return;
  VkClearAttachment att{};
  att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  att.colorAttachment = 0;
  // Formatos UNORM/SRGB/SFLOAT aceitam float32; o hardware converte.
  att.clearValue.color = {{color.r, color.g, color.b, color.a}};
  sc.dev->vtable.CmdClearAttachments(cb, 1, &att, uint32_t(scratch.size()), scratch.data());
}

void draw_overlay(SwapchainData &sc, VkCommandBuffer cb, uint32_t image, const OverlayDraw &draw) {
  auto &vt = sc.dev->vtable;
  VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rbi.renderPass = sc.render_pass;
  rbi.framebuffer = sc.framebuffers[image];
  rbi.renderArea = {{0, 0}, sc.extent};
  vt.CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  static thread_local std::vector<VkClearRect> scratch;
  if (draw.has_box) clear_rects(sc, cb, {draw.box}, draw.box_color, scratch);
  clear_rects(sc, cb, draw.outline, draw.outline_color, scratch);
  clear_rects(sc, cb, draw.text, draw.text_color, scratch);
  vt.CmdEndRenderPass(cb);
}

// ---------------------------------------------------------------------------
// Instância

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                              VkInstance *out) {
  auto *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
  while (chain &&
         !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
    chain = (VkLayerInstanceCreateInfo *)chain->pNext;
  if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
  auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkResult r = create(ci, alloc, out);
  if (r != VK_SUCCESS) return r;

  auto data = std::make_unique<InstanceData>();
  data->instance = *out;
  data->next_gipa = gipa;
  vkuInitInstanceDispatchTable(*out, &data->vtable, gipa);
  data->api_version =
      ci->pApplicationInfo && ci->pApplicationInfo->apiVersion ? ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
  if (ci->pApplicationInfo && ci->pApplicationInfo->pEngineName) data->engine = ci->pApplicationInfo->pEngineName;
  std::lock_guard lk(g_mu);
  g_instances[key(*out)] = std::move(data);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
  InstanceData *d = get_instance(instance);
  if (!d) return;
  auto destroy = d->vtable.DestroyInstance;
  {
    std::lock_guard lk(g_mu);
    g_instances.erase(key(instance));
  }
  destroy(instance, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *ci,
                                                    const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  InstanceData *d = get_instance(instance);
  VkResult r = d->vtable.CreateXlibSurfaceKHR(instance, ci, alloc, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lk(g_mu);
    g_surface_window[*out] = uint32_t(ci->window);
  }
  return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR *ci,
                                                   const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  InstanceData *d = get_instance(instance);
  VkResult r = d->vtable.CreateXcbSurfaceKHR(instance, ci, alloc, out);
  if (r == VK_SUCCESS) {
    std::lock_guard lk(g_mu);
    g_surface_window[*out] = ci->window;
  }
  return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                             const VkAllocationCallbacks *alloc) {
  InstanceData *d = get_instance(instance);
  {
    std::lock_guard lk(g_mu);
    g_surface_window.erase(surface);
  }
  d->vtable.DestroySurfaceKHR(instance, surface, alloc);
}

// ---------------------------------------------------------------------------
// Dispositivo

std::string driver_string(InstanceData *inst, VkPhysicalDevice phys, const VkPhysicalDeviceProperties &props) {
  if (inst->api_version >= VK_API_VERSION_1_1 && props.apiVersion >= VK_API_VERSION_1_2 &&
      inst->vtable.GetPhysicalDeviceProperties2) {
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    inst->vtable.GetPhysicalDeviceProperties2(phys, &p2);
    std::string s = drv.driverName;
    if (drv.driverInfo[0]) s += std::string(" ") + drv.driverInfo;
    return s;
  }
  // Sem VkPhysicalDeviceDriverProperties: decodifica driverVersion. A NVIDIA
  // usa 10.8.8.6 bits; Mesa e a maioria, o formato de VK_MAKE_VERSION.
  const uint32_t v = props.driverVersion;
  char buf[64];
  if (props.vendorID == 0x10de)
    std::snprintf(buf, sizeof buf, "nvidia %u.%u.%u", v >> 22, (v >> 14) & 0xff, (v >> 6) & 0xff);
  else
    std::snprintf(buf, sizeof buf, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
                  VK_API_VERSION_PATCH(v));
  return buf;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                            const VkAllocationCallbacks *alloc, VkDevice *out) {
  InstanceData *inst = get_instance(phys);
  auto *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
  while (chain &&
         !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
    chain = (VkLayerDeviceCreateInfo *)chain->pNext;
  if (!chain || !inst) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
  auto create = (PFN_vkCreateDevice)gipa(inst->instance, "vkCreateDevice");
  VkResult r = create(phys, ci, alloc, out);
  if (r != VK_SUCCESS) return r;

  auto data = std::make_unique<DeviceData>();
  data->device = *out;
  data->physical = phys;
  data->inst = inst;
  data->next_gdpa = gdpa;
  data->set_loader_data = nullptr;
  vkuInitDeviceDispatchTable(*out, &data->vtable, gdpa);
  for (auto *cb = (const VkLayerDeviceCreateInfo *)ci->pNext; cb; cb = (const VkLayerDeviceCreateInfo *)cb->pNext) {
    if (cb->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && cb->function == VK_LOADER_DATA_CALLBACK) {
      data->set_loader_data = cb->u.pfnSetDeviceLoaderData;
      break;
    }
  }
  uint32_t nfam = 0;
  inst->vtable.GetPhysicalDeviceQueueFamilyProperties(phys, &nfam, nullptr);
  data->families.resize(nfam);
  inst->vtable.GetPhysicalDeviceQueueFamilyProperties(phys, &nfam, data->families.data());
  inst->vtable.GetPhysicalDeviceMemoryProperties(phys, &data->mem_props);

  VkPhysicalDeviceProperties props;
  inst->vtable.GetPhysicalDeviceProperties(phys, &props);
  Runtime::get().set_device_info(inst->engine, driver_string(inst, phys, props), props.deviceName);

  std::lock_guard lk(g_mu);
  g_devices[key(*out)] = std::move(data);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
  DeviceData *d = get_device(device);
  if (!d) return;
  auto destroy = d->vtable.DestroyDevice;
  {
    std::lock_guard lk(g_mu);
    for (auto it = g_swapchains.begin(); it != g_swapchains.end();) {
      if (it->second->dev == d) {
        destroy_resources(*it->second);
        it = g_swapchains.erase(it);
      } else {
        ++it;
      }
    }
    g_devices.erase(key(device));
  }
  destroy(device, alloc);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out) {
  DeviceData *d = get_device(device);
  d->vtable.GetDeviceQueue(device, family, index, out);
  if (*out) {
    std::lock_guard lk(g_mu);
    d->queue_family[*out] = family;
  }
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *out) {
  DeviceData *d = get_device(device);
  d->vtable.GetDeviceQueue2(device, info, out);
  if (*out) {
    std::lock_guard lk(g_mu);
    d->queue_family[*out] = info->queueFamilyIndex;
  }
}

// ---------------------------------------------------------------------------
// Swapchain

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                  const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
  DeviceData *d = get_device(device);
  VkSwapchainCreateInfoKHR info = *ci;
  VkSurfaceCapabilitiesKHR caps{};
  if (d->inst->vtable.GetPhysicalDeviceSurfaceCapabilitiesKHR &&
      d->inst->vtable.GetPhysicalDeviceSurfaceCapabilitiesKHR(d->physical, ci->surface, &caps) == VK_SUCCESS) {
    // Overlay precisa de COLOR_ATTACHMENT; screenshot, de TRANSFER_SRC.
    info.imageUsage |=
        caps.supportedUsageFlags & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  }
  VkResult r = d->vtable.CreateSwapchainKHR(device, &info, alloc, out);
  if (r != VK_SUCCESS && info.imageUsage != ci->imageUsage) {
    info.imageUsage = ci->imageUsage;
    r = d->vtable.CreateSwapchainKHR(device, &info, alloc, out);
  }
  if (r != VK_SUCCESS) return r;

  auto sc = std::make_unique<SwapchainData>();
  sc->dev = d;
  sc->handle = *out;
  sc->format = info.imageFormat;
  sc->extent = info.imageExtent;
  sc->usage = info.imageUsage;
  uint32_t n = 0;
  d->vtable.GetSwapchainImagesKHR(device, *out, &n, nullptr);
  sc->images.resize(n);
  d->vtable.GetSwapchainImagesKHR(device, *out, &n, sc->images.data());

  uint32_t window = 0;
  {
    std::lock_guard lk(g_mu);
    // Recriação (resize) herda o swapchain lógico: o FPS e o benchmark continuam.
    auto old = ci->oldSwapchain ? g_swapchains.find(ci->oldSwapchain) : g_swapchains.end();
    sc->logical_id = old != g_swapchains.end() ? old->second->logical_id : Runtime::get().new_swapchain_id();
    ++g_logical_refs[sc->logical_id];
    auto w = g_surface_window.find(ci->surface);
    if (w != g_surface_window.end()) window = w->second;
    g_swapchains[*out] = std::move(sc);
  }
  if (window) Runtime::get().set_x11_window(window);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks *alloc) {
  DeviceData *d = get_device(device);
  uint32_t forget = 0;
  {
    std::lock_guard lk(g_mu);
    auto it = g_swapchains.find(swapchain);
    if (it != g_swapchains.end()) {
      destroy_resources(*it->second);
      const uint32_t id = it->second->logical_id;
      if (--g_logical_refs[id] <= 0) {
        g_logical_refs.erase(id);
        forget = id;
      }
      g_swapchains.erase(it);
    }
  }
  if (forget) Runtime::get().forget_swapchain(forget);
  d->vtable.DestroySwapchainKHR(device, swapchain, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi) {
  DeviceData *d = get_device(queue);
  Runtime &rt = Runtime::get();
  if (!rt.init(ipc::Api::Vulkan)) return d->vtable.QueuePresentKHR(queue, pi);

  uint32_t family = UINT32_MAX;
  {
    std::lock_guard lk(g_mu);
    auto it = d->queue_family.find(queue);
    if (it != d->queue_family.end()) family = it->second;
  }

  std::vector<VkCommandBuffer> cmds;
  VkFence submit_fence = VK_NULL_HANDLE;
  VkSemaphore signal = VK_NULL_HANDLE;
  SwapchainData *fence_owner = nullptr;
  uint32_t fence_image = 0;

  for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
    SwapchainData *sc;
    {
      std::lock_guard lk(g_mu);
      auto it = g_swapchains.find(pi->pSwapchains[i]);
      sc = it == g_swapchains.end() ? nullptr : it->second.get();
    }
    if (!sc) continue;
    client::PresentResult res;
    rt.on_present(sc->logical_id, sc->extent.width, sc->extent.height, res);
    const uint64_t present_ns = now_ns();
    if (res.capture) sc->cap_waiting.push_back(*res.capture);

    if (sc->ready && !sc->cap_inflight.empty()) poll_captures(*sc, false, 0);
    if (!res.draw && sc->cap_waiting.empty()) continue;

    if (!sc->init_tried || (sc->ready && sc->family != family)) {
      if (sc->ready) destroy_resources(*sc);
      sc->init_tried = true;
      if (family == UINT32_MAX || !init_resources(*sc, family)) {
        destroy_resources(*sc);
        FRAMUX_LOG_ONCE("overlay/captura indisponíveis: fila de present sem suporte");
      }
    }
    if (!sc->ready) {
      while (!sc->cap_waiting.empty()) {
        rt.send_capture_error(sc->cap_waiting.front().request_id, "captura indisponível neste swapchain");
        sc->cap_waiting.pop_front();
      }
      continue;
    }
    // Só um conjunto de semáforo/fence por present: swapchains extras
    // (raro) ficam sem overlay neste quadro.
    if (submit_fence) continue;

    const uint32_t image = pi->pImageIndices[i];
    if (image >= sc->images.size()) continue;
    auto &vt = d->vtable;
    if (sc->fence_in_flight[image]) {
      poll_captures(*sc, true, image);
      if (vt.WaitForFences(d->device, 1, &sc->fences[image], VK_TRUE, 100'000'000ull) != VK_SUCCESS) continue;
      sc->fence_in_flight[image] = false;
    }
    vt.ResetFences(d->device, 1, &sc->fences[image]);

    client::CaptureRequest cap{};
    bool has_cap = false;
    const uint32_t bpp = bytes_per_pixel(sc->format);
    if (!sc->cap_waiting.empty() && sc->cap_inflight.empty()) {
      cap = sc->cap_waiting.front();
      sc->cap_waiting.pop_front();
      has_cap = (sc->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && bpp != 0 &&
                ensure_capture_buffer(*sc, VkDeviceSize(sc->extent.width) * sc->extent.height * bpp);
      if (!has_cap)
        rt.send_capture_error(cap.request_id, bpp == 0 ? "formato de swapchain não suportado para captura"
                                                       : "swapchain não permite cópia da imagem");
    }
    const bool draw = res.draw && sc->can_draw;
    if (!draw && !has_cap) continue;

    VkCommandBuffer cb = sc->cmds[image];
    vt.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vt.BeginCommandBuffer(cb, &bi);
    if (has_cap && !cap.include_overlay) copy_to_buffer(*sc, cb, image);
    if (draw) draw_overlay(*sc, cb, image, *res.draw);
    if (has_cap && cap.include_overlay) copy_to_buffer(*sc, cb, image);
    vt.EndCommandBuffer(cb);

    if (has_cap) sc->cap_inflight.push_back({cap, image, present_ns});
    cmds.push_back(cb);
    submit_fence = sc->fences[image];
    signal = sc->semaphores[image];
    fence_owner = sc;
    fence_image = image;
  }

  if (cmds.empty()) return d->vtable.QueuePresentKHR(queue, pi);

  std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = pi->waitSemaphoreCount;
  si.pWaitSemaphores = pi->pWaitSemaphores;
  si.pWaitDstStageMask = stages.data();
  si.commandBufferCount = uint32_t(cmds.size());
  si.pCommandBuffers = cmds.data();
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &signal;
  if (d->vtable.QueueSubmit(queue, 1, &si, submit_fence) != VK_SUCCESS) {
    FRAMUX_LOG_ONCE("vkQueueSubmit do overlay falhou; quadro apresentado sem overlay");
    fence_owner->cap_inflight.clear();
    return d->vtable.QueuePresentKHR(queue, pi);
  }
  fence_owner->fence_in_flight[fence_image] = true;

  VkPresentInfoKHR p = *pi;
  p.waitSemaphoreCount = 1;
  p.pWaitSemaphores = &signal;
  return d->vtable.QueuePresentKHR(queue, &p);
}

// ---------------------------------------------------------------------------
// Resolução de funções

PFN_vkVoidFunction device_hook(const char *name) {
#define HOOK(fn) \
  if (!std::strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)fn
  HOOK(DestroyDevice);
  HOOK(GetDeviceQueue);
  HOOK(GetDeviceQueue2);
  HOOK(CreateSwapchainKHR);
  HOOK(DestroySwapchainKHR);
  HOOK(QueuePresentKHR);
#undef HOOK
  return nullptr;
}

PFN_vkVoidFunction instance_hook(const char *name) {
#define HOOK(fn) \
  if (!std::strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)fn
  HOOK(CreateInstance);
  HOOK(DestroyInstance);
  HOOK(CreateDevice);
  HOOK(CreateXlibSurfaceKHR);
  HOOK(CreateXcbSurfaceKHR);
  HOOK(DestroySurfaceKHR);
#undef HOOK
  return nullptr;
}

}  // namespace

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name) {
  if (!std::strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)GetInstanceProcAddr;
  if (!std::strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
  if (!std::strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)CreateInstance;
  if (!instance) return nullptr;
  InstanceData *d = get_instance(instance);
  if (!d) return nullptr;
  PFN_vkVoidFunction next = d->next_gipa(instance, name);
  // Só intercepta o que a próxima camada expõe (extensões habilitadas).
  if (!next) return nullptr;
  if (Runtime::skipped_by_auto_mode()) {
    // Transparente: só o necessário para manter as tabelas de despacho.
    if (!std::strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)DestroyInstance;
    if (!std::strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)CreateDevice;
    if (!std::strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
    return next;
  }
  if (auto h = instance_hook(name)) return h;
  if (auto h = device_hook(name)) return h;
  return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name) {
  if (!std::strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
  DeviceData *d = get_device(device);
  if (!d) return nullptr;
  PFN_vkVoidFunction next = d->next_gdpa(device, name);
  if (!next) return nullptr;
  if (Runtime::skipped_by_auto_mode())
    return std::strcmp(name, "vkDestroyDevice") ? next : (PFN_vkVoidFunction)DestroyDevice;
  if (auto h = device_hook(name)) return h;
  return next;
}

}  // namespace framux::vk

FRAMUX_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framux_GetInstanceProcAddr(VkInstance instance,
                                                                                  const char *name) {
  return framux::vk::GetInstanceProcAddr(instance, name);
}

FRAMUX_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framux_GetDeviceProcAddr(VkDevice device, const char *name) {
  return framux::vk::GetDeviceProcAddr(device, name);
}

FRAMUX_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
  if (v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
  if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
  v->pfnGetInstanceProcAddr = framux::vk::GetInstanceProcAddr;
  v->pfnGetDeviceProcAddr = framux::vk::GetDeviceProcAddr;
  v->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}
