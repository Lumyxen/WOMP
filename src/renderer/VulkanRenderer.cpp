#include "womp/renderer/VulkanRenderer.h"

#include "womp/renderer/EmbeddedShaders.h"

#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace womp {

namespace {

constexpr std::array deviceExtensions{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

struct SdfPushConstants {
    float rect[4]{};
    float fill[4]{};
    float stroke[4]{};
    float params[4]{};
};

void require(VkResult result, const char* message)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message);
    }
}

} // namespace

VulkanRenderer::VulkanRenderer(WaylandWindow& window)
    : window_(window)
{
    createInstance();
    createSurface();
    selectPhysicalDevice();
    createDevice();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createPipelines();
    createFramebuffers();
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
}

VulkanRenderer::~VulkanRenderer()
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        cleanupSwapchain();

        vkDestroySemaphore(device_, renderFinished_, nullptr);
        vkDestroySemaphore(device_, imageAvailable_, nullptr);
        vkDestroyFence(device_, frameInFlight_, nullptr);
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }

    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void VulkanRenderer::setSdfShapes(std::vector<SdfShape> shapes)
{
    sdfShapes_ = std::move(shapes);
    if (!commandBuffers_.empty() && !framebuffers_.empty()) {
        vkDeviceWaitIdle(device_);
        createCommandBuffers();
    }
}

void VulkanRenderer::clearSdfShapes()
{
    setSdfShapes({});
}

void VulkanRenderer::addSdfRoundedRect(float x, float y, float width, float height, float radius, Color fill)
{
    addSdfRoundedRect(x, y, width, height, radius, fill, 0.0f, {});
}

void VulkanRenderer::addSdfRoundedRect(float x, float y, float width, float height, float radius, Color fill, float strokeWidth, Color stroke)
{
    sdfShapes_.push_back({
        .kind = SdfShapeKind::RoundedRect,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .radius = radius,
        .strokeWidth = strokeWidth,
        .fill = fill,
        .stroke = stroke,
    });
    if (!commandBuffers_.empty() && !framebuffers_.empty()) {
        vkDeviceWaitIdle(device_);
        createCommandBuffers();
    }
}

void VulkanRenderer::addSdfCircle(float centerX, float centerY, float radius, Color fill)
{
    addSdfCircle(centerX, centerY, radius, fill, 0.0f, {});
}

void VulkanRenderer::addSdfCircle(float centerX, float centerY, float radius, Color fill, float strokeWidth, Color stroke)
{
    sdfShapes_.push_back({
        .kind = SdfShapeKind::Circle,
        .x = centerX,
        .y = centerY,
        .width = radius,
        .height = radius,
        .radius = radius,
        .strokeWidth = strokeWidth,
        .fill = fill,
        .stroke = stroke,
    });
    if (!commandBuffers_.empty() && !framebuffers_.empty()) {
        vkDeviceWaitIdle(device_);
        createCommandBuffers();
    }
}

void VulkanRenderer::addSdfLine(float x0, float y0, float x1, float y1, float thickness, Color fill)
{
    sdfShapes_.push_back({
        .kind = SdfShapeKind::Line,
        .x = x0,
        .y = y0,
        .width = x1,
        .height = y1,
        .radius = thickness,
        .fill = fill,
    });
    if (!commandBuffers_.empty() && !framebuffers_.empty()) {
        vkDeviceWaitIdle(device_);
        createCommandBuffers();
    }
}

void VulkanRenderer::drawFrame()
{
    require(vkWaitForFences(device_, 1, &frameInFlight_, VK_TRUE, UINT64_MAX), "failed to wait for frame fence");
    require(vkResetFences(device_, 1, &frameInFlight_), "failed to reset frame fence");

    std::uint32_t imageIndex = 0;
    const VkResult acquireResult = vkAcquireNextImageKHR(
        device_,
        swapchain_,
        UINT64_MAX,
        imageAvailable_,
        VK_NULL_HANDLE,
        &imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    require(acquireResult, "failed to acquire swapchain image");

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &imageAvailable_,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffers_[imageIndex],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &renderFinished_,
    };

    require(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frameInFlight_), "failed to submit draw commands");

    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &renderFinished_,
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &imageIndex,
    };

    const VkResult presentResult = vkQueuePresentKHR(graphicsQueue_, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
        return;
    }
    require(presentResult, "failed to present swapchain image");
}

