#include "womp/renderer/VulkanRenderer.h"

#include "womp/renderer/EmbeddedShaders.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

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
    float extra[4]{};
};

struct TextPushConstants {
    float rect[4]{};
    float uv[4]{};
    float color[4]{};
    float atlas[4]{};
};

enum class SdfPrimitiveKind : std::uint32_t {
    RoundedRect = 0,
    Circle = 1,
    Line = 2,
    Quad = 3,
    Triangle = 4,
};

struct GlyphBitmap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t bearingX = 0;
    std::int32_t bearingY = 0;
    float advance = 0.0f;
    std::vector<std::uint8_t> pixels;
};

struct FontMatch {
    std::string path;
    int faceIndex = 0;
};

using GlyphKey = std::tuple<std::string, int, std::uint32_t, std::uint32_t>;

void require(VkResult result, const char* message)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint32_t> decodeUtf8(const std::string& text)
{
    std::vector<std::uint32_t> codepoints;

    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::uint32_t codepoint = 0xfffd;
        std::size_t length = 1;

        if (byte < 0x80) {
            codepoint = byte;
        } else if ((byte & 0xe0) == 0xc0 && i + 1 < text.size()) {
            codepoint = byte & 0x1f;
            length = 2;
        } else if ((byte & 0xf0) == 0xe0 && i + 2 < text.size()) {
            codepoint = byte & 0x0f;
            length = 3;
        } else if ((byte & 0xf8) == 0xf0 && i + 3 < text.size()) {
            codepoint = byte & 0x07;
            length = 4;
        }

        bool valid = true;
        for (std::size_t j = 1; j < length; ++j) {
            const auto continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }

        if (!valid || (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000) || codepoint > 0x10ffff) {
            codepoint = 0xfffd;
            length = 1;
        }

        codepoints.push_back(codepoint);
        i += length;
    }

    return codepoints;
}

class FontResolver {
public:
    FontResolver()
    {
        if (FcInit() != FcTrue) {
            throw std::runtime_error("failed to initialize Fontconfig");
        }
    }

    std::optional<FontMatch> match(const std::vector<std::string>& families, std::uint32_t codepoint) const
    {
        auto pattern = std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)>(FcPatternCreate(), FcPatternDestroy);
        if (!pattern) {
            return std::nullopt;
        }

        for (const std::string& family : families) {
            FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
        }
        if (families.empty()) {
            FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif"));
        }

        auto charset = std::unique_ptr<FcCharSet, decltype(&FcCharSetDestroy)>(FcCharSetCreate(), FcCharSetDestroy);
        FcCharSetAddChar(charset.get(), codepoint);
        FcPatternAddCharSet(pattern.get(), FC_CHARSET, charset.get());
        FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
        FcDefaultSubstitute(pattern.get());

        FcResult result = FcResultNoMatch;
        auto match = std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)>(FcFontMatch(nullptr, pattern.get(), &result), FcPatternDestroy);
        if (!match || result != FcResultMatch) {
            return std::nullopt;
        }

        FcChar8* file = nullptr;
        int index = 0;
        if (FcPatternGetString(match.get(), FC_FILE, 0, &file) != FcResultMatch || file == nullptr) {
            return std::nullopt;
        }
        FcPatternGetInteger(match.get(), FC_INDEX, 0, &index);

        return FontMatch{
            .path = reinterpret_cast<const char*>(file),
            .faceIndex = index,
        };
    }
};

