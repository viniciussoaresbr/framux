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
#include "common/ipc_proto.h"
#include "common/log.h"

#define FRAMUX_EXPORT extern "C" __attribute__((visibility("default")))

namespace framux::vk {

using client::Runtime;

namespace {

// SPIR-V de rgb_to_nv12.comp (gerado; ver o comentário no .comp).
#include "layer/rgb_to_nv12.spv.h"

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

// Buffer host-visible para ler o swapchain (screenshot ou gravação).
struct HostBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  bool coherent = false;
};

// Gravação: alguns quadros em voo para a cópia nunca esperar a GPU.
constexpr uint32_t kVideoStaging = 3;

struct VideoInflight {
  uint32_t staging;
  uint32_t image;
  uint64_t present_ns;
  uint32_t width, height, stride, vk_format, flags;  // como vai ao daemon
};

// Gravação em NV12: o RGB do swapchain vai para um buffer na GPU e um compute
// shader escreve o NV12 (1,5 byte por pixel, opcionalmente na metade do
// tamanho) no buffer host-visible que a camada lê.
struct YuvPipe {
  bool tried = false, ok = false;
  VkShaderModule module = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet sets[kVideoStaging] = {};
  VkBuffer bound[kVideoStaging] = {};  // saída ligada a cada set
  HostBuffer rgb;                      // memória da GPU, não do host
};

struct YuvParams {  // push constants de rgb_to_nv12.comp
  uint32_t src_w, src_h, out_w, out_h, stride, scale, bgr;
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

  HostBuffer cap;
  std::deque<PendingCapture> cap_inflight;  // no máximo um, na prática
  std::deque<client::CaptureRequest> cap_waiting;

