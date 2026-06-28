#pragma once

#include <cstdint>
#include <memory>
#include <string>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "vkray_scene.h"

namespace vkray {

// ============================================================================
// IRenderer: 抽象渲染器接口
// ----------------------------------------------------------------------------
// 当前仅 ComputePathTracer 实现。未来可新增 HardwareRayTracer（基于
// VK_KHR_ray_tracing_pipeline）作为兄弟实现，无需改动窗口/模块层。
// ============================================================================
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool init(HWND hwnd, uint32_t width, uint32_t height) = 0;
    virtual void renderFrame() = 0;
    virtual void onResize(uint32_t width, uint32_t height) = 0;
    virtual void onCameraChanged() = 0;
    virtual void setCamera(const OrbitCamera& cam) = 0;
    virtual void shutdown() = 0;
};

// ============================================================================
// ComputePathTracer: 基于 compute shader 的 GPU 路径追踪器
// ============================================================================
class ComputePathTracer : public IRenderer {
public:
    ComputePathTracer();
    ~ComputePathTracer() override;

    bool init(HWND hwnd, uint32_t width, uint32_t height) override;
    void renderFrame() override;
    void onResize(uint32_t width, uint32_t height) override;
    void onCameraChanged() override { frameCount_ = 0; }
    void setCamera(const OrbitCamera& cam) override;
    void shutdown() override;

private:
    // Vulkan 核心句柄
    VkInstance       instance_       = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice         device_         = VK_NULL_HANDLE;
    VkQueue          queue_          = VK_NULL_HANDLE;
    uint32_t         queueFamily_    = ~0u;
    VkSurfaceKHR     surface_        = VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain_      = VK_NULL_HANDLE;

    // Swapchain 资源
    VkFormat         swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D       swapchainExtent_ = {0, 0};
    std::vector<VkImage>     swapchainImages_;
    std::vector<VkImageView> swapchainViews_;

    // 累积图像（compute 写入 / graphics 采样）
    VkImage        accumImage_       = VK_NULL_HANDLE;
    VkDeviceMemory accumMemory_      = VK_NULL_HANDLE;
    VkImageView    accumViewCompute_ = VK_NULL_HANDLE; // GENERAL，compute 写
    VkImageView    accumViewSample_  = VK_NULL_HANDLE; // SHADER_READ_ONLY_OPTIMAL，graphics 采样
    VkSampler      accumSampler_     = VK_NULL_HANDLE; // present pass 采样 accum

    // 场景 UBO + SSBO
    VkBuffer       cameraUbo_       = VK_NULL_HANDLE;
    VkDeviceMemory cameraUboMem_    = VK_NULL_HANDLE;
    VkBuffer       sphereBuf_       = VK_NULL_HANDLE;
    VkDeviceMemory sphereBufMem_    = VK_NULL_HANDLE;
    VkBuffer       wallBuf_         = VK_NULL_HANDLE;
    VkDeviceMemory wallBufMem_      = VK_NULL_HANDLE;
    VkBuffer       lightBuf_        = VK_NULL_HANDLE;
    VkDeviceMemory lightBufMem_     = VK_NULL_HANDLE;
    VkBuffer       presentUbo_      = VK_NULL_HANDLE;
    VkDeviceMemory presentUboMem_   = VK_NULL_HANDLE;

    // Descriptor
    VkDescriptorPool      descriptorPool_  = VK_NULL_HANDLE;
    VkDescriptorSetLayout computeSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout presentSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet       computeSet_      = VK_NULL_HANDLE;
    VkDescriptorSet       presentSet_      = VK_NULL_HANDLE;

    // Pipeline
    VkPipelineLayout computePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       computePipeline_       = VK_NULL_HANDLE;
    VkPipelineLayout presentPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       presentPipeline_       = VK_NULL_HANDLE;
    VkRenderPass     presentRenderPass_     = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> presentFramebuffers_;

    // Command
    VkCommandPool   commandPool_   = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence         frameFence_    = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable_ = VK_NULL_HANDLE;  // single (1 frame in flight)
    std::vector<VkSemaphore> renderFinished_;           // per swapchain image

    // 状态
    HWND          hwnd_        = nullptr;
    uint32_t      width_       = 0;
    uint32_t      height_      = 0;
    uint32_t      frameCount_  = 0;
    OrbitCamera   camera_{};
    Scene         scene_{};

    // 内部辅助
    bool createInstance();
    bool pickPhysicalDevice();
    bool createDeviceAndQueue();
    bool createSurface(HWND hwnd);
    bool createSwapchain();
    bool createAccumImage();
    bool createSceneBuffers();
    bool createDescriptorPool();
    bool createComputeLayout();
    bool createPresentLayout();
    bool createComputePipeline();
    bool createPresentPipeline();
    bool createCommandResources();
    bool createFramebuffers();

    void destroySwapchainObjects();
    void destroyAll();

    // SPIR-V 文件加载
    static std::vector<uint32_t> loadSpv(const std::string& path);

    // 找到合适的内存类型
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const;
};

} // namespace vkray