GlyphBitmap renderGlyph(FT_Library library, const FontMatch& font, std::uint32_t codepoint, std::uint32_t pixelSize)
{
    FT_Face rawFace = nullptr;
    if (FT_New_Face(library, font.path.c_str(), font.faceIndex, &rawFace) != 0) {
        return {};
    }

    auto face = std::unique_ptr<std::remove_pointer_t<FT_Face>, decltype(&FT_Done_Face)>(rawFace, FT_Done_Face);
    FT_Set_Pixel_Sizes(face.get(), 0, pixelSize);

    if (FT_Load_Char(face.get(), codepoint, FT_LOAD_RENDER) != 0) {
        return {};
    }

    const FT_GlyphSlot glyph = face->glyph;
    GlyphBitmap bitmap{
        .width = glyph->bitmap.width,
        .height = glyph->bitmap.rows,
        .bearingX = glyph->bitmap_left,
        .bearingY = glyph->bitmap_top,
        .advance = static_cast<float>(glyph->advance.x) / 64.0f,
    };

    bitmap.pixels.resize(bitmap.width * bitmap.height);
    for (std::uint32_t y = 0; y < bitmap.height; ++y) {
        const std::uint8_t* src = glyph->bitmap.buffer + y * glyph->bitmap.pitch;
        std::uint8_t* dst = bitmap.pixels.data() + y * bitmap.width;
        std::memcpy(dst, src, bitmap.width);
    }

    return bitmap;
}

SdfPushConstants toSdfPushConstants(const Primitive& primitive)
{
    SdfPushConstants push{
        .fill = {
            primitive.style.fill.r,
            primitive.style.fill.g,
            primitive.style.fill.b,
            primitive.style.fill.a,
        },
        .stroke = {
            primitive.style.stroke.r,
            primitive.style.stroke.g,
            primitive.style.stroke.b,
            primitive.style.stroke.a,
        },
    };

    if (const auto* roundedRect = std::get_if<RoundedRectPrimitive>(&primitive.geometry)) {
        push.rect[0] = roundedRect->x;
        push.rect[1] = roundedRect->y;
        push.rect[2] = roundedRect->width;
        push.rect[3] = roundedRect->height;
        push.params[0] = roundedRect->radius;
        push.params[2] = static_cast<float>(SdfPrimitiveKind::RoundedRect);
    } else if (const auto* circle = std::get_if<CirclePrimitive>(&primitive.geometry)) {
        push.rect[0] = circle->centerX;
        push.rect[1] = circle->centerY;
        push.rect[2] = circle->radius;
        push.rect[3] = circle->radius;
        push.params[0] = circle->radius;
        push.params[2] = static_cast<float>(SdfPrimitiveKind::Circle);
    } else if (const auto* quad = std::get_if<QuadPrimitive>(&primitive.geometry)) {
        push.rect[0] = quad->x;
        push.rect[1] = quad->y;
        push.rect[2] = quad->width;
        push.rect[3] = quad->height;
        push.params[2] = static_cast<float>(SdfPrimitiveKind::Quad);
    } else if (const auto* triangle = std::get_if<TrianglePrimitive>(&primitive.geometry)) {
        push.rect[0] = triangle->x0;
        push.rect[1] = triangle->y0;
        push.rect[2] = triangle->x1;
        push.rect[3] = triangle->y1;
        push.extra[0] = triangle->x2;
        push.extra[1] = triangle->y2;
        push.params[2] = static_cast<float>(SdfPrimitiveKind::Triangle);
    } else if (const auto* line = std::get_if<LinePrimitive>(&primitive.geometry)) {
        push.rect[0] = line->x0;
        push.rect[1] = line->y0;
        push.rect[2] = line->x1;
        push.rect[3] = line->y1;
        push.params[0] = line->thickness;
        push.params[2] = static_cast<float>(SdfPrimitiveKind::Line);
    }

    push.params[1] = primitive.style.strokeWidth;
    push.params[3] = 1.0f;
    return push;
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
    createTextDescriptorSetLayout();
    createPipelines();
    createFramebuffers();
    createCommandPool();
    rebuildTextAtlas();
    createCommandBuffers();
    createSyncObjects();
}

VulkanRenderer::~VulkanRenderer()
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        cleanupSwapchain();
        cleanupTextAtlas();

        vkDestroySemaphore(device_, renderFinished_, nullptr);
        vkDestroySemaphore(device_, imageAvailable_, nullptr);
        vkDestroyFence(device_, frameInFlight_, nullptr);
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDescriptorSetLayout(device_, textDescriptorSetLayout_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }

    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void VulkanRenderer::setPrimitives(std::vector<Primitive> primitives)
{
    primitives_ = std::move(primitives);
    if (!commandBuffers_.empty() && !framebuffers_.empty()) {
        vkDeviceWaitIdle(device_);
        rebuildTextAtlas();
        createCommandBuffers();
    }
}