void VulkanRenderer::recreateSwapchain()
{
    if (window_.width() == 0 || window_.height() == 0) {
        return;
    }

    vkDeviceWaitIdle(device_);
    cleanupSwapchain();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createPipelines();
    createFramebuffers();
    createCommandBuffers();
}

void VulkanRenderer::waitIdle() const
{
    vkDeviceWaitIdle(device_);
}

void VulkanRenderer::createInstance()
{
    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "womp",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "womp",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };

    constexpr std::array extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };

    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    require(vkCreateInstance(&createInfo, nullptr, &instance_), "failed to create Vulkan instance");
}

void VulkanRenderer::createSurface()
{
    const VkWaylandSurfaceCreateInfoKHR createInfo{
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = window_.display(),
        .surface = window_.surface(),
    };

    require(vkCreateWaylandSurfaceKHR(instance_, &createInfo, nullptr, &surface_), "failed to create Wayland Vulkan surface");
}

void VulkanRenderer::selectPhysicalDevice()
{
    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("no Vulkan physical devices found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    for (VkPhysicalDevice device : devices) {
        const QueueFamily queue = findGraphicsPresentQueue(device);
        if (!queue.found) {
            continue;
        }

        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

        const bool hasSwapchain = std::ranges::any_of(extensions, [](const VkExtensionProperties& extension) {
            return std::string(extension.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        });

        if (hasSwapchain) {
            physicalDevice_ = device;
            queueFamilyIndex_ = queue.index;
            return;
        }
    }

    throw std::runtime_error("no Vulkan device supports graphics, present, and swapchain");
}

void VulkanRenderer::createDevice()
{
    constexpr float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamilyIndex_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    const VkDeviceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
    };

    require(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "failed to create Vulkan device");
    vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &graphicsQueue_);
}

void VulkanRenderer::createSwapchain()
{
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());

    std::uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, modes.data());

    const VkSurfaceFormatKHR surfaceFormat = selectSurfaceFormat(formats);
    const VkPresentModeKHR presentMode = selectPresentMode(modes);
    const VkExtent2D extent = selectSwapExtent(capabilities);

    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    const VkSwapchainCreateInfoKHR createInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface_,
        .minImageCount = imageCount,
        .imageFormat = surfaceFormat.format,
        .imageColorSpace = surfaceFormat.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = presentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };

    require(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "failed to create Vulkan swapchain");

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    swapchainFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
}

void VulkanRenderer::createImageViews()
{
    swapchainImageViews_.resize(swapchainImages_.size());

    for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
        const VkImageViewCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchainImages_[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchainFormat_,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };

        require(vkCreateImageView(device_, &createInfo, nullptr, &swapchainImageViews_[i]), "failed to create swapchain image view");
    }
}