  HostBuffer video[kVideoStaging];
  bool video_busy[kVideoStaging] = {};
  std::deque<VideoInflight> video_inflight;
  YuvPipe yuv;
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
  if (!(want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {  // memória da GPU
    for (uint32_t i = 0; i < d.mem_props.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (d.mem_props.memoryTypes[i].propertyFlags & want) == want) {
        *coherent = false;
        return int32_t(i);
      }
    return -1;
  }
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

void free_host_buffer(DeviceData &d, HostBuffer &b) {
  if (b.buffer) d.vtable.DestroyBuffer(d.device, b.buffer, nullptr);
  if (b.memory) d.vtable.FreeMemory(d.device, b.memory, nullptr);
  b = HostBuffer{};
}

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
  free_host_buffer(d, sc.cap);
  for (HostBuffer &b : sc.video) free_host_buffer(d, b);
  YuvPipe &y = sc.yuv;
  if (y.pipeline) vt.DestroyPipeline(d.device, y.pipeline, nullptr);
  if (y.layout) vt.DestroyPipelineLayout(d.device, y.layout, nullptr);
  if (y.pool) vt.DestroyDescriptorPool(d.device, y.pool, nullptr);
  if (y.set_layout) vt.DestroyDescriptorSetLayout(d.device, y.set_layout, nullptr);
  if (y.module) vt.DestroyShaderModule(d.device, y.module, nullptr);
  free_host_buffer(d, y.rgb);
  y = YuvPipe{};
  for (bool &b : sc.video_busy) b = false;
  sc.video_inflight.clear();
  sc.fences.clear();
  sc.semaphores.clear();
  sc.framebuffers.clear();
  sc.views.clear();
  sc.cmds.clear();
  sc.fence_in_flight.clear();
  sc.render_pass = VK_NULL_HANDLE;
  sc.pool = VK_NULL_HANDLE;
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

// Buffer de `size` bytes, host-visible (leitura pela CPU) ou na memória da
// GPU (`host` = false). Os de gravação também servem de storage buffer.
bool ensure_buffer(DeviceData &d, HostBuffer &b, VkDeviceSize size, bool host = true) {
  if (b.buffer && b.size >= size) return true;
  auto &vt = d.vtable;
  free_host_buffer(d, b);

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vt.CreateBuffer(d.device, &bci, nullptr, &b.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vt.GetBufferMemoryRequirements(d.device, b.buffer, &req);
  const int32_t type =
      find_memory_type(d, req.memoryTypeBits,
                       host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &b.coherent);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = uint32_t(type);
  if (type < 0 || vt.AllocateMemory(d.device, &mai, nullptr, &b.memory) != VK_SUCCESS ||
      vt.BindBufferMemory(d.device, b.buffer, b.memory, 0) != VK_SUCCESS) {
    free_host_buffer(d, b);
    return false;
  }
  b.size = size;
  return true;
}

// Mapeia `b` e chama fn(ptr); devolve false se o mapeamento falhar.
template <class Fn>
bool read_host_buffer(DeviceData &d, const HostBuffer &b, Fn &&fn) {
  auto &vt = d.vtable;
  void *ptr = nullptr;
  if (vt.MapMemory(d.device, b.memory, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) return false;
  if (!b.coherent) {
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, b.memory, 0, VK_WHOLE_SIZE};
    vt.InvalidateMappedMemoryRanges(d.device, 1, &r);
  }
  fn(static_cast<const uint8_t *>(ptr));
  vt.UnmapMemory(d.device, b.memory);
  return true;
}

// Envia ao daemon a captura cujo command buffer já terminou.
void finish_capture(SwapchainData &sc, const PendingCapture &pc) {
  Runtime &rt = Runtime::get();
  const uint32_t bpp = bytes_per_pixel(sc.format);
  const size_t size = size_t(sc.extent.width) * sc.extent.height * bpp;
  const int fd = memfd_create("framux-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  bool ok = fd >= 0 && ftruncate(fd, off_t(size)) == 0;
  const bool mapped = read_host_buffer(*sc.dev, sc.cap, [&](const uint8_t *ptr) {
    size_t done = 0;
    while (ok && done < size) {
      const ssize_t w = pwrite(fd, ptr + done, size - done, off_t(done));
      if (w <= 0)
        ok = false;
      else
        done += size_t(w);
    }
  });
  if (!mapped) {
    if (fd >= 0) close(fd);
    rt.send_capture_error(pc.req.request_id, "falha ao mapear a memória da captura");
    return;
  }
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

// Gravação: entrega ao runtime os quadros cuja cópia já terminou.
void poll_video(SwapchainData &sc, bool wait_image, uint32_t image) {
  auto &vt = sc.dev->vtable;
  Runtime &rt = Runtime::get();
  while (!sc.video_inflight.empty()) {
    const VideoInflight v = sc.video_inflight.front();
    const VkFence f = sc.fences[v.image];
    VkResult r = vt.GetFenceStatus(sc.dev->device, f);
    if (r == VK_NOT_READY && wait_image && v.image == image)
      r = vt.WaitForFences(sc.dev->device, 1, &f, VK_TRUE, 1'000'000'000ull);
    if (r != VK_SUCCESS) return;
    sc.video_inflight.pop_front();
    sc.video_busy[v.staging] = false;
    const uint64_t t0 = now_ns();
    read_host_buffer(*sc.dev, sc.video[v.staging], [&](const uint8_t *ptr) {
      rt.video_write(ptr, v.width, v.height, v.stride, v.vk_format, v.flags, v.present_ns);
    });
    rt.add_video_cost(now_ns() - t0);
  }
}

// Copia a imagem para `dst`; `for_host`: deixa o buffer pronto para a CPU
// ler (senão, o chamador põe a barreira para o próximo uso na GPU).
void copy_to_buffer(SwapchainData &sc, VkCommandBuffer cb, uint32_t image, VkBuffer dst, bool for_host = true) {
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
  vt.CmdCopyImageToBuffer(cb, sc.images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
  std::swap(b.oldLayout, b.newLayout);
  b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  vt.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                        nullptr, 1, &b);
  if (!for_host) return;
  VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.buffer = dst;
  bb.size = VK_WHOLE_SIZE;
  vt.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0,
                        nullptr);
}

// Pipeline de compute da conversão para NV12; false (uma vez) se a fila ou
// o driver não permitirem: a gravação segue em RGB.
bool init_yuv(SwapchainData &sc) {
  YuvPipe &y = sc.yuv;
  if (y.tried) return y.ok;
  y.tried = true;
  DeviceData &d = *sc.dev;
  auto &vt = d.vtable;
  const VkQueueFlags qf = sc.family < d.families.size() ? d.families[sc.family].queueFlags : 0;
  if (!(qf & VK_QUEUE_COMPUTE_BIT)) {
    FRAMUX_LOG_ONCE("gravação: fila sem compute; conversão de cor fica no daemon");
    return false;
  }
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = sizeof kRgbToNv12Spv;
  smci.pCode = kRgbToNv12Spv;
  VkDescriptorSetLayoutBinding binds[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    binds[i].binding = i;
    binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[i].descriptorCount = 1;
    binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = 2;
  dslci.pBindings = binds;
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(YuvParams)};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &y.set_layout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kVideoStaging};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = kVideoStaging;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &ps;
  bool ok = vt.CreateShaderModule(d.device, &smci, nullptr, &y.module) == VK_SUCCESS &&
            vt.CreateDescriptorSetLayout(d.device, &dslci, nullptr, &y.set_layout) == VK_SUCCESS &&
            vt.CreatePipelineLayout(d.device, &plci, nullptr, &y.layout) == VK_SUCCESS &&
            vt.CreateDescriptorPool(d.device, &dpci, nullptr, &y.pool) == VK_SUCCESS;
  if (ok) {
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = y.module;
    cpci.stage.pName = "main";
    cpci.layout = y.layout;
    VkDescriptorSetLayout layouts[kVideoStaging];
    std::fill(std::begin(layouts), std::end(layouts), y.set_layout);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = y.pool;
    dsai.descriptorSetCount = kVideoStaging;
    dsai.pSetLayouts = layouts;
    ok = vt.CreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &y.pipeline) == VK_SUCCESS &&
         vt.AllocateDescriptorSets(d.device, &dsai, y.sets) == VK_SUCCESS &&
         ensure_buffer(d, y.rgb, VkDeviceSize(sc.extent.width) * sc.extent.height * 4, false);
  }
  if (!ok) {
    FRAMUX_LOG_ONCE("gravação: conversão de cor na GPU indisponível; fica no daemon");
    return false;
  }
  y.ok = true;
  return true;
}

// Grava em `cb`: imagem → buffer RGB na GPU → compute → NV12 em video[k].
void record_nv12(SwapchainData &sc, VkCommandBuffer cb, uint32_t image, uint32_t k, const YuvParams &prm) {
  DeviceData &d = *sc.dev;
  auto &vt = d.vtable;
  YuvPipe &y = sc.yuv;
  if (y.bound[k] != sc.video[k].buffer) {  // o set k não está em uso: video_busy[k] era false
    VkDescriptorBufferInfo bi[2] = {{y.rgb.buffer, 0, VK_WHOLE_SIZE}, {sc.video[k].buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet = y.sets[k];
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[i].pBufferInfo = &bi[i];
    }
    vt.UpdateDescriptorSets(d.device, 2, w, 0, nullptr);
    y.bound[k] = sc.video[k].buffer;
  }
  VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.size = VK_WHOLE_SIZE;
  // O quadro anterior pode ainda estar lendo o buffer RGB no compute.
  bb.buffer = y.rgb.buffer;
  bb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vt.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bb,
                        0, nullptr);
  copy_to_buffer(sc, cb, image, y.rgb.buffer, false);
  bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vt.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &bb,
                        0, nullptr);
  vt.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, y.pipeline);
  vt.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, y.layout, 0, 1, &y.sets[k], 0, nullptr);
  vt.CmdPushConstants(cb, y.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof prm, &prm);
  vt.CmdDispatch(cb, (prm.stride / 4 + 7) / 8, (prm.out_h / 2 + 7) / 8, 1);
  bb.buffer = sc.video[k].buffer;
  bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vt.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0,
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
  bool video_pushed = false;

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
    if (sc->ready && !sc->video_inflight.empty()) poll_video(*sc, false, 0);
    if (!res.draw && sc->cap_waiting.empty() && !res.video) continue;

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
      poll_video(*sc, true, image);
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
                ensure_buffer(*d, sc->cap, VkDeviceSize(sc->extent.width) * sc->extent.height * bpp);
      if (!has_cap)
        rt.send_capture_error(cap.request_id, bpp == 0 ? "formato de swapchain não suportado para captura"
                                                       : "swapchain não permite cópia da imagem");
    }
    // Gravação: um buffer livre do anel; sem nenhum (GPU atrasada), o quadro
    // fica de fora e o daemon repete o anterior.
    int video_staging = -1;
    VideoInflight vin{};
    YuvParams yuv{};
    bool use_yuv = false;
    if (res.video) {
      if (!(sc->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || bpp != 4) {
        FRAMUX_LOG_ONCE("gravação indisponível neste swapchain (formato %d ou sem cópia da imagem)", int(sc->format));
      } else {
        const uint32_t w = sc->extent.width, h = sc->extent.height;
        const uint32_t mode = rt.video_mode();
        const bool bgr = sc->format == VK_FORMAT_B8G8R8A8_UNORM || sc->format == VK_FORMAT_B8G8R8A8_SRGB;
        const bool rgba8 = bgr || sc->format == VK_FORMAT_R8G8B8A8_UNORM || sc->format == VK_FORMAT_R8G8B8A8_SRGB ||
                           sc->format == VK_FORMAT_A8B8G8R8_UNORM_PACK32 ||
                           sc->format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
        const uint32_t scale = mode & ipc::kVideoHalf ? 2 : 1;
        const uint32_t ow = (w / scale) & ~1u, oh = (h / scale) & ~1u;
        use_yuv = (mode & ipc::kVideoYuv) && rgba8 && ow >= 16 && oh >= 16 && init_yuv(*sc);
        if (use_yuv) {
          const uint32_t stride = (ow + 3) & ~3u;
          yuv = {w, h, ow, oh, stride, scale, bgr ? 1u : 0u};
          vin = {0, image, present_ns, ow, oh, stride, ipc::kVkFormatNv12, scale == 2 ? uint32_t(ipc::kImgHalved) : 0};
        } else {
          vin = {0, image, present_ns, w, h, w * 4, uint32_t(sc->format), 0};
        }
        const VkDeviceSize size = use_yuv ? VkDeviceSize(vin.stride) * vin.height * 3 / 2 : VkDeviceSize(w) * h * 4;
        for (uint32_t k = 0; k < kVideoStaging; ++k)
          if (!sc->video_busy[k]) {
            if (ensure_buffer(*d, sc->video[k], size)) video_staging = int(k);
            break;
          }
      }
    }
    const bool draw = res.draw && sc->can_draw;
    if (!draw && !has_cap && video_staging < 0) continue;

    VkCommandBuffer cb = sc->cmds[image];
    vt.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vt.BeginCommandBuffer(cb, &bi);
    // O vídeo sai sem o contador, como o screenshot padrão.
    if (video_staging >= 0) {
      if (use_yuv)
        record_nv12(*sc, cb, image, uint32_t(video_staging), yuv);
      else
        copy_to_buffer(*sc, cb, image, sc->video[video_staging].buffer);
    }
    if (has_cap && !cap.include_overlay) copy_to_buffer(*sc, cb, image, sc->cap.buffer);
    if (draw) draw_overlay(*sc, cb, image, *res.draw);
    if (has_cap && cap.include_overlay) copy_to_buffer(*sc, cb, image, sc->cap.buffer);
    vt.EndCommandBuffer(cb);

    if (has_cap) sc->cap_inflight.push_back({cap, image, present_ns});
    if (video_staging >= 0) {
      sc->video_busy[video_staging] = true;
      vin.staging = uint32_t(video_staging);
      sc->video_inflight.push_back(vin);
      video_pushed = true;
    }
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
    if (video_pushed) {
      fence_owner->video_busy[fence_owner->video_inflight.back().staging] = false;
      fence_owner->video_inflight.pop_back();
    }
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
