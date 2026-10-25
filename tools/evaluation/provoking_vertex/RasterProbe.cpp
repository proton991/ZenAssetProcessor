#include "RasterProbe.h"
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <type_traits>

namespace zenasset::evaluation
{
namespace
{
constexpr VkFormat     TargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkDeviceSize OutputBytes  = VkDeviceSize(RasterProbe::Width) * RasterProbe::Height * 4;

Status Checked(VkResult result, const char* operation)
{
    Status status;
    if (result != VK_SUCCESS)
    {
        status = {StatusCode::IOError, std::string(operation) + " failed with VkResult " + std::to_string(result)};
    }
    return status;
}

struct Buffer
{
    VkDevice       device{};
    VkBuffer       handle{};
    VkDeviceMemory memory{};
    void*          mapped{};
    bool           coherent{false};

    Buffer()                         = default;
    Buffer(const Buffer&)            = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer()
    {
        if (device != VK_NULL_HANDLE)
        {
            if (mapped != nullptr)
            {
                vkUnmapMemory(device, memory);
            }
            vkDestroyBuffer(device, handle, nullptr);
            vkFreeMemory(device, memory, nullptr);
        }
    }

    Status Synchronize(bool read)
    {
        Status status;
        if (!coherent)
        {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = memory;
            range.size   = VK_WHOLE_SIZE;
            status       = read ? Checked(vkInvalidateMappedMemoryRanges(device, 1, &range), "invalidate readback memory")
                                : Checked(vkFlushMappedMemoryRanges(device, 1, &range), "flush upload memory");
        }
        return status;
    }
};
} // namespace

struct RasterProbe::Impl
{
    VkInstance                       instance{};
    VkDebugUtilsMessengerEXT         messenger{};
    VkPhysicalDevice                 physical{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice                         device{};
    VkQueue                          queue{};
    uint32_t                         family{UINT32_MAX};
    VkCommandPool                    commandPool{};
    VkCommandBuffer                  command{};
    VkFence                          fence{};
    VkImage                          image{};
    VkDeviceMemory                   imageMemory{};
    VkImageView                      imageView{};
    VkRenderPass                     renderPass{};
    VkFramebuffer                    framebuffer{};
    VkPipelineLayout                 pipelineLayout{};
    VkPipeline                       pipelines[2]{};
    std::string                      deviceName;
    std::atomic<uint32_t>            validationErrors{0};
    bool                             lastSupported{false};
    bool                             ready{false};

    ~Impl()
    {
        if (device != VK_NULL_HANDLE)
        {
            const VkResult idle = vkDeviceWaitIdle(device);
            if (idle != VK_SUCCESS)
            {
                std::fprintf(stderr, "Vulkan cleanup wait failed: %d\n", idle);
            }
            for (VkPipeline pipeline : pipelines)
            {
                vkDestroyPipeline(device, pipeline, nullptr);
            }
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            vkDestroyFramebuffer(device, framebuffer, nullptr);
            vkDestroyRenderPass(device, renderPass, nullptr);
            vkDestroyImageView(device, imageView, nullptr);
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, imageMemory, nullptr);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, commandPool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger != VK_NULL_HANDLE)
        {
            const PFN_vkDestroyDebugUtilsMessengerEXT destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy != nullptr)
            {
                destroy(instance, messenger, nullptr);
            }
        }
        if (instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance, nullptr);
        }
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*                                       user)
    {
        Impl& self = *static_cast<Impl*>(user);
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
        {
            ++self.validationErrors;
        }
        std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
        return VK_FALSE;
    }

    Status CreateInstance()
    {
        const char* layer     = "VK_LAYER_KHRONOS_validation";
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        uint32_t    count     = 0;
        Status      status    = Checked(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate validation layers");
        std::vector<VkLayerProperties> layers(count);
        if (status.Succeeded() && count != 0)
        {
            status = Checked(vkEnumerateInstanceLayerProperties(&count, layers.data()), "read validation layers");
        }
        bool available = false;
        for (const VkLayerProperties& properties : layers)
        {
            available = available || std::strcmp(properties.layerName, layer) == 0;
        }
        if (status.Succeeded() && !available)
        {
            status = {StatusCode::UnsupportedFeature, "VK_LAYER_KHRONOS_validation is required"};
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "ZenAsset provoking vertex acceptance";
        app.apiVersion       = VK_API_VERSION_1_1;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                              | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = Validation;
        debug.pUserData       = this;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pNext                   = &debug;
        create.pApplicationInfo        = &app;
        create.enabledLayerCount       = 1;
        create.ppEnabledLayerNames     = &layer;
        create.enabledExtensionCount   = 1;
        create.ppEnabledExtensionNames = &extension;
        if (status.Succeeded())
        {
            status = Checked(vkCreateInstance(&create, nullptr, &instance), "create instance");
        }
        if (status.Succeeded())
        {
            const PFN_vkCreateDebugUtilsMessengerEXT make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            status = make == nullptr ? Status{StatusCode::UnsupportedFeature, "Vulkan debug messenger is unavailable"}
                                     : Checked(make(instance, &debug, nullptr, &messenger), "create validation messenger");
        }
        return status;
    }

    Status SelectDevice()
    {
        uint32_t count  = 0;
        Status   status = Checked(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        if (status.Succeeded() && count != 0)
        {
            status = Checked(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "read devices");
        }
        const char* requested  = std::getenv("ZEN_VULKAN_DEVICE");
        const bool  hasRequest = requested != nullptr && requested[0] != '\0';
        if (status.Succeeded())
        {
            for (VkPhysicalDevice candidate : devices)
            {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate, &properties);
                if (physical == VK_NULL_HANDLE && properties.apiVersion >= VK_API_VERSION_1_1
                    && (!hasRequest || std::strstr(properties.deviceName, requested) != nullptr))
                {
                    uint32_t queueCount = 0;
                    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
                    std::vector<VkQueueFamilyProperties> queues(queueCount);
                    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
                    for (uint32_t index = 0; index < queueCount; ++index)
                    {
                        if (physical == VK_NULL_HANDLE && queues[index].queueCount != 0
                            && (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
                        {
                            physical   = candidate;
                            family     = index;
                            deviceName = properties.deviceName;
                        }
                    }
                }
            }
            if (physical == VK_NULL_HANDLE)
            {
                status = {StatusCode::UnsupportedFeature, std::string("No Vulkan 1.1 graphics device matches ")
                                                              + (hasRequest ? requested : "the requirements")};
            }
        }
        if (status.Succeeded())
        {
            vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, TargetFormat, &properties);
            const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            if ((properties.optimalTilingFeatures & required) != required)
            {
                status = {StatusCode::UnsupportedFeature, "RGBA8 color attachment and transfer source support is required"};
            }
        }
        return status;
    }

    Status CreateDevice()
    {
        uint32_t count = 0;
        Status   status =
            Checked(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "enumerate device extensions");
        std::vector<VkExtensionProperties> extensions(count);
        if (status.Succeeded() && count != 0)
        {
            status = Checked(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()),
                             "read device extensions");
        }
        bool hasProvoking = false;
        for (const VkExtensionProperties& extension : extensions)
        {
            hasProvoking = hasProvoking || std::strcmp(extension.extensionName, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME) == 0;
        }
        VkPhysicalDeviceProvokingVertexFeaturesEXT provoking{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
        if (status.Succeeded() && hasProvoking)
        {
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &provoking;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            lastSupported = provoking.provokingVertexLast == VK_TRUE;
        }
        provoking.provokingVertexLast                       = lastSupported ? VK_TRUE : VK_FALSE;
        provoking.transformFeedbackPreservesProvokingVertex = VK_FALSE;
        const char*             extension                   = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
        const float             priority                    = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount       = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos    = &queueInfo;
        if (lastSupported)
        {
            create.pNext                   = &provoking;
            create.enabledExtensionCount   = 1;
            create.ppEnabledExtensionNames = &extension;
        }
        if (status.Succeeded())
        {
            status = Checked(vkCreateDevice(physical, &create, nullptr, &device), "create device");
        }
        if (status.Succeeded())
        {
            vkGetDeviceQueue(device, family, 0, &queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = family;
            status                = Checked(vkCreateCommandPool(device, &pool, nullptr, &commandPool), "create command pool");
        }
        if (status.Succeeded())
        {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool        = commandPool;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            status = Checked(vkAllocateCommandBuffers(device, &allocate, &command), "allocate command buffer");
        }
        if (status.Succeeded())
        {
            VkFenceCreateInfo createFence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            status = Checked(vkCreateFence(device, &createFence, nullptr, &fence), "create submission fence");
        }
        return status;
    }

    Status Allocate(const VkMemoryRequirements& requirements,
                    VkMemoryPropertyFlags       required,
                    VkMemoryPropertyFlags       preferred,
                    VkDeviceMemory&             memory,
                    bool&                       coherent)
    {
        uint32_t selected = UINT32_MAX;
        for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
        {
            const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[index].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << index)) != 0 && (flags & required) == required
                && (selected == UINT32_MAX || (flags & preferred) == preferred))
            {
                selected = index;
            }
        }
        Status status;
        if (selected == UINT32_MAX)
        {
            status = {StatusCode::UnsupportedFeature, "No compatible Vulkan memory type"};
        }
        else
        {
            coherent = (memoryProperties.memoryTypes[selected].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize  = requirements.size;
            allocate.memoryTypeIndex = selected;
            status                   = Checked(vkAllocateMemory(device, &allocate, nullptr, &memory), "allocate memory");
        }
        return status;
    }

    Status MakeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, Buffer& buffer)
    {
        buffer.device = device;
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size        = bytes;
        create.usage       = usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Status status      = Checked(vkCreateBuffer(device, &create, nullptr, &buffer.handle), "create buffer");
        if (status.Succeeded())
        {
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
            status = Allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              buffer.memory, buffer.coherent);
        }
        if (status.Succeeded())
        {
            status = Checked(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "bind buffer memory");
        }
        if (status.Succeeded())
        {
            status = Checked(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped), "map buffer memory");
        }
        return status;
    }

    Status CreateTarget()
    {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType     = VK_IMAGE_TYPE_2D;
        create.format        = TargetFormat;
        create.extent        = {Width, Height, 1};
        create.mipLevels     = 1;
        create.arrayLayers   = 1;
        create.samples       = VK_SAMPLE_COUNT_1_BIT;
        create.tiling        = VK_IMAGE_TILING_OPTIMAL;
        create.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Status status        = Checked(vkCreateImage(device, &create, nullptr, &image), "create target image");
        if (status.Succeeded())
        {
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, image, &requirements);
            bool coherent = false;
            status        = Allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, imageMemory, coherent);
        }
        if (status.Succeeded())
        {
            status = Checked(vkBindImageMemory(device, image, imageMemory, 0), "bind target memory");
        }
        if (status.Succeeded())
        {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image            = image;
            view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view.format           = TargetFormat;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            status                = Checked(vkCreateImageView(device, &view, nullptr, &imageView), "create target view");
        }
        if (status.Succeeded())
        {
            VkAttachmentDescription attachment{};
            attachment.format         = TargetFormat;
            attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // Each call waits for completion; discard the previous frame on the next pass.
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription  subpass{};
            subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments    = &color;
            VkSubpassDependency dependencies[2]{};
            dependencies[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
            dependencies[0].dstSubpass    = 0;
            dependencies[0].srcStageMask  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dependencies[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1].srcSubpass    = 0;
            dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
            dependencies[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[1].dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            pass.attachmentCount = 1;
            pass.pAttachments    = &attachment;
            pass.subpassCount    = 1;
            pass.pSubpasses      = &subpass;
            pass.dependencyCount = 2;
            pass.pDependencies   = dependencies;
            status               = Checked(vkCreateRenderPass(device, &pass, nullptr, &renderPass), "create render pass");
        }
        if (status.Succeeded())
        {
            VkFramebufferCreateInfo createFramebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            createFramebuffer.renderPass      = renderPass;
            createFramebuffer.attachmentCount = 1;
            createFramebuffer.pAttachments    = &imageView;
            createFramebuffer.width           = Width;
            createFramebuffer.height          = Height;
            createFramebuffer.layers          = 1;
            status = Checked(vkCreateFramebuffer(device, &createFramebuffer, nullptr, &framebuffer), "create framebuffer");
        }
        return status;
    }

    Status ReadShader(const std::string& path, VkShaderModule& module)
    {
        Status         status;
        std::ifstream  input(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
        std::streamoff size = -1;
        if (input)
        {
            size = input.tellg();
        }
        if (size <= 0 || size > 1024 * 1024 || size % sizeof(uint32_t) != 0)
        {
            status = {StatusCode::InvalidData, "Cannot read a valid SPIR-V shader: " + path};
        }
        else
        {
            std::vector<uint32_t> words(static_cast<size_t>(size) / sizeof(uint32_t));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char*>(words.data()), size))
            {
                status = {StatusCode::IOError, "Cannot read SPIR-V shader: " + path};
            }
            else
            {
                VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                create.codeSize = static_cast<size_t>(size);
                create.pCode    = words.data();
                status          = Checked(vkCreateShaderModule(device, &create, nullptr, &module), "create shader module");
            }
        }
        return status;
    }

    Status CreatePipelines(const std::string& vertexPath, const std::string& fragmentPath)
    {
        static_assert(std::is_standard_layout_v<Vertex>);
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        Status status = Checked(vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout), "create pipeline layout");
        VkShaderModule vertex{};
        VkShaderModule fragment{};
        if (status.Succeeded())
        {
            status = ReadShader(vertexPath, vertex);
        }
        if (status.Succeeded())
        {
            status = ReadShader(fragmentPath, fragment);
        }
        for (uint32_t mode = 0; mode < (lastSupported ? 2u : 1u) && status.Succeeded(); ++mode)
        {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage                   = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module                  = vertex;
            stages[1].stage                   = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module                  = fragment;
            stages[0].pName = stages[1].pName = "main";
            const VkVertexInputBindingDescription   binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            const VkVertexInputAttributeDescription attributes[2] = {
                {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, pos)},
                {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}};
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            input.vertexBindingDescriptionCount   = 1;
            input.pVertexBindingDescriptions      = &binding;
            input.vertexAttributeDescriptionCount = 2;
            input.pVertexAttributeDescriptions    = attributes;
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            const VkViewport                  viewport{0, 0, float(Width), float(Height), 0, 1};
            const VkRect2D                    scissor{{0, 0}, {Width, Height}};
            VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewportState.viewportCount = 1;
            viewportState.pViewports    = &viewport;
            viewportState.scissorCount  = 1;
            viewportState.pScissors     = &scissor;
            VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking{
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
            provoking.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.pNext       = mode == 1 ? &provoking : nullptr;
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode    = VK_CULL_MODE_NONE;
            raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth   = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState color{};
            color.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1;
            blend.pAttachments    = &color;
            VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            create.stageCount          = 2;
            create.pStages             = stages;
            create.pVertexInputState   = &input;
            create.pInputAssemblyState = &assembly;
            create.pViewportState      = &viewportState;
            create.pRasterizationState = &raster;
            create.pMultisampleState   = &multisample;
            create.pColorBlendState    = &blend;
            create.layout              = pipelineLayout;
            create.renderPass          = renderPass;
            status = Checked(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipelines[mode]),
                             "create graphics pipeline");
        }
        vkDestroyShaderModule(device, fragment, nullptr);
        vkDestroyShaderModule(device, vertex, nullptr);
        return status;
    }

    Status Record(const Buffer& vertices, const Buffer& indices, const Buffer& output, uint32_t count, bool lastVertex)
    {
        Status status = Checked(vkResetCommandPool(device, commandPool, 0), "reset command pool");
        if (status.Succeeded())
        {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            status      = Checked(vkBeginCommandBuffer(command, &begin), "begin command buffer");
        }
        if (status.Succeeded())
        {
            VkClearValue clear{};
            clear.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            pass.renderPass        = renderPass;
            pass.framebuffer       = framebuffer;
            pass.renderArea.extent = {Width, Height};
            pass.clearValueCount   = 1;
            pass.pClearValues      = &clear;
            vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[lastVertex ? 1 : 0]);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
            vkCmdBindIndexBuffer(command, indices.handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command, count, 1, 0, 0, 0);
            vkCmdEndRenderPass(command);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent      = {Width, Height, 1};
            vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.handle, 1, &copy);
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer                                            = output.handle;
            barrier.size                                              = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &barrier, 0, nullptr);
            status = Checked(vkEndCommandBuffer(command), "end command buffer");
        }
        return status;
    }

    Status Render(std::span<const Vertex>   vertices,
                  std::span<const uint32_t> indices,
                  bool                      lastVertex,
                  std::vector<uint8_t>&     rgba)
    {
        rgba.clear();
        Status status;
        if (!ready)
        {
            status = {StatusCode::InvalidArgument, "Raster probe is not initialized"};
        }
        else if (lastVertex && !lastSupported)
        {
            status = {StatusCode::UnsupportedFeature, "Last provoking vertex is unsupported on this device"};
        }
        else if (vertices.empty() || indices.empty() || indices.size() % 3 != 0 || indices.size() > UINT32_MAX
                 || vertices.size() > std::numeric_limits<size_t>::max() / sizeof(Vertex)
                 || indices.size() > std::numeric_limits<size_t>::max() / sizeof(uint32_t))
        {
            status = {StatusCode::InvalidArgument, "A valid indexed triangle list is required"};
        }
        for (size_t index = 0; index < indices.size() && status.Succeeded(); ++index)
        {
            if (indices[index] >= vertices.size())
            {
                status = {StatusCode::InvalidArgument, "Triangle index exceeds the vertex count"};
            }
        }
        Buffer         vertexBuffer;
        Buffer         indexBuffer;
        Buffer         output;
        const uint32_t errorsBefore = validationErrors.load();
        if (status.Succeeded())
        {
            status = MakeBuffer(vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer);
        }
        if (status.Succeeded())
        {
            status = MakeBuffer(indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer);
        }
        if (status.Succeeded())
        {
            status = MakeBuffer(OutputBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, output);
        }
        if (status.Succeeded())
        {
            std::memcpy(vertexBuffer.mapped, vertices.data(), vertices.size_bytes());
            std::memcpy(indexBuffer.mapped, indices.data(), indices.size_bytes());
            status = vertexBuffer.Synchronize(false);
        }
        if (status.Succeeded())
        {
            status = indexBuffer.Synchronize(false);
        }
        if (status.Succeeded())
        {
            status = Record(vertexBuffer, indexBuffer, output, static_cast<uint32_t>(indices.size()), lastVertex);
        }
        if (status.Succeeded())
        {
            status = Checked(vkResetFences(device, 1, &fence), "reset submission fence");
        }
        if (status.Succeeded())
        {
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers    = &command;
            status                    = Checked(vkQueueSubmit(queue, 1, &submit, fence), "submit render");
            if (status.Succeeded())
            {
                status = Checked(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for render");
                if (!status.Succeeded())
                {
                    const VkResult idle = vkDeviceWaitIdle(device);
                    if (idle != VK_SUCCESS)
                    {
                        std::fprintf(stderr, "Vulkan recovery wait failed: %d\n", idle);
                    }
                }
            }
            if (!status.Succeeded())
            {
                ready = false;
            }
        }
        if (status.Succeeded())
        {
            status = output.Synchronize(true);
        }
        if (status.Succeeded() && validationErrors.load() != errorsBefore)
        {
            status = {StatusCode::InvalidData, "Vulkan validation reported a rendering error"};
        }
        if (status.Succeeded())
        {
            rgba.resize(static_cast<size_t>(OutputBytes));
            std::memcpy(rgba.data(), output.mapped, rgba.size());
        }
        return status;
    }
};