void VulkanRenderer::createRenderPass()
{
    const VkAttachmentDescription colorAttachment{
        .format = swapchainFormat_,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    const VkAttachmentReference colorReference{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    const VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorReference,
    };

    const VkSubpassDependency dependency{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };

    const VkRenderPassCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorAttachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    require(vkCreateRenderPass(device_, &createInfo, nullptr, &renderPass_), "failed to create render pass");
}

void VulkanRenderer::createPipelines()
{
    const auto createPipeline = [&](const char* vertexShaderName,
                                    const char* fragmentShaderName,
                                    VkPipelineLayout pipelineLayout,
                                    VkPipelineColorBlendAttachmentState colorBlendAttachment) {
        const VkShaderModule vertexShader = createShaderModule(vertexShaderName);
        const VkShaderModule fragmentShader = createShaderModule(fragmentShaderName);

        const VkPipelineShaderStageCreateInfo vertexStage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertexShader,
            .pName = "main",
        };

        const VkPipelineShaderStageCreateInfo fragmentStage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragmentShader,
            .pName = "main",
        };

        const std::array stages{vertexStage, fragmentStage};
        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };

        const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };

        const VkViewport viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(swapchainExtent_.width),
            .height = static_cast<float>(swapchainExtent_.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        const VkRect2D scissor{
            .offset = {0, 0},
            .extent = swapchainExtent_,
        };

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
        };

        const VkPipelineRasterizationStateCreateInfo rasterizer{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };

        const VkPipelineMultisampleStateCreateInfo multisampling{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };

        const VkPipelineColorBlendStateCreateInfo colorBlending{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &colorBlendAttachment,
        };

        const VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &assembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisampling,
            .pColorBlendState = &colorBlending,
            .layout = pipelineLayout,
            .renderPass = renderPass_,
            .subpass = 0,
        };

        VkPipeline pipeline = VK_NULL_HANDLE;
        require(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "failed to create graphics pipeline");

        vkDestroyShaderModule(device_, fragmentShader, nullptr);
        vkDestroyShaderModule(device_, vertexShader, nullptr);

        return pipeline;
    };

    const VkPipelineLayoutCreateInfo backgroundLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    require(vkCreatePipelineLayout(device_, &backgroundLayoutInfo, nullptr, &backgroundPipelineLayout_), "failed to create background pipeline layout");

    const VkPushConstantRange sdfPushConstantRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(SdfPushConstants),
    };
    const VkPipelineLayoutCreateInfo sdfLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &sdfPushConstantRange,
    };
    require(vkCreatePipelineLayout(device_, &sdfLayoutInfo, nullptr, &sdfPipelineLayout_), "failed to create SDF pipeline layout");

    const VkPipelineColorBlendAttachmentState opaqueBlend{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    backgroundPipeline_ = createPipeline("background.vert.spv", "background.frag.spv", backgroundPipelineLayout_, opaqueBlend);

    const VkPipelineColorBlendAttachmentState alphaBlend{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    sdfPipeline_ = createPipeline("sdf.vert.spv", "sdf.frag.spv", sdfPipelineLayout_, alphaBlend);
}

void VulkanRenderer::createFramebuffers()
{
    framebuffers_.resize(swapchainImageViews_.size());

    for (std::size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        const VkImageView attachment = swapchainImageViews_[i];
        const VkFramebufferCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderPass_,
            .attachmentCount = 1,
            .pAttachments = &attachment,
            .width = swapchainExtent_.width,
            .height = swapchainExtent_.height,
            .layers = 1,
        };

        require(vkCreateFramebuffer(device_, &createInfo, nullptr, &framebuffers_[i]), "failed to create framebuffer");
    }
}

void VulkanRenderer::createCommandPool()
{
    const VkCommandPoolCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamilyIndex_,
    };

    require(vkCreateCommandPool(device_, &createInfo, nullptr, &commandPool_), "failed to create command pool");
}

