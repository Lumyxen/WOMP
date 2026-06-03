#pragma once

#include "womp/platform/WaylandWindow.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace womp {

class VulkanRenderer {
public:
    enum class SdfShapeKind : std::uint32_t {
        RoundedRect = 0,
        Circle = 1,
        Line = 2,
    };

    struct Color {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    struct SdfShape {
        SdfShapeKind kind = SdfShapeKind::RoundedRect;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float radius = 0.0f;
        float strokeWidth = 0.0f;
        Color fill{};
        Color stroke{};
    };

    explicit VulkanRenderer(WaylandWindow& window);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void setSdfShapes(std::vector<SdfShape> shapes);
    void clearSdfShapes();
    void addSdfRoundedRect(float x, float y, float width, float height, float radius, Color fill);
    void addSdfRoundedRect(float x, float y, float width, float height, float radius, Color fill, float strokeWidth, Color stroke);
    void addSdfCircle(float centerX, float centerY, float radius, Color fill);
    void addSdfCircle(float centerX, float centerY, float radius, Color fill, float strokeWidth, Color stroke);
    void addSdfLine(float x0, float y0, float x1, float y1, float thickness, Color fill);

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
    void createPipelines();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void recordCommandBuffer(VkCommandBuffer commandBuffer, VkFramebuffer framebuffer);

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
    VkPipelineLayout backgroundPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline backgroundPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout sdfPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline sdfPipeline_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<SdfShape> sdfShapes_;

    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;
    VkFence frameInFlight_ = VK_NULL_HANDLE;
};

} // namespace womp
