// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include "platform/vulkan.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace nanorv::platform {
namespace {
void checked(VkResult result,const char* operation) {
  if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(result));
}
bool matches(HostGpu wanted,std::uint32_t vendor) {
  return wanted==HostGpu::Auto || (wanted==HostGpu::Nvidia && vendor==0x10de) ||
    (wanted==HostGpu::Intel && vendor==0x8086) || (wanted==HostGpu::Amd && vendor==0x1002);
}
}
struct VulkanPresenter::Impl {
  HMODULE library=nullptr;
  HWND window=nullptr;
  VkInstance instance=VK_NULL_HANDLE;
  VkPhysicalDevice physical=VK_NULL_HANDLE;
  VkDevice device=VK_NULL_HANDLE;
  VkSurfaceKHR surface=VK_NULL_HANDLE;
  VkSwapchainKHR swapchain=VK_NULL_HANDLE;
  VkQueue queue=VK_NULL_HANDLE;
  VkCommandPool pool=VK_NULL_HANDLE;
  VkExtent2D extent{};
  std::uint32_t family=0,current=0;
  bool resize=true;
  std::string name;
  std::vector<VkImage> images;
  // Present waits have no fence in Vulkan 1.0. A semaphore per swapchain image
  // is reused only after that same image is reacquired by the presentation API.
  std::vector<VkSemaphore> rendered;
  struct Frame {
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkSemaphore acquired=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory=VK_NULL_HANDLE,image_memory=VK_NULL_HANDLE;
    VkImage image=VK_NULL_HANDLE;
    void* mapped=nullptr;
    unsigned width=0,height=0;
  };
  std::array<Frame,2> frames{};
#define FN(n) PFN_vk##n n=nullptr
  FN(GetInstanceProcAddr);FN(GetDeviceProcAddr);FN(CreateInstance);FN(DestroyInstance);
  FN(EnumeratePhysicalDevices);FN(GetPhysicalDeviceProperties);FN(GetPhysicalDeviceMemoryProperties);
  FN(GetPhysicalDeviceQueueFamilyProperties);FN(GetPhysicalDeviceFormatProperties);
  FN(EnumerateDeviceExtensionProperties);FN(CreateDevice);FN(DestroyDevice);FN(GetDeviceQueue);
  FN(CreateWin32SurfaceKHR);FN(DestroySurfaceKHR);FN(GetPhysicalDeviceSurfaceSupportKHR);
  FN(GetPhysicalDeviceSurfaceCapabilitiesKHR);FN(GetPhysicalDeviceSurfaceFormatsKHR);
  FN(GetPhysicalDeviceSurfacePresentModesKHR);FN(CreateSwapchainKHR);FN(DestroySwapchainKHR);
  FN(GetSwapchainImagesKHR);FN(AcquireNextImageKHR);FN(QueuePresentKHR);
  FN(CreateCommandPool);FN(DestroyCommandPool);FN(AllocateCommandBuffers);FN(ResetCommandBuffer);
  FN(BeginCommandBuffer);FN(EndCommandBuffer);FN(CmdPipelineBarrier);FN(CmdCopyBufferToImage);
  FN(CmdBlitImage);FN(CmdCopyImageToBuffer);FN(CreateBuffer);FN(DestroyBuffer);
  FN(GetBufferMemoryRequirements);FN(CreateImage);FN(DestroyImage);FN(GetImageMemoryRequirements);
  FN(AllocateMemory);FN(FreeMemory);FN(BindBufferMemory);FN(BindImageMemory);FN(MapMemory);FN(UnmapMemory);
  FN(CreateSemaphore);FN(DestroySemaphore);FN(CreateFence);FN(DestroyFence);FN(WaitForFences);
  FN(ResetFences);FN(QueueSubmit);FN(DeviceWaitIdle);
#undef FN
  ~Impl() {
    if(device && DeviceWaitIdle)DeviceWaitIdle(device);
    if(device) {
      clear_swapchain();
      for(auto& frame:frames) {
        clear_upload(frame);
        if(frame.acquired)DestroySemaphore(device,frame.acquired,nullptr);
        if(frame.fence)DestroyFence(device,frame.fence,nullptr);
      }
      if(pool)DestroyCommandPool(device,pool,nullptr);
      DestroyDevice(device,nullptr);
    }
    if(surface)DestroySurfaceKHR(instance,surface,nullptr);
    if(instance)DestroyInstance(instance,nullptr);
    if(library)FreeLibrary(library);
  }
  template<class T> void instance_fn(T& fn,const char* symbol) {
    fn=reinterpret_cast<T>(GetInstanceProcAddr(instance,symbol));
    if(!fn)throw std::runtime_error(std::string("Vulkan loader missing ")+symbol);
  }
  template<class T> void device_fn(T& fn,const char* symbol) {
    fn=reinterpret_cast<T>(GetDeviceProcAddr(device,symbol));
    if(!fn)throw std::runtime_error(std::string("Vulkan device missing ")+symbol);
  }
  void initialize() {
    library=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!library)throw std::runtime_error("System32/vulkan-1.dll unavailable; install the host GPU Vulkan driver");
    GetInstanceProcAddr=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library,"vkGetInstanceProcAddr"));
    if(!GetInstanceProcAddr)throw std::runtime_error("Vulkan loader missing vkGetInstanceProcAddr");
    instance_fn(CreateInstance,"vkCreateInstance");
    const char* extensions[]{VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName="NanoRV";application.apiVersion=VK_API_VERSION_1_0;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&application;
    if(window) {create.enabledExtensionCount=2;create.ppEnabledExtensionNames=extensions;}
    checked(CreateInstance(&create,nullptr,&instance),"vkCreateInstance");
#define LOAD(n) instance_fn(n,"vk" #n)
    LOAD(DestroyInstance);LOAD(EnumeratePhysicalDevices);LOAD(GetPhysicalDeviceProperties);
    LOAD(GetPhysicalDeviceMemoryProperties);LOAD(GetPhysicalDeviceQueueFamilyProperties);
    LOAD(GetPhysicalDeviceFormatProperties);LOAD(EnumerateDeviceExtensionProperties);
    LOAD(CreateDevice);LOAD(GetDeviceProcAddr);
    if(window) {
      LOAD(CreateWin32SurfaceKHR);LOAD(DestroySurfaceKHR);LOAD(GetPhysicalDeviceSurfaceSupportKHR);
      LOAD(GetPhysicalDeviceSurfaceCapabilitiesKHR);LOAD(GetPhysicalDeviceSurfaceFormatsKHR);
      LOAD(GetPhysicalDeviceSurfacePresentModesKHR);
      VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
      info.hinstance=GetModuleHandleW(nullptr);info.hwnd=window;
      checked(CreateWin32SurfaceKHR(instance,&info,nullptr,&surface),"vkCreateWin32SurfaceKHR");
    }
#undef LOAD
  }
  std::vector<VkPhysicalDevice> devices() {
    std::uint32_t count=0;checked(EnumeratePhysicalDevices(instance,&count,nullptr),"vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> result(count);
    checked(EnumeratePhysicalDevices(instance,&count,result.data()),"vkEnumeratePhysicalDevices");
    result.resize(count);return result;
  }
  void select(HostGpu wanted) {
    int best=-1;
    for(auto candidate:devices()) {
      VkPhysicalDeviceProperties properties{};GetPhysicalDeviceProperties(candidate,&properties);
      if(!matches(wanted,properties.vendorID) || properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
      VkFormatProperties format{};GetPhysicalDeviceFormatProperties(candidate,VK_FORMAT_B8G8R8A8_UNORM,&format);
      if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))continue;
      if(surface) {
        std::uint32_t count=0;checked(EnumerateDeviceExtensionProperties(candidate,nullptr,&count,nullptr),"device extensions");
        std::vector<VkExtensionProperties> extensions(count);
        checked(EnumerateDeviceExtensionProperties(candidate,nullptr,&count,extensions.data()),"device extensions");
        if(std::none_of(extensions.begin(),extensions.end(),[](const auto& e){return std::strcmp(e.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME)==0;}))continue;
      }
      std::uint32_t count=0;GetPhysicalDeviceQueueFamilyProperties(candidate,&count,nullptr);
      std::vector<VkQueueFamilyProperties> families(count);GetPhysicalDeviceQueueFamilyProperties(candidate,&count,families.data());
      for(std::uint32_t index=0;index<count;++index) {
        if(!families[index].queueCount || !(families[index].queueFlags&VK_QUEUE_GRAPHICS_BIT))continue;
        VkBool32 supported=VK_TRUE;
        if(surface)checked(GetPhysicalDeviceSurfaceSupportKHR(candidate,index,surface,&supported),"surface support");
        if(!supported)continue;
        const int score=(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?100:10)+(properties.vendorID==0x10de?1:0);
        if(score>best) {best=score;physical=candidate;family=index;name=properties.deviceName;}
        break;
      }
    }
    if(!physical)throw std::runtime_error("no compatible hardware Vulkan GPU matches --host-gpu (software devices are excluded)");
    const float priority=1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex=family;queue_info.queueCount=1;queue_info.pQueuePriorities=&priority;
    const char* extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};create.queueCreateInfoCount=1;create.pQueueCreateInfos=&queue_info;
    if(surface) {create.enabledExtensionCount=1;create.ppEnabledExtensionNames=&extension;}
    checked(CreateDevice(physical,&create,nullptr,&device),"vkCreateDevice");