void VulkanRenderer::createCommandBuffers()
{
    if (!commandBuffers_.empty()) {
        vkFreeCommandBuffers(device_, commandPool_, static_cast<std::uint32_t>(commandBuffers_.size()), commandBuffers_.data());
    }

    commandBuffers_.resize(framebuffers_.size());
    const VkCommandBufferAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size()),
    };

    require(vkAllocateCommandBuffers(device_, &allocateInfo, commandBuffers_.data()), "failed to allocate command buffers");

    for (std::size_t i = 0; i < commandBuffers_.size(); ++i) {
        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        require(vkBeginCommandBuffer(commandBuffers_[i], &beginInfo), "failed to begin command buffer");
        recordCommandBuffer(commandBuffers_[i], framebuffers_[i]);

        require(vkEndCommandBuffer(commandBuffers_[i]), "failed to record command buffer");
    }
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer commandBuffer, VkFramebuffer framebuffer)
{
    constexpr VkClearValue clearColor{{{0.0f, 0.0f, 0.0f, 1.0f}}};
    const VkRenderPassBeginInfo renderPassInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = renderPass_,
        .framebuffer = framebuffer,
        .renderArea = {{0, 0}, swapchainExtent_},
        .clearValueCount = 1,
        .pClearValues = &clearColor,
    };

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, backgroundPipeline_);
    vkCmdDraw(commandBuffer, 6, 1, 0, 0);

    if (!sdfShapes_.empty()) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sdfPipeline_);
        for (const SdfShape& shape : sdfShapes_) {
            const SdfPushConstants push{
                .rect = {shape.x, shape.y, shape.width, shape.height},
                .fill = {shape.fill.r, shape.fill.g, shape.fill.b, shape.fill.a},
                .stroke = {shape.stroke.r, shape.stroke.g, shape.stroke.b, shape.stroke.a},
                .params = {shape.radius, shape.strokeWidth, static_cast<float>(shape.kind), 1.0f},
            };
            vkCmdPushConstants(commandBuffer, sdfPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(commandBuffer, 6, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(commandBuffer);
}

void VulkanRenderer::createSyncObjects()
{
    const VkSemaphoreCreateInfo semaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    const VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    require(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_), "failed to create image semaphore");
    require(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_), "failed to create render semaphore");
    require(vkCreateFence(device_, &fenceInfo, nullptr, &frameInFlight_), "failed to create frame fence");
}

void VulkanRenderer::cleanupSwapchain()
{
    if (!commandBuffers_.empty()) {
        vkFreeCommandBuffers(device_, commandPool_, static_cast<std::uint32_t>(commandBuffers_.size()), commandBuffers_.data());
        commandBuffers_.clear();
    }

    for (VkFramebuffer framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.clear();

    vkDestroyPipeline(device_, sdfPipeline_, nullptr);
    sdfPipeline_ = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(device_, sdfPipelineLayout_, nullptr);
    sdfPipelineLayout_ = VK_NULL_HANDLE;
    vkDestroyPipeline(device_, backgroundPipeline_, nullptr);
    backgroundPipeline_ = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(device_, backgroundPipelineLayout_, nullptr);
    backgroundPipelineLayout_ = VK_NULL_HANDLE;
    vkDestroyRenderPass(device_, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;

    for (VkImageView imageView : swapchainImageViews_) {
        vkDestroyImageView(device_, imageView, nullptr);
    }
    swapchainImageViews_.clear();

    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

VulkanRenderer::QueueFamily VulkanRenderer::findGraphicsPresentQueue(VkPhysicalDevice device) const
{
    std::uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueCount, queues.data());

    for (std::uint32_t i = 0; i < queueCount; ++i) {
        VkBool32 canPresent = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &canPresent);

        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && canPresent == VK_TRUE) {
            return {.index = i, .found = true};
        }
    }

    return {};
}

VkSurfaceFormatKHR VulkanRenderer::selectSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }

    return formats.front();
}

VkPresentModeKHR VulkanRenderer::selectPresentMode(const std::vector<VkPresentModeKHR>& modes) const
{
    return std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()
        ? VK_PRESENT_MODE_MAILBOX_KHR
        : VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::selectSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const
{
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    return {
        .width = std::clamp(window_.width(), capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        .height = std::clamp(window_.height(), capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

VkShaderModule VulkanRenderer::createShaderModule(const char* filename) const
{
    const EmbeddedShader shaderCode = embeddedShader(filename);

    const VkShaderModuleCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shaderCode.size,
        .pCode = shaderCode.code,
    };

    VkShaderModule shader = VK_NULL_HANDLE;
    require(vkCreateShaderModule(device_, &createInfo, nullptr, &shader), "failed to create shader module");
    return shader;
}

} // namespace womp
