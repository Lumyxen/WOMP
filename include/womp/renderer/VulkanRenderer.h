#pragma once

#include "womp/platform/WaylandWindow.h"
#include "womp/scene/Primitive.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace womp {

class VulkanRenderer {
public:
    enum class PrimitiveUpdate : std::uint8_t {
        DrawOnly = 0,
        Text = 1,
        Svg = 2,
        Full = 3,
    };

    explicit VulkanRenderer(WaylandWindow& window);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void setPrimitives(std::vector<Primitive> primitives, PrimitiveUpdate update = PrimitiveUpdate::Full);
    void clearPrimitives();
    void addPrimitive(Primitive primitive);

    void drawFrame();
    void recreateSwapchain();
    void waitIdle() const;
    float measureTextVisualWidth(
        const std::string& text,
        const std::vector<std::string>& fontFamilies,
        float fontSize) const;

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
    void createTextDescriptorSetLayout();
    void createPipelines();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void recordCommandBuffer(VkCommandBuffer commandBuffer, VkFramebuffer framebuffer);
    void rebuildTextAtlas();
    void rebuildSvgAtlas();
    void waitForFrameIdle() const;

    void cleanupSwapchain();
    void cleanupTextAtlas();
    void cleanupSvgAtlas();
    QueueFamily findGraphicsPresentQueue(VkPhysicalDevice device) const;
    VkSurfaceFormatKHR selectSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR selectPresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D selectSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    VkShaderModule createShaderModule(const char* filename) const;
    std::uint32_t findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

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
    VkDescriptorSetLayout textDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool textDescriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet textDescriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout textPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline textPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool svgDescriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet svgDescriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout svgPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline svgPipeline_ = VK_NULL_HANDLE;
    VkImage svgAtlasImage_ = VK_NULL_HANDLE;
    VkDeviceMemory svgAtlasMemory_ = VK_NULL_HANDLE;
    VkImageView svgAtlasView_ = VK_NULL_HANDLE;
    VkSampler svgAtlasSampler_ = VK_NULL_HANDLE;
    std::uint32_t svgAtlasWidth_ = 0;
    std::uint32_t svgAtlasHeight_ = 0;
    VkImage textAtlasImage_ = VK_NULL_HANDLE;
    VkDeviceMemory textAtlasMemory_ = VK_NULL_HANDLE;
    VkImageView textAtlasView_ = VK_NULL_HANDLE;
    VkSampler textAtlasSampler_ = VK_NULL_HANDLE;
    std::uint32_t textAtlasWidth_ = 0;
    std::uint32_t textAtlasHeight_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<Primitive> primitives_;

    struct TextGlyphDraw {
        float rect[4]{};
        float uv[4]{};
        Color color{};
    };

    std::unordered_map<PrimitiveId, std::vector<TextGlyphDraw>> textGlyphs_;
    std::unordered_map<PrimitiveId, float> textCaretX_;

    struct SvgDraw {
        float rect[4]{};
        float uv[4]{};
        Color color{};
        float mode = 0.0f;
    };

    std::unordered_map<PrimitiveId, SvgDraw> svgDraws_;

    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;
    VkFence frameInFlight_ = VK_NULL_HANDLE;
};

} // namespace womp