#define LOAD(n) device_fn(n,"vk" #n)
    LOAD(DestroyDevice);LOAD(GetDeviceQueue);LOAD(CreateCommandPool);LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);LOAD(ResetCommandBuffer);LOAD(BeginCommandBuffer);LOAD(EndCommandBuffer);
    LOAD(CmdPipelineBarrier);LOAD(CmdCopyBufferToImage);LOAD(CmdBlitImage);LOAD(CmdCopyImageToBuffer);
    LOAD(CreateBuffer);LOAD(DestroyBuffer);LOAD(GetBufferMemoryRequirements);LOAD(CreateImage);LOAD(DestroyImage);
    LOAD(GetImageMemoryRequirements);LOAD(AllocateMemory);LOAD(FreeMemory);LOAD(BindBufferMemory);LOAD(BindImageMemory);
    LOAD(MapMemory);LOAD(UnmapMemory);LOAD(CreateSemaphore);LOAD(DestroySemaphore);LOAD(CreateFence);LOAD(DestroyFence);
    LOAD(WaitForFences);LOAD(ResetFences);LOAD(QueueSubmit);LOAD(DeviceWaitIdle);
    if(surface) {LOAD(CreateSwapchainKHR);LOAD(DestroySwapchainKHR);LOAD(GetSwapchainImagesKHR);LOAD(AcquireNextImageKHR);LOAD(QueuePresentKHR);}