void VulkanRenderer::clearPrimitives()
{
    setPrimitives({});
}

void VulkanRenderer::addPrimitive(Primitive primitive)
{
    primitives_.push_back(std::move(primitive));
    if (!commandBuffers_.empty() && !framebuffers_.empty()) {
        vkDeviceWaitIdle(device_);
        rebuildTextAtlas();
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

void VulkanRenderer::createTextDescriptorSetLayout()
{
    const VkDescriptorSetLayoutBinding atlasBinding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    const VkDescriptorSetLayoutCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &atlasBinding,
    };

    require(vkCreateDescriptorSetLayout(device_, &createInfo, nullptr, &textDescriptorSetLayout_), "failed to create text descriptor layout");
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

    const VkPushConstantRange textPushConstantRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(TextPushConstants),
    };
    const VkPipelineLayoutCreateInfo textLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &textDescriptorSetLayout_,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &textPushConstantRange,
    };
    require(vkCreatePipelineLayout(device_, &textLayoutInfo, nullptr, &textPipelineLayout_), "failed to create text pipeline layout");

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
    textPipeline_ = createPipeline("text.vert.spv", "text.frag.spv", textPipelineLayout_, alphaBlend);
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

    if (!primitives_.empty()) {
        for (const Primitive& primitive : primitives_) {
            if (!primitive.visible) {
                continue;
            }

            const auto drawSdf = [&](const Primitive& sdfPrimitive) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sdfPipeline_);
                const SdfPushConstants push = toSdfPushConstants(sdfPrimitive);
                vkCmdPushConstants(commandBuffer, sdfPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
                vkCmdDraw(commandBuffer, 6, 1, 0, 0);
            };

            const auto drawText = [&]() {
                const auto glyphs = textGlyphs_.find(primitive.id);
                if (glyphs == textGlyphs_.end() || glyphs->second.empty() || textDescriptorSet_ == VK_NULL_HANDLE) {
                    return;
                }

                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipeline_);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipelineLayout_, 0, 1, &textDescriptorSet_, 0, nullptr);
                for (const TextGlyphDraw& glyph : glyphs->second) {
                    const TextPushConstants push{
                        .rect = {glyph.rect[0], glyph.rect[1], glyph.rect[2], glyph.rect[3]},
                        .uv = {glyph.uv[0], glyph.uv[1], glyph.uv[2], glyph.uv[3]},
                        .color = {glyph.color.r, glyph.color.g, glyph.color.b, glyph.color.a},
                        .atlas = {
                            static_cast<float>(textAtlasWidth_),
                            static_cast<float>(textAtlasHeight_),
                            static_cast<float>(swapchainExtent_.width),
                            static_cast<float>(swapchainExtent_.height),
                        },
                    };
                    vkCmdPushConstants(commandBuffer, textPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
                    vkCmdDraw(commandBuffer, 6, 1, 0, 0);
                }
            };

            if (const auto* textField = std::get_if<TextFieldPrimitive>(&primitive.geometry)) {
                drawSdf(Primitive::roundedRect(
                    {
                        .x = textField->x,
                        .y = textField->y,
                        .width = textField->width,
                        .height = textField->height,
                        .radius = 4.0f,
                    },
                    primitive.style));
                drawText();

                if (textField->focused) {
                    float caretX = textField->x + textField->padding;
                    const auto caretPosition = textCaretX_.find(primitive.id);
                    if (caretPosition != textCaretX_.end()) {
                        caretX = caretPosition->second;
                    }
                    Primitive caret = Primitive::line(
                        {
                            .x0 = caretX,
                            .y0 = textField->y + textField->padding,
                            .x1 = caretX,
                            .y1 = textField->y + textField->height - textField->padding,
                            .thickness = textField->caretWidth,
                        },
                        {.fill = textField->caretColor});
                    drawSdf(caret);
                }
            } else if (std::holds_alternative<TextPrimitive>(primitive.geometry)) {
                drawText();
            } else {
                drawSdf(primitive);
            }
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