RasterProbe::RasterProbe() : m_impl(std::make_unique<Impl>()) {}
RasterProbe::~RasterProbe() = default;

Status RasterProbe::Initialize(const std::string& vertexShader, const std::string& fragmentShader)
{
    Status status;
    if (m_impl->instance != VK_NULL_HANDLE)
    {
        status = {StatusCode::InvalidArgument, "Raster probe initialization was already attempted"};
    }
    else
    {
        status = m_impl->CreateInstance();
    }
    if (status.Succeeded())
    {
        status = m_impl->SelectDevice();
    }
    if (status.Succeeded())
    {
        status = m_impl->CreateDevice();
    }
    if (status.Succeeded())
    {
        status = m_impl->CreateTarget();
    }
    if (status.Succeeded())
    {
        status = m_impl->CreatePipelines(vertexShader, fragmentShader);
    }
    if (status.Succeeded() && m_impl->validationErrors.load() != 0)
    {
        status = {StatusCode::InvalidData, "Vulkan validation reported an initialization error"};
    }
    m_impl->ready = status.Succeeded();
    return status;
}

Status RasterProbe::Render(std::span<const Vertex>   vertices,
                           std::span<const uint32_t> indices,
                           bool                      lastVertex,
                           std::vector<uint8_t>&     rgba)
{
    return m_impl->Render(vertices, indices, lastVertex, rgba);
}

bool RasterProbe::SupportsLastVertex() const
{
    return m_impl->lastSupported;
}

const std::string& RasterProbe::DeviceName() const
{
    return m_impl->deviceName;
}

uint32_t RasterProbe::ValidationErrorCount() const
{
    return m_impl->validationErrors.load();
}
} // namespace zenasset::evaluation