#undef LOAD
    GetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool_info.queueFamilyIndex=family;
    checked(CreateCommandPool(device,&pool_info,nullptr,&pool),"vkCreateCommandPool");
    for(auto& frame:frames) {
      VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      command.commandPool=pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;
      checked(AllocateCommandBuffers(device,&command,&frame.command),"vkAllocateCommandBuffers");
      VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      checked(CreateSemaphore(device,&semaphore,nullptr,&frame.acquired),"vkCreateSemaphore");
      VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
      checked(CreateFence(device,&fence,nullptr,&frame.fence),"vkCreateFence");
    }
  }
  std::uint32_t memory_type(std::uint32_t bits,VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties properties{};GetPhysicalDeviceMemoryProperties(physical,&properties);
    for(std::uint32_t n=0;n<properties.memoryTypeCount;++n)
      if((bits&(1u<<n)) && (properties.memoryTypes[n].propertyFlags&wanted)==wanted)return n;
    throw std::runtime_error("Vulkan GPU has no suitable memory type");
  }
  void allocate(VkMemoryRequirements requirements,VkMemoryPropertyFlags flags,VkDeviceMemory& memory) {
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize=requirements.size;info.memoryTypeIndex=memory_type(requirements.memoryTypeBits,flags);
    checked(AllocateMemory(device,&info,nullptr,&memory),"vkAllocateMemory");
  }
  void clear_upload(Frame& frame) {
    if(frame.mapped)UnmapMemory(device,frame.buffer_memory);
    if(frame.buffer)DestroyBuffer(device,frame.buffer,nullptr);
    if(frame.buffer_memory)FreeMemory(device,frame.buffer_memory,nullptr);
    if(frame.image)DestroyImage(device,frame.image,nullptr);
    if(frame.image_memory)FreeMemory(device,frame.image_memory,nullptr);
    frame.mapped=nullptr;frame.buffer=VK_NULL_HANDLE;frame.buffer_memory=frame.image_memory=VK_NULL_HANDLE;
    frame.image=VK_NULL_HANDLE;frame.width=frame.height=0;
  }
  void upload(Frame& frame,unsigned width,unsigned height) {
    if(frame.width==width && frame.height==height)return;
    clear_upload(frame);
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=static_cast<VkDeviceSize>(width)*height*4;
    buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    checked(CreateBuffer(device,&buffer,nullptr,&frame.buffer),"vkCreateBuffer");
    VkMemoryRequirements requirements{};GetBufferMemoryRequirements(device,frame.buffer,&requirements);
    allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,frame.buffer_memory);
    checked(BindBufferMemory(device,frame.buffer,frame.buffer_memory,0),"vkBindBufferMemory");
    checked(MapMemory(device,frame.buffer_memory,0,VK_WHOLE_SIZE,0,&frame.mapped),"vkMapMemory");
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;
    image.format=VK_FORMAT_B8G8R8A8_UNORM;image.extent={width,height,1};image.mipLevels=image.arrayLayers=1;
    image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    checked(CreateImage(device,&image,nullptr,&frame.image),"vkCreateImage");
    GetImageMemoryRequirements(device,frame.image,&requirements);allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,frame.image_memory);
    checked(BindImageMemory(device,frame.image,frame.image_memory,0),"vkBindImageMemory");
    frame.width=width;frame.height=height;
  }
  void clear_swapchain() {
    for(auto semaphore:rendered)DestroySemaphore(device,semaphore,nullptr);
    rendered.clear();images.clear();
    if(swapchain)DestroySwapchainKHR(device,swapchain,nullptr);
    swapchain=VK_NULL_HANDLE;
  }
  bool recreate() {
    RECT client{};GetClientRect(window,&client);
    if(client.right<=0 || client.bottom<=0 || IsIconic(window))return false;
    checked(DeviceWaitIdle(device),"vkDeviceWaitIdle (resize)");clear_swapchain();
    VkSurfaceCapabilitiesKHR caps{};checked(GetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"surface capabilities");
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("Vulkan surface does not allow transfer destinations");
    extent=caps.currentExtent;
    if(extent.width==UINT32_MAX)extent={std::clamp(static_cast<std::uint32_t>(client.right),caps.minImageExtent.width,caps.maxImageExtent.width),
      std::clamp(static_cast<std::uint32_t>(client.bottom),caps.minImageExtent.height,caps.maxImageExtent.height)};
    if(!extent.width || !extent.height)return false;
    std::uint32_t count=0;checked(GetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"surface formats");
    std::vector<VkSurfaceFormatKHR> formats(count);checked(GetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"surface formats");
    auto chosen=std::find_if(formats.begin(),formats.end(),[](const auto& f){return f.format==VK_FORMAT_B8G8R8A8_UNORM;});
    if(chosen==formats.end())chosen=std::find_if(formats.begin(),formats.end(),[](const auto& f){return f.format==VK_FORMAT_R8G8B8A8_UNORM;});
    if(chosen==formats.end())throw std::runtime_error("Vulkan surface lacks an 8-bit UNORM format");
    VkFormatProperties properties{};GetPhysicalDeviceFormatProperties(physical,chosen->format,&properties);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))throw std::runtime_error("Vulkan surface format does not support blit");
    checked(GetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&count,nullptr),"present modes");
    std::vector<VkPresentModeKHR> modes(count);checked(GetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&count,modes.data()),"present modes");
    const auto mode=std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_MAILBOX_KHR)!=modes.end()?VK_PRESENT_MODE_MAILBOX_KHR:VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};info.surface=surface;
    info.minImageCount=caps.minImageCount+1;if(caps.maxImageCount)info.minImageCount=std::min(info.minImageCount,caps.maxImageCount);
    info.imageFormat=chosen->format;info.imageColorSpace=chosen->colorSpace;info.imageExtent=extent;info.imageArrayLayers=1;
    info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;info.preTransform=caps.currentTransform;
    for(const auto alpha:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
      if(caps.supportedCompositeAlpha&alpha) {info.compositeAlpha=alpha;break;}
    info.presentMode=mode;info.clipped=VK_TRUE;
    checked(CreateSwapchainKHR(device,&info,nullptr,&swapchain),"vkCreateSwapchainKHR");
    checked(GetSwapchainImagesKHR(device,swapchain,&count,nullptr),"swapchain images");images.resize(count);
    checked(GetSwapchainImagesKHR(device,swapchain,&count,images.data()),"swapchain images");images.resize(count);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(std::uint32_t n=0;n<count;++n) {
      VkSemaphore handle=VK_NULL_HANDLE;checked(CreateSemaphore(device,&semaphore,nullptr,&handle),"present semaphore");rendered.push_back(handle);
    }
    resize=false;return true;
  }
  void barrier(VkCommandBuffer command,VkImage image,VkImageLayout before,VkImageLayout after,VkAccessFlags source,VkAccessFlags destination,
      VkPipelineStageFlags source_stage,VkPipelineStageFlags destination_stage) {
    VkImageMemoryBarrier info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};info.oldLayout=before;info.newLayout=after;
    info.srcQueueFamilyIndex=info.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;info.image=image;
    info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};info.srcAccessMask=source;info.dstAccessMask=destination;
    CmdPipelineBarrier(command,source_stage,destination_stage,0,0,nullptr,0,nullptr,1,&info);
  }
  void begin(Frame& frame) {
    checked(ResetCommandBuffer(frame.command,0),"vkResetCommandBuffer");
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(BeginCommandBuffer(frame.command,&info),"vkBeginCommandBuffer");
    barrier(frame.command,frame.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={frame.width,frame.height,1};
    CmdCopyBufferToImage(frame.command,frame.buffer,frame.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier(frame.command,frame.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  }
  bool present(unsigned width,unsigned height,std::span<const std::uint32_t> pixels) {
    RECT client{};GetClientRect(window,&client);
    if(IsIconic(window) || client.right<=0 || client.bottom<=0)return true;
    if(static_cast<unsigned>(client.right)!=extent.width || static_cast<unsigned>(client.bottom)!=extent.height)resize=true;
    if((resize || !swapchain) && !recreate())return true;
    auto& frame=frames[current];checked(WaitForFences(device,1,&frame.fence,VK_TRUE,2'000'000'000ull),"frame fence");
    upload(frame,width,height);
    std::uint32_t image=0;
    const auto acquired=AcquireNextImageKHR(device,swapchain,2'000'000'000ull,frame.acquired,VK_NULL_HANDLE,&image);
    if(acquired==VK_ERROR_OUT_OF_DATE_KHR) {resize=true;return true;}
    if(acquired==VK_TIMEOUT || acquired==VK_NOT_READY)return true;
    if(acquired!=VK_SUBOPTIMAL_KHR)checked(acquired,"vkAcquireNextImageKHR");
    std::memcpy(frame.mapped,pixels.data(),pixels.size_bytes());begin(frame);
    barrier(frame.command,images[image],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    blit.srcOffsets[1]={static_cast<int>(width),static_cast<int>(height),1};blit.dstOffsets[1]={static_cast<int>(extent.width),static_cast<int>(extent.height),1};
    CmdBlitImage(frame.command,frame.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
    barrier(frame.command,images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,
      VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    checked(EndCommandBuffer(frame.command),"vkEndCommandBuffer");
    checked(ResetFences(device,1,&frame.fence),"vkResetFences");
    const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&frame.acquired;submit.pWaitDstStageMask=&stage;
    submit.commandBufferCount=1;submit.pCommandBuffers=&frame.command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&rendered[image];
    checked(QueueSubmit(queue,1,&submit,frame.fence),"vkQueueSubmit");
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};info.waitSemaphoreCount=1;info.pWaitSemaphores=&rendered[image];
    info.swapchainCount=1;info.pSwapchains=&swapchain;info.pImageIndices=&image;
    const auto result=QueuePresentKHR(queue,&info);
    resize=acquired==VK_SUBOPTIMAL_KHR || result==VK_SUBOPTIMAL_KHR || result==VK_ERROR_OUT_OF_DATE_KHR;
    if(result!=VK_SUBOPTIMAL_KHR && result!=VK_ERROR_OUT_OF_DATE_KHR)checked(result,"vkQueuePresentKHR");
    current=(current+1)%static_cast<std::uint32_t>(frames.size());return true;
  }
  void self_test() {
    auto& frame=frames[0];
    for(const auto size:{std::pair{17u,13u},std::pair{256u,192u},std::pair{33u,65u}}) {
      upload(frame,size.first,size.second);
      std::vector<std::uint32_t> expected(static_cast<std::size_t>(size.first)*size.second);
      for(std::size_t n=0;n<expected.size();++n)expected[n]=0xff000000u|static_cast<std::uint32_t>((n*7919)&0xffffff);
      std::memcpy(frame.mapped,expected.data(),expected.size()*4);begin(frame);
      // The upload buffer is reused for readback only after the earlier transfer
      // read is complete; the image also carries a write-to-read dependency.
      VkBufferMemoryBarrier reuse{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};reuse.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      reuse.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;reuse.srcQueueFamilyIndex=reuse.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      reuse.buffer=frame.buffer;reuse.size=VK_WHOLE_SIZE;
      CmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&reuse,0,nullptr);
      VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={size.first,size.second,1};
      CmdCopyImageToBuffer(frame.command,frame.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,frame.buffer,1,&copy);
      VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=frame.buffer;host.size=VK_WHOLE_SIZE;
      CmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
      checked(EndCommandBuffer(frame.command),"self-test command");checked(ResetFences(device,1,&frame.fence),"self-test reset");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&frame.command;
      checked(QueueSubmit(queue,1,&submit,frame.fence),"self-test submit");checked(WaitForFences(device,1,&frame.fence,VK_TRUE,2'000'000'000ull),"self-test fence");
      if(std::memcmp(frame.mapped,expected.data(),expected.size()*4))throw std::runtime_error("Vulkan GPU readback pixel mismatch");
    }
  }
};
VulkanPresenter::VulkanPresenter():impl_(std::make_unique<Impl>()) {}
VulkanPresenter::~VulkanPresenter()=default;
bool VulkanPresenter::open(void* window,HostGpu gpu,std::string& error) {
  if(impl_->device) {error="Vulkan presenter already open";return false;}
  if(!window) {error="Vulkan presentation requires a Win32 window";return false;}
  try {impl_->window=static_cast<HWND>(window);impl_->initialize();impl_->select(gpu);return true;}
  catch(const std::exception& e) {error=e.what();impl_=std::make_unique<Impl>();return false;}
}
bool VulkanPresenter::present(unsigned width,unsigned height,std::span<const std::uint32_t> pixels,std::string& error) {
  if(!impl_->device || !width || !height || width>16384 || height>16384 || pixels.size()!=static_cast<std::size_t>(width)*height) {
    error="invalid Vulkan framebuffer or unopened presenter";return false;
  }
  try {return impl_->present(width,height,pixels);}catch(const std::exception& e) {error=e.what();return false;}
}
std::string VulkanPresenter::description() const {return impl_->name;}
bool VulkanPresenter::enumerate(std::vector<VulkanDevice>& output,std::string& error) {
  output.clear();
  try {
    Impl state;state.initialize();
    for(auto device:state.devices()) {
      VkPhysicalDeviceProperties p{};state.GetPhysicalDeviceProperties(device,&p);
      output.push_back({p.deviceName,p.vendorID,p.deviceID,p.apiVersion,p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,p.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU});
    }
    return true;
  }catch(const std::exception& e) {error=e.what();return false;}
}
bool VulkanPresenter::self_test(HostGpu gpu,std::string& description,std::string& error) {
  try {Impl state;state.initialize();state.select(gpu);state.self_test();description=state.name;return true;}
  catch(const std::exception& e) {error=e.what();return false;}
}
}
