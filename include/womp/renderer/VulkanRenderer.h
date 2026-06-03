#pragma once

#include "womp/platform/WaylandWindow.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace womp {

class VulkanRenderer {
public:
    explicit VulkanRenderer(WaylandWindow& window);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void drawFrame();
    void recreateSwapchain();
    void waitIdle() const;

private:
    struct QueueFamily {
        std::uint32_t index = 0;
        bool found = false;
    };

    void createInstance();
    void createSurface();
    void selectPhysicalDevice();
    void createDevice();
    void createSwapchain();
    void createImageViews();
    void createRenderPass();
    void createPipeline();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();

    void cleanupSwapchain();
    QueueFamily findGraphicsPresentQueue(VkPhysicalDevice device) const;
    VkSurfaceFormatKHR selectSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR selectPresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D selectSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    VkShaderModule createShaderModule(const char* filename) const;

    WaylandWindow& window_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;

    std::uint32_t queueFamilyIndex_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkFramebuffer> framebuffers_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;
    VkFence frameInFlight_ = VK_NULL_HANDLE;
};

} // namespace womp
