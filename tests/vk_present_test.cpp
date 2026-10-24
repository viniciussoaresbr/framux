// App Vulkan mínimo (xcb + swapchain + clear) usado nos testes da camada,
// inclusive no build i686, onde não há vkcube 32 bits.
// uso: vk_present_test [segundos]   (padrão: roda até ser morto)
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define VK(x)                                                 \
  do {                                                        \
    VkResult r_ = (x);                                        \
    if (r_ != VK_SUCCESS) {                                   \
      std::fprintf(stderr, "%s falhou: %d\n", #x, int(r_));   \
      std::exit(r_ == VK_ERROR_INCOMPATIBLE_DRIVER ? 77 : 1); \
    }                                                         \
  } while (0)

int main(int argc, char **argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 1e9;
  xcb_connection_t *c = xcb_connect(nullptr, nullptr);
  if (xcb_connection_has_error(c)) return 77;
  xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  const xcb_window_t win = xcb_generate_id(c);
  xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0, 640, 480, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual, 0, nullptr);
  xcb_map_window(c, win);
  xcb_flush(c);

  const char *inst_ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "vk_present_test";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = 2;
  ici.ppEnabledExtensionNames = inst_ext;
  VkInstance inst;
  VK(vkCreateInstance(&ici, nullptr, &inst));

  VkXcbSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
  sci.connection = c;
  sci.window = win;
  VkSurfaceKHR surface;
  VK(vkCreateXcbSurfaceKHR(inst, &sci, nullptr, &surface));

  uint32_t n = 1;
  VkPhysicalDevice phys;
  vkEnumeratePhysicalDevices(inst, &n, &phys);
  if (n == 0) return 77;
  uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> qf(nq);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
  uint32_t family = UINT32_MAX;
  for (uint32_t i = 0; i < nq && family == UINT32_MAX; ++i) {
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
    if (present && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family = i;
  }
  if (family == UINT32_MAX) return 77;

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char *dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dev_ext;
  VkDevice dev;
  VK(vkCreateDevice(phys, &dci, nullptr, &dev));
  VkQueue queue;
  vkGetDeviceQueue(dev, family, 0, &queue);

  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(nf);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, formats.data());

  VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  swci.surface = surface;
  swci.minImageCount = caps.minImageCount + 1;
  swci.imageFormat = formats[0].format;
  swci.imageColorSpace = formats[0].colorSpace;
  swci.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{640, 480};
  swci.imageArrayLayers = 1;
  swci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  swci.preTransform = caps.currentTransform;
  swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swci.clipped = VK_TRUE;
  VkSwapchainKHR swapchain;
  VK(vkCreateSwapchainKHR(dev, &swci, nullptr, &swapchain));
  uint32_t nimg = 0;
  vkGetSwapchainImagesKHR(dev, swapchain, &nimg, nullptr);
  std::vector<VkImage> images(nimg);
  vkGetSwapchainImagesKHR(dev, swapchain, &nimg, images.data());

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.queueFamilyIndex = family;
  VkCommandPool pool;
  VK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
  std::vector<VkCommandBuffer> cmds(nimg);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = nimg;
  VK(vkAllocateCommandBuffers(dev, &cai, cmds.data()));
  for (uint32_t i = 0; i < nimg; ++i) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmds[i], &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = images[i];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmds[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkClearColorValue blue{{0.0f, 0.0f, 1.0f, 1.0f}};
    vkCmdClearColorImage(cmds[i], images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue, 1, &b.subresourceRange);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmds[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    vkEndCommandBuffer(cmds[i]);
  }

  constexpr int kFrames = 2;
  VkSemaphore acquired[kFrames];
  VkFence fences[kFrames];
  std::vector<VkSemaphore> rendered(nimg);
  VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (int i = 0; i < kFrames; ++i) {
    vkCreateSemaphore(dev, &semci, nullptr, &acquired[i]);
    vkCreateFence(dev, &fci, nullptr, &fences[i]);
  }
  for (auto &s : rendered) vkCreateSemaphore(dev, &semci, nullptr, &s);

  const auto start = std::chrono::steady_clock::now();
  uint64_t frame = 0;
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
    const int f = int(frame++ % kFrames);
    vkWaitForFences(dev, 1, &fences[f], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &fences[f]);
    uint32_t idx;
    VK(vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquired[f], VK_NULL_HANDLE, &idx));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired[f];
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmds[idx];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered[idx];
    VK(vkQueueSubmit(queue, 1, &si, fences[f]));
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &idx;
    VK(vkQueuePresentKHR(queue, &pi));
  }
  vkDeviceWaitIdle(dev);
  std::printf("%llu quadros\n", (unsigned long long)frame);

  for (auto &s : rendered) vkDestroySemaphore(dev, s, nullptr);
  for (int i = 0; i < kFrames; ++i) {
    vkDestroySemaphore(dev, acquired[i], nullptr);
    vkDestroyFence(dev, fences[i], nullptr);
  }
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroySwapchainKHR(dev, swapchain, nullptr);
  vkDestroyDevice(dev, nullptr);
  vkDestroySurfaceKHR(inst, surface, nullptr);
  vkDestroyInstance(inst, nullptr);
  xcb_disconnect(c);
  return 0;
}