void VulkanRenderer::rebuildTextAtlas()
{
    cleanupTextAtlas();
    textGlyphs_.clear();
    textCaretX_.clear();

    FT_Library rawLibrary = nullptr;
    if (FT_Init_FreeType(&rawLibrary) != 0) {
        throw std::runtime_error("failed to initialize FreeType");
    }
    auto library = std::unique_ptr<std::remove_pointer_t<FT_Library>, decltype(&FT_Done_FreeType)>(rawLibrary, FT_Done_FreeType);
    FontResolver fonts;

    struct CachedGlyph {
        GlyphBitmap bitmap;
        std::uint32_t atlasX = 0;
        std::uint32_t atlasY = 0;
    };

    constexpr std::uint32_t atlasWidth = 1024;
    std::uint32_t atlasHeight = 64;
    std::vector<std::uint8_t> atlas(atlasWidth * atlasHeight, 0);
    std::map<GlyphKey, CachedGlyph> glyphCache;
    std::uint32_t cursorX = 1;
    std::uint32_t cursorY = 1;
    std::uint32_t rowHeight = 0;
    bool hasVisibleGlyph = false;

    const auto ensureAtlasHeight = [&](std::uint32_t requiredHeight) {
        if (requiredHeight <= atlasHeight) {
            return;
        }

        std::uint32_t newHeight = atlasHeight;
        while (newHeight < requiredHeight) {
            newHeight *= 2;
        }

        std::vector<std::uint8_t> resized(atlasWidth * newHeight, 0);
        for (std::uint32_t y = 0; y < atlasHeight; ++y) {
            std::memcpy(resized.data() + y * atlasWidth, atlas.data() + y * atlasWidth, atlasWidth);
        }
        atlas = std::move(resized);
        atlasHeight = newHeight;
    };

    const auto cacheGlyph = [&](const std::vector<std::string>& families, std::uint32_t codepoint, std::uint32_t pixelSize) -> const CachedGlyph* {
        const std::optional<FontMatch> font = fonts.match(families, codepoint);
        if (!font) {
            return nullptr;
        }

        const GlyphKey key{font->path, font->faceIndex, codepoint, pixelSize};
        const auto existing = glyphCache.find(key);
        if (existing != glyphCache.end()) {
            return &existing->second;
        }

        CachedGlyph cached{
            .bitmap = renderGlyph(library.get(), *font, codepoint, pixelSize),
        };

        if (cached.bitmap.width > 0 && cached.bitmap.height > 0) {
            if (cursorX + cached.bitmap.width + 1 > atlasWidth) {
                cursorX = 1;
                cursorY += rowHeight + 1;
                rowHeight = 0;
            }

            ensureAtlasHeight(cursorY + cached.bitmap.height + 1);
            cached.atlasX = cursorX;
            cached.atlasY = cursorY;

            for (std::uint32_t y = 0; y < cached.bitmap.height; ++y) {
                std::memcpy(
                    atlas.data() + (cached.atlasY + y) * atlasWidth + cached.atlasX,
                    cached.bitmap.pixels.data() + y * cached.bitmap.width,
                    cached.bitmap.width);
            }

            cursorX += cached.bitmap.width + 1;
            rowHeight = std::max(rowHeight, cached.bitmap.height);
            hasVisibleGlyph = true;
        }

        const auto inserted = glyphCache.emplace(key, std::move(cached));
        return &inserted.first->second;
    };

    const auto layoutText = [&](PrimitiveId id,
                                const std::string& text,
                                const std::vector<std::string>& families,
                                float x,
                                float y,
                                float fontSize,
                                Color color,
                                float maxX = std::numeric_limits<float>::max(),
                                std::optional<std::size_t> caretCodepointIndex = std::nullopt) {
        const std::uint32_t pixelSize = std::max(1u, static_cast<std::uint32_t>(fontSize + 0.5f));
        const float lineHeight = fontSize * 1.25f;
        const float originX = x;
        float penX = x;
        float baseline = y + fontSize;
        std::vector<TextGlyphDraw>& draws = textGlyphs_[id];
        std::size_t codepointIndex = 0;
        if (caretCodepointIndex && *caretCodepointIndex == 0) {
            textCaretX_[id] = penX;
        }

        for (std::uint32_t codepoint : decodeUtf8(text)) {
            if (codepoint == '\n') {
                penX = originX;
                baseline += lineHeight;
                ++codepointIndex;
                if (caretCodepointIndex && *caretCodepointIndex == codepointIndex) {
                    textCaretX_[id] = penX;
                }
                continue;
            }

            const CachedGlyph* glyph = cacheGlyph(families, codepoint, pixelSize);
            if (glyph == nullptr) {
                penX += fontSize * 0.5f;
                continue;
            }

            const GlyphBitmap& bitmap = glyph->bitmap;
            if (bitmap.width > 0 && bitmap.height > 0) {
                const float glyphX = penX + static_cast<float>(bitmap.bearingX);
                const float glyphY = baseline - static_cast<float>(bitmap.bearingY);
                if (glyphX + static_cast<float>(bitmap.width) <= maxX) {
                    draws.push_back({
                        .rect = {glyphX, glyphY, static_cast<float>(bitmap.width), static_cast<float>(bitmap.height)},
                        .uv = {
                            static_cast<float>(glyph->atlasX) / static_cast<float>(atlasWidth),
                            static_cast<float>(glyph->atlasY) / static_cast<float>(atlasHeight),
                            static_cast<float>(bitmap.width) / static_cast<float>(atlasWidth),
                            static_cast<float>(bitmap.height) / static_cast<float>(atlasHeight),
                        },
                        .color = color,
                    });
                }
            }

            penX += bitmap.advance > 0.0f ? bitmap.advance : fontSize * 0.5f;
            ++codepointIndex;
            if (caretCodepointIndex && *caretCodepointIndex == codepointIndex) {
                textCaretX_[id] = penX;
            }
            if (penX > maxX) {
                break;
            }
        }

        if (caretCodepointIndex && textCaretX_.find(id) == textCaretX_.end()) {
            textCaretX_[id] = penX;
        }
    };

    for (const Primitive& primitive : primitives_) {
        if (!primitive.visible) {
            continue;
        }

        if (const auto* text = std::get_if<TextPrimitive>(&primitive.geometry)) {
            layoutText(primitive.id, text->text, text->fontFamilies, text->x, text->y, text->fontSize, primitive.style.fill);
        } else if (const auto* textField = std::get_if<TextFieldPrimitive>(&primitive.geometry)) {
            const bool showingPlaceholder = textField->text.empty() && !textField->placeholder.empty();
            layoutText(
                primitive.id,
                showingPlaceholder ? textField->placeholder : textField->text,
                textField->fontFamilies,
                textField->x + textField->padding,
                textField->y + textField->padding,
                textField->fontSize,
                showingPlaceholder ? textField->placeholderColor : textField->textColor,
                textField->x + textField->width - textField->padding,
                showingPlaceholder ? std::nullopt : std::optional<std::size_t>{textField->caretCodepointIndex});
        }
    }

    if (!hasVisibleGlyph) {
        return;
    }

    textAtlasWidth_ = atlasWidth;
    textAtlasHeight_ = atlasHeight;

    const VkDeviceSize uploadSize = atlas.size();
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;

    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = uploadSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    require(vkCreateBuffer(device_, &bufferInfo, nullptr, &stagingBuffer), "failed to create text staging buffer");

    VkMemoryRequirements bufferRequirements{};
    vkGetBufferMemoryRequirements(device_, stagingBuffer, &bufferRequirements);
    const VkMemoryAllocateInfo bufferAlloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = bufferRequirements.size,
        .memoryTypeIndex = findMemoryType(bufferRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    require(vkAllocateMemory(device_, &bufferAlloc, nullptr, &stagingMemory), "failed to allocate text staging memory");
    vkBindBufferMemory(device_, stagingBuffer, stagingMemory, 0);

    void* mapped = nullptr;
    vkMapMemory(device_, stagingMemory, 0, uploadSize, 0, &mapped);
    std::memcpy(mapped, atlas.data(), uploadSize);
    vkUnmapMemory(device_, stagingMemory);

    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = {textAtlasWidth_, textAtlasHeight_, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    require(vkCreateImage(device_, &imageInfo, nullptr, &textAtlasImage_), "failed to create text atlas image");

    VkMemoryRequirements imageRequirements{};
    vkGetImageMemoryRequirements(device_, textAtlasImage_, &imageRequirements);
    const VkMemoryAllocateInfo imageAlloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = imageRequirements.size,
        .memoryTypeIndex = findMemoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    require(vkAllocateMemory(device_, &imageAlloc, nullptr, &textAtlasMemory_), "failed to allocate text atlas memory");
    vkBindImageMemory(device_, textAtlasImage_, textAtlasMemory_, 0);

    const VkCommandBufferAllocateInfo commandAlloc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    require(vkAllocateCommandBuffers(device_, &commandAlloc, &commandBuffer), "failed to allocate text upload command buffer");

    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    require(vkBeginCommandBuffer(commandBuffer, &beginInfo), "failed to begin text upload command buffer");

    const VkImageMemoryBarrier toTransfer{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = textAtlasImage_,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    const VkBufferImageCopy copyRegion{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {textAtlasWidth_, textAtlasHeight_, 1},
    };
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, textAtlasImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    const VkImageMemoryBarrier toShaderRead{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = textAtlasImage_,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

    require(vkEndCommandBuffer(commandBuffer), "failed to record text upload command buffer");

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };
    require(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE), "failed to submit text upload");
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);

    const VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = textAtlasImage_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    require(vkCreateImageView(device_, &viewInfo, nullptr, &textAtlasView_), "failed to create text atlas view");

    const VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
    };
    require(vkCreateSampler(device_, &samplerInfo, nullptr, &textAtlasSampler_), "failed to create text atlas sampler");

    const VkDescriptorPoolSize poolSize{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    require(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &textDescriptorPool_), "failed to create text descriptor pool");

    const VkDescriptorSetAllocateInfo descriptorAlloc{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = textDescriptorPool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &textDescriptorSetLayout_,
    };
    require(vkAllocateDescriptorSets(device_, &descriptorAlloc, &textDescriptorSet_), "failed to allocate text descriptor set");

    const VkDescriptorImageInfo descriptorImage{
        .sampler = textAtlasSampler_,
        .imageView = textAtlasView_,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet descriptorWrite{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = textDescriptorSet_,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &descriptorImage,
    };
    vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);
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

    vkDestroyPipeline(device_, textPipeline_, nullptr);
    textPipeline_ = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(device_, textPipelineLayout_, nullptr);
    textPipelineLayout_ = VK_NULL_HANDLE;
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

void VulkanRenderer::cleanupTextAtlas()
{
    if (textDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, textDescriptorPool_, nullptr);
        textDescriptorPool_ = VK_NULL_HANDLE;
        textDescriptorSet_ = VK_NULL_HANDLE;
    }
    if (textAtlasSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, textAtlasSampler_, nullptr);
        textAtlasSampler_ = VK_NULL_HANDLE;
    }
    if (textAtlasView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, textAtlasView_, nullptr);
        textAtlasView_ = VK_NULL_HANDLE;
    }
    if (textAtlasImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, textAtlasImage_, nullptr);
        textAtlasImage_ = VK_NULL_HANDLE;
    }
    if (textAtlasMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, textAtlasMemory_, nullptr);
        textAtlasMemory_ = VK_NULL_HANDLE;
    }

    textAtlasWidth_ = 0;
    textAtlasHeight_ = 0;
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

std::uint32_t VulkanRenderer::findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool matchesType = (typeFilter & (1u << i)) != 0;
        const bool matchesProperties = (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (matchesType && matchesProperties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable Vulkan memory type");
}

} // namespace womp
