#include "Processing/AtlasPacking.h"
#include "Processing/ArrayPacking.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <memory>

namespace
{
using namespace zenasset;
using namespace zenasset::detail;
constexpr uint32_t    Side        = 129;
constexpr uint64_t    OutputBytes = uint64_t(Side) * Side * 20 * sizeof(float);
std::atomic<uint32_t> validationErrors{0};

VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                          VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT* data,
                                          void*)
{
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
        ++validationErrors;
    }
    std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    return VK_FALSE;
}

struct Buffer
{
    VkDevice       device{};
    VkBuffer       handle{};
    VkDeviceMemory memory{};
    void*          mapped{};
    ~Buffer()
    {
        if (mapped != nullptr)
        {
            vkUnmapMemory(device, memory);
        }
        vkDestroyBuffer(device, handle, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};

struct Image
{
    VkDevice       device{};
    VkImage        handle{};
    VkDeviceMemory memory{};
    VkImageView    view{};
    VkImageView    arrayView{};
    uint64_t       allocationBytes{0};
    ~Image()
    {
        vkDestroyImageView(device, arrayView, nullptr);
        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, handle, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};

struct Parameters
{
    float low[4]{};
    float high[4]{};
    float query[4]{};
    float gradients[4]{};
    float settings[4]{};
    float sampling[4]{};
};
static_assert(sizeof(Parameters) == 96);

class Device
{
public:
    ~Device()
    {
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
            m_images.clear();
            m_buffers.clear();
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
            vkDestroySampler(m_device, m_sampler, nullptr);
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            vkDestroyDevice(m_device, nullptr);
        }
        if (m_messenger != VK_NULL_HANDLE)
        {
            const PFN_vkDestroyDebugUtilsMessengerEXT destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(m_instance, m_messenger, nullptr);
        }
        if (m_instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(m_instance, nullptr);
        }
    }

    bool Init(const char* vertexPath, const char* fragmentPath, bool anisotropy)
    {
        const char*       layer     = "VK_LAYER_KHRONOS_validation";
        const char*       extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "ZenAsset atlas sampling evaluation";
        app.apiVersion       = VK_API_VERSION_1_1;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        debug.pfnUserCallback = Validation;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo  = &app;
        create.enabledLayerCount = create.enabledExtensionCount = 1;
        create.ppEnabledLayerNames                              = &layer;
        create.ppEnabledExtensionNames                          = &extension;
        create.pNext                                            = &debug;
        Call(vkCreateInstance(&create, nullptr, &m_instance), "create instance");
        if (m_ok)
        {
            const PFN_vkCreateDebugUtilsMessengerEXT make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
            Call(make(m_instance, &debug, nullptr, &m_messenger), "create validation messenger");
        }
        uint32_t deviceCount = 0;
        if (m_ok)
        {
            Call(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr), "enumerate devices");
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        if (m_ok && deviceCount != 0)
        {
            Call(vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data()), "read devices");
        }
        uint32_t    family       = UINT32_MAX;
        const char* preferred    = std::getenv("ZEN_VULKAN_DEVICE");
        const bool  hasPreferred = preferred != nullptr && preferred[0] != '\0';
        for (VkPhysicalDevice physical : devices)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical, &properties);
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
            for (uint32_t index = 0; index < count; ++index)
            {
                if (m_physical == VK_NULL_HANDLE && (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0
                    && (hasPreferred ? std::strstr(properties.deviceName, preferred) != nullptr
                                     : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
                {
                    m_physical   = physical;
                    m_properties = properties;
                    family       = index;
                }
            }
        }
        if (m_physical == VK_NULL_HANDLE)
        {
            Fail("No requested Vulkan graphics device available");
        }
        if (m_ok)
        {
            vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memory);
            const float             priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount       = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos    = &queue;
            VkPhysicalDeviceFeatures available{};
            vkGetPhysicalDeviceFeatures(m_physical, &available);
            VkPhysicalDeviceFeatures features{};
            features.fragmentStoresAndAtomics = VK_TRUE;
            features.samplerAnisotropy        = anisotropy ? VK_TRUE : VK_FALSE;
            features.shaderFloat64            = ZENASSET_PROBE_FLOAT64 ? VK_TRUE : VK_FALSE;
            device.pEnabledFeatures           = &features;
            if (available.fragmentStoresAndAtomics == VK_FALSE
                || (ZENASSET_PROBE_FLOAT64 && available.shaderFloat64 == VK_FALSE)
                || (anisotropy && available.samplerAnisotropy == VK_FALSE))
            {
                Fail("Requested fragment storage, diagnostic float64, or anisotropic filtering is unavailable");
            }
            if (m_ok)
            {
                Call(vkCreateDevice(m_physical, &device, nullptr, &m_device), "create device");
            }
        }
        if (m_ok)
        {
            vkGetDeviceQueue(m_device, family, 0, &m_queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = family;
            Call(vkCreateCommandPool(m_device, &pool, nullptr, &m_commandPool), "create command pool");
        }
        if (m_ok)
        {
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool        = m_commandPool;
            allocation.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            Call(vkAllocateCommandBuffers(m_device, &allocation, &m_commands), "allocate commands");
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
            sampler.mipmapMode                    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.maxLod                                                     = VK_LOD_CLAMP_NONE;
            sampler.anisotropyEnable                                           = anisotropy ? VK_TRUE : VK_FALSE;
            sampler.maxAnisotropy = anisotropy ? std::min(16.0f, m_properties.limits.maxSamplerAnisotropy) : 1.0f;
            Call(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "create sampler");
        }
        if (m_ok)
        {
            InitPipeline(vertexPath, fragmentPath);
        }
        if (m_ok)
        {
            std::printf(
                "{\"kind\":\"device\",\"name\":\"%s\",\"vendor\":%u,\"device\":%u,\"driver\":%u,"
                "\"mipmap_precision_bits\":%u,\"subtexel_precision_bits\":%u,\"shader_float64_enabled\":%s,\"sampler_anisotropy_enabled\":%s,\"sampler_max_anisotropy\":%.9g}\n",
                m_properties.deviceName, m_properties.vendorID, m_properties.deviceID, m_properties.driverVersion,
                m_properties.limits.mipmapPrecisionBits, m_properties.limits.subTexelPrecisionBits,
                ZENASSET_PROBE_FLOAT64 ? "true" : "false", anisotropy ? "true" : "false",
                anisotropy ? std::min(16.0f, m_properties.limits.maxSamplerAnisotropy) : 1.0f);
        }
        return m_ok;
    }

    uint32_t MipPrecision() const
    {
        return m_properties.limits.mipmapPrecisionBits;
    }

    uint32_t SubtexelPrecision() const
    {
        return m_properties.limits.subTexelPrecisionBits;
    }

    bool Good() const
    {
        return m_ok;
    }

    bool Begin()
    {
        if (m_ok)
        {
            Call(vkResetCommandPool(m_device, m_commandPool, 0), "reset commands");
        }
        if (m_ok)
        {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            Call(vkBeginCommandBuffer(m_commands, &begin), "begin commands");
        }
        return m_ok;
    }

    bool Submit()
    {
        if (m_ok)
        {
            Call(vkEndCommandBuffer(m_commands), "end commands");
        }
        if (m_ok)
        {
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers    = &m_commands;
            Call(vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE), "submit commands");
        }
        if (m_ok)
        {
            Call(vkQueueWaitIdle(m_queue), "wait for probe");
        }
        return m_ok;
    }

    Buffer* MakeBuffer(uint64_t bytes, VkBufferUsageFlags usage)
    {
        Buffer* buffer = nullptr;
        if (m_ok)
        {
            m_buffers.push_back(std::make_unique<Buffer>());
            buffer         = m_buffers.back().get();
            buffer->device = m_device;
            VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            create.size  = bytes;
            create.usage = usage;
            Call(vkCreateBuffer(m_device, &create, nullptr, &buffer->handle), "create buffer");
        }
        if (m_ok)
        {
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(m_device, buffer->handle, &requirements);
            Allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer->memory);
        }
        if (m_ok)
        {
            Call(vkBindBufferMemory(m_device, buffer->handle, buffer->memory, 0), "bind buffer");
        }
        if (m_ok)
        {
            Call(vkMapMemory(m_device, buffer->memory, 0, bytes, 0, &buffer->mapped), "map coherent buffer");
        }
        return m_ok ? buffer : nullptr;
    }

    Image* Upload(const ImagePixels& pixels)
    {
        const uint32_t layerCount = pixels.arrayLayers;
        Image*         image      = nullptr;
        const uint32_t count      = static_cast<uint32_t>(std::max(size_t(1), pixels.mips.size()));
        const VkFormat format = pixels.format == PixelFormat::RGBA8Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physical, format, &properties);
        const VkFormatFeatureFlags needed =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & needed) != needed)
        {
            Fail("Device cannot linearly sample the probe format");
        }
        if (m_ok)
        {
            m_images.push_back(std::make_unique<Image>());
            image         = m_images.back().get();
            image->device = m_device;
            VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            create.imageType   = VK_IMAGE_TYPE_2D;
            create.format      = format;
            create.extent      = {pixels.width, pixels.height, 1};
            create.mipLevels   = count;
            create.arrayLayers = layerCount;
            create.samples     = VK_SAMPLE_COUNT_1_BIT;
            create.tiling      = VK_IMAGE_TILING_OPTIMAL;
            create.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            Call(vkCreateImage(m_device, &create, nullptr, &image->handle), "create sampled image");
        }
        if (m_ok)
        {
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(m_device, image->handle, &requirements);
            image->allocationBytes = requirements.size;
            Allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image->memory);
        }
        if (m_ok)
        {
            Call(vkBindImageMemory(m_device, image->handle, image->memory, 0), "bind image");
        }
        if (m_ok)
        {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image            = image->handle;
            view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view.format           = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, count, layerCount == 1 ? 0u : 1u, 1};
            Call(vkCreateImageView(m_device, &view, nullptr, &image->view), "create image view");
            if (m_ok)
            {
                view.viewType                        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                view.subresourceRange.layerCount     = layerCount;
                view.subresourceRange.baseArrayLayer = 0;
                Call(vkCreateImageView(m_device, &view, nullptr, &image->arrayView), "create array view");
            }
        }
        std::vector<VkBufferImageCopy> copies;
        uint64_t                       bytes = 0;
        for (uint32_t layer = 0; layer < layerCount; ++layer)
        {
            for (uint32_t mip = 0; mip < count; ++mip)
            {
                VkBufferImageCopy copy{};
                copy.bufferOffset     = bytes;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
                copy.imageExtent      = {std::max(1u, pixels.width >> mip), std::max(1u, pixels.height >> mip), 1};
                copies.push_back(copy);
                std::span<const uint8_t> source;
                if (!GetImageLayerMipBytes(pixels, layer, mip, source))
                {
                    Fail("Invalid packed page layer/mip storage");
                }
                bytes += source.size();
            }
        }
        Buffer* staging = MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (m_ok)
        {
            for (const VkBufferImageCopy& copy : copies)
            {
                std::span<const uint8_t> source;
                GetImageLayerMipBytes(pixels, copy.imageSubresource.baseArrayLayer, copy.imageSubresource.mipLevel, source);
                std::memcpy(static_cast<uint8_t*>(staging->mapped) + copy.bufferOffset, source.data(), source.size());
            }
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image                                             = image->handle;
            barrier.subresourceRange                                  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, count, 0, layerCount};
            barrier.oldLayout                                         = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout                                         = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask                                     = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(m_commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &barrier);
            vkCmdCopyBufferToImage(m_commands, staging->handle, image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<uint32_t>(copies.size()), copies.data());
            barrier.oldLayout     = barrier.newLayout;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(m_commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
        }
        return m_ok ? image : nullptr;
    }

    bool Sample(Image& source, Image& low, Image& high, Buffer& output, const Parameters& parameters)
    {
        if (Begin())
        {
            VkDescriptorImageInfo  images[4]{{m_sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {m_sampler, low.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {m_sampler, high.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {m_sampler, low.arrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkDescriptorBufferInfo buffer{output.handle, 0, OutputBytes};
            VkWriteDescriptorSet   writes[5]{};
            for (uint32_t index = 0; index < 5; ++index)
            {
                writes[index].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[index].dstSet          = m_set;
                writes[index].dstBinding      = index;
                writes[index].descriptorCount = 1;
                writes[index].descriptorType =
                    index == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[index].pImageInfo  = index == 4 ? &images[3] : index < 3 ? &images[index] : nullptr;
                writes[index].pBufferInfo = index == 3 ? &buffer : nullptr;
            }
            vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
            vkCmdBindPipeline(m_commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
            vkCmdBindDescriptorSets(m_commands, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_set, 0, nullptr);
            vkCmdPushConstants(m_commands, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters), &parameters);
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass        = m_renderPass;
            begin.framebuffer       = m_framebuffer;
            begin.renderArea.extent = {Side, Side};
            vkCmdBeginRenderPass(m_commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdDraw(m_commands, 3, 1, 0, 0);
            vkCmdEndRenderPass(m_commands);
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer                                            = output.handle;
            barrier.size                                              = OutputBytes;
            vkCmdPipelineBarrier(m_commands, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                                 1, &barrier, 0, nullptr);
            Submit();
        }
        return m_ok;
    }

    void ClearImages()
    {
        // Every upload/sample submit completed before the caller reaches here.
        m_images.clear();
        m_buffers.clear();
    }

private:
    void Fail(const char* message)
    {
        std::fprintf(stderr, "%s\n", message);
        m_ok = false;
    }

    void Call(VkResult result, const char* operation)
    {
        if (result != VK_SUCCESS)
        {
            std::fprintf(stderr, "%s: VkResult %d\n", operation, int(result));
            m_ok = false;
        }
    }

    void Allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties, VkDeviceMemory& memory)
    {
        uint32_t type = UINT32_MAX;
        for (uint32_t index = 0; index < m_memory.memoryTypeCount; ++index)
        {
            if (type == UINT32_MAX && (requirements.memoryTypeBits & (1u << index)) != 0
                && (m_memory.memoryTypes[index].propertyFlags & properties) == properties)
            {
                type = index;
            }
        }
        if (type == UINT32_MAX)
        {
            Fail("No compatible memory type for probe allocation");
        }
        else
        {
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize  = requirements.size;
            allocate.memoryTypeIndex = type;
            Call(vkAllocateMemory(m_device, &allocate, nullptr, &memory), "allocate memory");
        }
    }

    VkShaderModule ReadShader(const char* path)
    {
        std::ifstream         file(path, std::ios::binary | std::ios::ate);
        const std::streamoff  size = file ? static_cast<std::streamoff>(file.tellg()) : std::streamoff(0);
        std::vector<uint32_t> code;
        if (size <= 0 || size > 1024 * 1024 || size % 4 != 0)
        {
            Fail("Invalid/missing probe shader");
        }
        else
        {
            code.resize(static_cast<size_t>(size) / 4);
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(code.data()), size))
            {
                Fail("Cannot read probe shader");
            }
        }
        VkShaderModule module{};
        if (m_ok)
        {
            VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            create.codeSize = code.size() * 4;
            create.pCode    = code.data();
            Call(vkCreateShaderModule(m_device, &create, nullptr, &module), "create shader");
        }
        return module;
    }

    void InitPipeline(const char* vertexPath, const char* fragmentPath)
    {
        VkDescriptorSetLayoutBinding bindings[5]{};
        for (uint32_t index = 0; index < 5; ++index)
        {
            bindings[index].binding         = index;
            bindings[index].descriptorCount = 1;
            bindings[index].descriptorType =
                index == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = 5;
        set.pBindings    = bindings;
        Call(vkCreateDescriptorSetLayout(m_device, &set, nullptr, &m_setLayout), "create descriptor layout");
        if (m_ok)
        {
            VkDescriptorPoolSize       sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool.maxSets       = 1;
            pool.poolSizeCount = 2;
            pool.pPoolSizes    = sizes;
            Call(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool), "create descriptor pool");
        }
        if (m_ok)
        {
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool     = m_descriptorPool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts        = &m_setLayout;
            Call(vkAllocateDescriptorSets(m_device, &allocate, &m_set), "allocate descriptor set");
            VkPushConstantRange        push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Parameters)};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.setLayoutCount         = 1;
            layout.pSetLayouts            = &m_setLayout;
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges    = &push;
            Call(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_pipelineLayout), "create pipeline layout");
        }
        VkShaderModule vertex   = ReadShader(vertexPath);
        VkShaderModule fragment = ReadShader(fragmentPath);
        if (m_ok)
        {
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            pass.subpassCount = 1;
            pass.pSubpasses   = &subpass;
            Call(vkCreateRenderPass(m_device, &pass, nullptr, &m_renderPass), "create attachment-free render pass");
        }
        if (m_ok)
        {
            VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            frame.renderPass = m_renderPass;
            frame.width = frame.height = Side;
            frame.layers               = 1;
            Call(vkCreateFramebuffer(m_device, &frame, nullptr, &m_framebuffer), "create framebuffer");
        }
        if (m_ok)
        {
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (uint32_t index = 0; index < 2; ++index)
            {
                stages[index].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[index].stage  = index == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[index].module = index == 0 ? vertex : fragment;
                stages[index].pName  = "main";
            }
            VkPipelineVertexInputStateCreateInfo   vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport                        viewport{0, 0, float(Side), float(Side), 0, 1};
            VkRect2D                          scissor{{0, 0}, {Side, Side}};
            VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            view.viewportCount = view.scissorCount = 1;
            view.pViewports                        = &viewport;
            view.pScissors                         = &scissor;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode    = VK_CULL_MODE_NONE;
            raster.lineWidth   = 1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            VkGraphicsPipelineCreateInfo        pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipeline.stageCount          = 2;
            pipeline.pStages             = stages;
            pipeline.pVertexInputState   = &vertices;
            pipeline.pInputAssemblyState = &assembly;
            pipeline.pViewportState      = &view;
            pipeline.pRasterizationState = &raster;
            pipeline.pMultisampleState   = &samples;
            pipeline.pColorBlendState    = &blend;
            pipeline.layout              = m_pipelineLayout;
            pipeline.renderPass          = m_renderPass;
            Call(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline),
                 "create graphics pipeline");
        }
        vkDestroyShaderModule(m_device, vertex, nullptr);
        vkDestroyShaderModule(m_device, fragment, nullptr);
    }


    bool                                 m_ok{true};
    VkInstance                           m_instance{};
    VkDebugUtilsMessengerEXT             m_messenger{};
    VkPhysicalDevice                     m_physical{};
    VkPhysicalDeviceProperties           m_properties{};
    VkPhysicalDeviceMemoryProperties     m_memory{};
    VkDevice                             m_device{};
    VkQueue                              m_queue{};
    VkCommandPool                        m_commandPool{};
    VkCommandBuffer                      m_commands{};
    VkSampler                            m_sampler{};
    VkDescriptorSetLayout                m_setLayout{};
    VkDescriptorPool                     m_descriptorPool{};
    VkDescriptorSet                      m_set{};
    VkPipelineLayout                     m_pipelineLayout{};
    VkPipeline                           m_pipeline{};
    VkRenderPass                         m_renderPass{};
    VkFramebuffer                        m_framebuffer{};
    std::vector<std::unique_ptr<Buffer>> m_buffers;
    std::vector<std::unique_ptr<Image>>  m_images;
};

std::shared_ptr<ImagePixels> MakePixels(uint32_t width,
                                        uint32_t height,
                                        uint32_t pattern,
                                        bool     srgb,
                                        bool     partial,
                                        uint32_t maxMips = UINT32_MAX)
{
    std::shared_ptr<ImagePixels> result = std::make_shared<ImagePixels>();
    result->width                       = width;
    result->height                      = height;
    result->format                      = srgb ? PixelFormat::RGBA8Srgb : PixelFormat::RGBA8Unorm;
    uint32_t mip                        = 0;
    bool     done                       = false;
    while (!done)
    {
        std::vector<uint8_t> bytes(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    uint8_t value = 0;
                    if (pattern == 0)
                    {
                        value = static_cast<uint8_t>((x * 31 + y * 67 + channel * 73 + mip * 97) % 256);
                    }
                    else if (pattern == 1)
                    {
                        value = ((x + y + mip + channel) % 2) == 0 ? 0 : 255;
                    }
                    else
                    {
                        value = ((mip + channel) % 2) == 0 ? 0 : 255;
                    }
                    bytes[(size_t(y) * width + x) * 4 + channel] = value;
                }
            }
        }
        result->mips.push_back(std::move(bytes));
        ++mip;
        done   = (width == 1 && height == 1) || (partial && mip == 2) || mip == maxMips;
        width  = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    result->bytes = result->mips[0];
    return result;
}

struct Metrics
{
    double   maxError{0};
    uint64_t aboveStep{0};
    uint64_t alphaChanges{0};
    uint32_t maxByteError{0};
    uint32_t maxX{0};
    uint32_t maxY{0};
    uint32_t maxChannel{0};
    double   maxQueryDifference{0};
    double   maxIdealDifference{0};
    uint64_t wrongBoundLevels{0};
    double   maxNativeBlendError{0};
    double   maxRegionError{0};
    bool     finite{true};
};

Metrics Compare(const Buffer& buffer)
{
    Metrics      result;
    const float* data = static_cast<const float*>(buffer.mapped);
    for (uint32_t pixel = 0; pixel < Side * Side; ++pixel)
    {
        const float* reference     = data + pixel * 20;
        const float* atlas         = reference + 4;
        const float* lods          = reference + 8;
        result.maxQueryDifference  = std::max(result.maxQueryDifference, std::abs(double(lods[0]) - lods[1]));
        result.maxIdealDifference  = std::max(result.maxIdealDifference, std::abs(double(lods[0]) - lods[2]));
        result.wrongBoundLevels   += lods[3] != 0;
        result.alphaChanges       += (reference[3] >= 0.5f) != (atlas[3] >= 0.5f);
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            if (!std::isfinite(reference[channel]) || !std::isfinite(atlas[channel]))
            {
                result.finite = false;
            }
            else
            {
                const double error = std::abs(double(reference[channel]) - atlas[channel]);
                result.maxNativeBlendError =
                    std::max(result.maxNativeBlendError, std::abs(double(reference[channel]) - reference[12 + channel]));
                result.maxRegionError  = std::max(result.maxRegionError, double(std::max(reference[16], reference[17])));
                result.aboveStep      += error > 1.0 / 255.0 + 1e-7;
                const uint32_t byteError =
                    static_cast<uint32_t>(std::abs(std::lround(reference[channel] * 255) - std::lround(atlas[channel] * 255)));
                result.maxByteError = std::max(result.maxByteError, byteError);
                if (error > result.maxError)
                {
                    result.maxError   = error;
                    result.maxX       = pixel % Side;
                    result.maxY       = pixel / Side;
                    result.maxChannel = channel;
                }
            }
        }
    }
    return result;
}

void Region(float* output, const AtlasMipRegion& region)
{
    output[0] = float(region.x);
    output[1] = float(region.y);
    output[2] = float(region.width);
    output[3] = float(region.height);
}

void PrintOneDimensionalSamples(const Buffer& buffer, uint32_t query)
{
    // In the 1x17 checker fixture, red directly exposes the filtered weight.
    // Report distinct rows rather than repeating the same weight across columns.
    const float* data = static_cast<const float*>(buffer.mapped);
    for (uint32_t row = 0; row < 12; ++row)
    {
        const float* sample = data + row * Side * 20;
        std::printf("{\"kind\":\"weight_diagnostic\",\"query\":%u,\"sampling_grid\":%u,\"row\":%u,"
                    "\"uv\":[%.9g,%.9g],\"uv_bits\":[%u,%u],\"reference_red\":%.9g,\"atlas_red\":%.9g,"
                    "\"reference_alpha\":%.9g,\"atlas_alpha\":%.9g}\n",
                    query, query / 2, row, sample[18], sample[19], std::bit_cast<uint32_t>(sample[18]),
                    std::bit_cast<uint32_t>(sample[19]), sample[0], sample[4], sample[3], sample[7]);
    }
}

// Probe-only alternative: three equal-sized power-of-two tiles in a 2x2 grid.
// All native mip cells retain their own gutter and compression-block boundary.
// This deliberately measures the full-tail storage cost; it is not a production packer.
AtlasPackResult NativePrototype(const std::vector<AtlasTileInput>& inputs)
{
    AtlasPackResult result;
    bool            valid = inputs.size() == 3 && inputs[0].pixels != nullptr;
    if (valid)
    {
        const ImagePixels& source = *inputs[0].pixels;
        const uint32_t     count  = static_cast<uint32_t>(source.mips.size());
        valid = std::has_single_bit(source.width) && std::has_single_bit(source.height) && source.width <= 256
             && source.height <= 256 && count > 0 && count <= 9 && (std::min(source.width, source.height) >> (count - 1)) != 0;
        for (const AtlasTileInput& input : inputs)
        {
            valid = valid && input.pixels && input.pixels->width == source.width && input.pixels->height == source.height
                 && input.pixels->format == source.format && input.pixels->mips.size() == count;
        }
        if (valid)
        {
            const uint32_t               gutter     = 1u << (count - 1);
            const uint32_t               alignment  = 4 * gutter;
            const uint32_t               cellWidth  = (source.width + 2 * gutter + alignment - 1) / alignment * alignment;
            const uint32_t               cellHeight = (source.height + 2 * gutter + alignment - 1) / alignment * alignment;
            std::shared_ptr<ImagePixels> page       = std::make_shared<ImagePixels>();
            page->width                             = std::bit_ceil(cellWidth * 2);
            page->height                            = std::bit_ceil(cellHeight * 2);
            page->format                            = source.format;
            for (uint32_t level = 0; level < count; ++level)
            {
                const uint32_t       pageWidth  = page->width >> level;
                const uint32_t       pageHeight = page->height >> level;
                const uint32_t       tileWidth  = source.width >> level;
                const uint32_t       tileHeight = source.height >> level;
                const uint32_t       cellX      = cellWidth >> level;
                const uint32_t       cellY      = cellHeight >> level;
                const uint32_t       guard      = gutter >> level;
                std::vector<uint8_t> bytes(size_t(pageWidth) * pageHeight * 4, 0);
                for (uint32_t tile = 0; tile < inputs.size(); ++tile)
                {
                    const std::vector<uint8_t>& pixels = inputs[tile].pixels->mips[level];
                    for (uint32_t y = 0; y < cellY; ++y)
                    {
                        const uint32_t sy = y < guard ? 0 : std::min(y - guard, tileHeight - 1);
                        for (uint32_t x = 0; x < cellX; ++x)
                        {
                            const uint32_t sx     = x < guard ? 0 : std::min(x - guard, tileWidth - 1);
                            const size_t   offset = (size_t((tile / 2) * cellY + y) * pageWidth + (tile % 2) * cellX + x) * 4;
                            std::memcpy(bytes.data() + offset, pixels.data() + (size_t(sy) * tileWidth + sx) * 4, 4);
                        }
                    }
                }
                page->mips.push_back(std::move(bytes));
            }
            page->bytes = page->mips[0];
            result.pages.push_back({0, page});
            for (uint32_t tile = 0; tile < inputs.size(); ++tile)
            {
                result.tiles.push_back({inputs[tile].id, static_cast<uint32_t>(result.mips.size()), count});
                for (uint32_t level = 0; level < count; ++level)
                {
                    result.mips.push_back({0, ((tile % 2) * cellWidth + gutter) >> level,
                                           ((tile / 2) * cellHeight + gutter) >> level, source.width >> level,
                                           source.height >> level});
                }
            }
        }
    }
    if (!valid)
    {
        result.status = {StatusCode::InvalidArgument, "Native atlas probe needs three bounded equal-sized power-of-two tiles"};
    }
    return result;
}

AtlasPackResult ArrayPages(std::span<const AtlasTileInput> inputs, uint64_t& budget)
{
    AtlasPackResult result;
    ArrayPackResult packed = PackArrayPages(inputs, {}, budget);
    result.status          = packed.status;
    result.pages           = std::move(packed.pages);
    for (const ArrayTileLayer& tile : packed.tiles)
    {
        if (tile.id == 100)
        {
            if (tile.page >= result.pages.size() || tile.layer != 1)
            {
                result.status = {StatusCode::InvalidData, "Probe subject must occupy the middle array layer"};
            }
            else
            {
                const ImagePixels& source = *result.pages[tile.page].pixels;
                const uint32_t     count  = static_cast<uint32_t>(source.mips.size());
                result.tiles.push_back({100, 0, count});
                for (uint32_t mip = 0; mip < count; ++mip)
                {
                    result.mips.push_back(
                        {tile.page, 0, 0, std::max(1u, source.width >> mip), std::max(1u, source.height >> mip)});
                }
            }
        }
    }
    return result;
}

bool RunCase(Device&   device,
             uint32_t  width,
             uint32_t  height,
             uint32_t  pattern,
             bool      srgb,
             bool      partial,
             bool      powerTwo,
             bool      quantize,
             uint32_t  layout,
             uint32_t& failures)
{
    const bool                         native  = layout == 1;
    const bool                         array   = layout == 2 || layout == 3 || layout == 6;
    const bool                         single  = layout >= 4;
    const bool                         grouped = native || array;
    const uint32_t                     maxMips = single ? 1 : UINT32_MAX;
    const std::shared_ptr<ImagePixels> source  = MakePixels(width, height, pattern, srgb, partial, maxMips);
    std::vector<AtlasTileInput>        inputs{
        {100, 0, source},
        {1, 0, MakePixels(grouped ? width : 140, grouped ? height : 80, 1, srgb, grouped && partial, maxMips)},
        {2, 0, MakePixels(grouped ? width : 67, grouped ? height : 99, 2, srgb, grouped && partial, maxMips)}};
    if (array)
    {
        inputs[2].id = 200;
    }
    AtlasPackingOptions options;
    options.maxPageDimension     = width >= 512 || height >= 512 ? 2048 : 512;
    options.powerOfTwoPages      = powerTwo;
    uint64_t              budget = 64ull << 20;
    const AtlasPackResult packed = native ? NativePrototype(inputs)
                                 : array  ? ArrayPages(inputs, budget)
                                          : PackAtlasTiles(inputs, options, budget);
    bool                  valid  = packed.status.Succeeded();
    if (!valid)
    {
        std::fprintf(stderr, "Packing probe: %s\n", packed.status.message.c_str());
    }
    Image*              reference = nullptr;
    Buffer*             output    = nullptr;
    std::vector<Image*> pages;
    if (valid && device.Begin())
    {
        reference                 = device.Upload(*source);
        uint64_t sourceAllocation = reference != nullptr ? reference->allocationBytes : 0;
        if (grouped || single)
        {
            for (size_t index = 0; device.Good() && index < inputs.size(); ++index)
            {
                Image* extra      = inputs[index].id != 100 ? device.Upload(*inputs[index].pixels) : nullptr;
                sourceAllocation += extra != nullptr ? extra->allocationBytes : 0;
            }
        }
        uint64_t atlasAllocation = 0;
        for (const AtlasPage& page : packed.pages)
        {
            if (device.Good())
            {
                pages.push_back(device.Upload(*page.pixels));
                atlasAllocation += pages.back() != nullptr ? pages.back()->allocationBytes : 0;
            }
        }
        output = device.MakeBuffer(OutputBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        valid  = device.Submit();
        if (valid && (grouped || single))
        {
            std::printf("{\"kind\":\"allocation\",\"width\":%u,\"height\":%u,\"mips\":%zu,\"srgb\":%s,"
                        "\"source_images\":3,\"atlas_images\":%zu,\"source_bytes\":%llu,\"atlas_bytes\":%llu}\n",
                        width, height, source->mips.size(), srgb ? "true" : "false", pages.size(),
                        static_cast<unsigned long long>(sourceAllocation), static_cast<unsigned long long>(atlasAllocation));
        }
    }
    const uint32_t count  = static_cast<uint32_t>(source->mips.size());
    const float    maxLod = float(count - 1);
    const float    lods[]{-1, 0, 0.125f, 0.499f, 0.5f, 0.999f, 1.125f, 2.3f, 3.7f, maxLod - 0.125f, maxLod, maxLod + 2};
    const AtlasTileRegions* tile = nullptr;
    for (const AtlasTileRegions& entry : packed.tiles)
    {
        if (entry.id == 100)
        {
            tile = &entry;
        }
    }
    valid = valid && device.Good() && tile != nullptr;
    for (uint32_t query = 0; valid && query < (single ? 12u : 24u); ++query)
    {
        Parameters parameters;
        // With one physical level and no anisotropy, the previous twelve LOD
        // and six gradient operations repeated the same spatial samples. Keep
        // one explicit operation and one oblique minifying gradient per grid.
        const uint32_t operation = single ? (query % 2 == 0 ? 1u : 17u) : query;
        parameters.sampling[0]   = single ? float(query / 2) : 0.0f;
        parameters.sampling[1]   = float(device.SubtexelPrecision());
        float lod                = operation < 12 ? lods[operation] : 0.3f + float((operation - 12) % 6) * 0.75f;
        parameters.query[0]      = lod;
        parameters.query[1]      = operation < 12 ? 0.0f : operation < 18 ? 1.0f : 2.0f;
        parameters.query[2]      = float(Side);
        parameters.query[3]      = maxLod;
        if (operation >= 12)
        {
            const float angle       = float((operation - 12) % 6) * 0.37f;
            const float major       = std::exp2(lod);
            const float minor       = major / std::exp2(float((operation - 12) % 6));
            parameters.gradients[0] = std::cos(angle) * major / width;
            parameters.gradients[1] = std::sin(angle) * major / height;
            parameters.gradients[2] = -std::sin(angle) * minor / width;
            parameters.gradients[3] = std::cos(angle) * minor / height;
        }
        lod                              = std::clamp(lod, 0.0f, maxLod);
        const uint32_t        low        = static_cast<uint32_t>(std::floor(lod));
        const uint32_t        high       = std::min(low + 1, count - 1);
        const AtlasMipRegion& lowRegion  = packed.mips[tile->firstMip + (native ? 0 : low)];
        const AtlasMipRegion& highRegion = packed.mips[tile->firstMip + (native ? 0 : high)];
        Region(parameters.low, lowRegion);
        Region(parameters.high, highRegion);
        parameters.settings[0] = float(device.MipPrecision());
        parameters.settings[1] = quantize ? 1.0f : 0.0f;
        parameters.settings[2] = float(low);
        parameters.settings[3] = float(layout);
        valid = device.Sample(*reference, *pages[lowRegion.page], *pages[highRegion.page], *output, parameters);
        if (valid)
        {
            const Metrics metrics = Compare(*output);
            const bool    pass  = metrics.finite && metrics.maxByteError <= 1 && (layout != 0 || metrics.wrongBoundLevels == 0)
                               && (!single || metrics.alphaChanges == 0);
            failures           += !pass;
            std::printf(
                "{\"kind\":\"sample\",\"width\":%u,\"height\":%u,\"pattern\":%u,\"srgb\":%s,\"partial\":%s,"
                "\"mode\":\"%s\",\"query\":%u,\"sampling_grid\":%u,\"lod\":%.9g,\"pages\":%zu,\"low_page\":%u,\"high_page\":%u,"
                "\"samples\":%u,\"max_error\":%.12g,\"max_byte_error\":%u,\"channels_above_step\":%llu,"
                "\"alpha_changes\":%llu,\"finite\":%s,\"pass\":%s,\"max_x\":%u,\"max_y\":%u,\"max_channel\":%u,\"query_difference\":%.9g,\"ideal_difference\":%.9g,\"wrong_bound_levels\":%llu,\"native_blend_error\":%.12g,\"region_error\":%.12g}\n",
                width, height, pattern, srgb ? "true" : "false", partial ? "true" : "false",
                operation < 12   ? "explicit"
                : operation < 18 ? "ideal_gradient"
                                 : "hardware_gradient",
                query, uint32_t(parameters.sampling[0]), parameters.query[0], packed.pages.size(), lowRegion.page,
                highRegion.page, Side * Side, metrics.maxError, metrics.maxByteError,
                static_cast<unsigned long long>(metrics.aboveStep), static_cast<unsigned long long>(metrics.alphaChanges),
                metrics.finite ? "true" : "false", pass ? "true" : "false", metrics.maxX, metrics.maxY, metrics.maxChannel,
                metrics.maxQueryDifference, metrics.maxIdealDifference,
                static_cast<unsigned long long>(metrics.wrongBoundLevels), metrics.maxNativeBlendError, metrics.maxRegionError);
            if (width == 1 && height == 17 && pattern == 1 && !srgb && single && query % 2 == 0)
            {
                PrintOneDimensionalSamples(*output, query);
            }
        }
    }
    if (device.Good())
    {
        device.ClearImages();
    }
    return valid;
}
} // namespace

bool ParseLayout(const char* text, uint32_t& layout)
{
    const char*                  end    = text + std::strlen(text);
    const std::from_chars_result parsed = std::from_chars(text, end, layout);
    const bool                   valid  = parsed.ec == std::errc{} && parsed.ptr == end && layout <= 14
                                       && (ZENASSET_PROBE_FLOAT64 || (layout != 10 && layout != 11 && layout != 13));
    return valid;
}

int main(int argc, char** argv)
{
    uint32_t layout   = 0;
    bool     valid    = argc == 7 && (std::strcmp(argv[3], "0") == 0 || std::strcmp(argv[3], "1") == 0)
                     && (std::strcmp(argv[4], "0") == 0 || std::strcmp(argv[4], "1") == 0) && ParseLayout(argv[5], layout)
                     && (std::strcmp(argv[6], "0") == 0 || std::strcmp(argv[6], "1") == 0);
    uint32_t failures = 0;
    if (valid)
    {
        std::printf(
            "{\"kind\":\"settings\",\"probe_revision\":10,\"power_two_pages\":%s,\"quantize_blend\":%s,\"layout\":%u,\"anisotropy\":%s}\n",
            argv[3][0] == '1' ? "true" : "false", argv[4][0] == '1' ? "true" : "false", layout,
            argv[6][0] == '1' ? "true" : "false");
        Device device;
        valid = device.Init(argv[1], argv[2], argv[6][0] == '1');
        const std::array<uint32_t, 2> logicalDimensions[]{{32, 32}, {13, 7}, {1, 17}, {257, 129}};
        const std::array<uint32_t, 2> nativeDimensions[]{{32, 32}, {8, 32}, {256, 256}, {256, 128}};
        const std::array<uint32_t, 2> singleDimensions[]{{32, 32},   {13, 7},    {1, 17},     {257, 129}, {127, 255},
                                                         {1, 1},     {2, 2},     {4, 8},      {8, 4},     {16, 64},
                                                         {128, 256}, {256, 128}, {512, 1024}, {1024, 512}};
        const std::span<const std::array<uint32_t, 2>> dimensions =
            layout == 1   ? std::span<const std::array<uint32_t, 2>>(nativeDimensions)
            : layout >= 4 ? std::span<const std::array<uint32_t, 2>>(singleDimensions)
                          : std::span<const std::array<uint32_t, 2>>(logicalDimensions);
        for (const std::array<uint32_t, 2>& size : dimensions)
        {
            for (uint32_t pattern = 0; valid && pattern < 3; ++pattern)
            {
                for (uint32_t mode = 0; valid && mode < (layout >= 4 ? 2u : 4u); ++mode)
                {
                    if (layout != 1 || size[0] == size[1] || (mode & 2) != 0)
                    {
                        valid = RunCase(device, size[0], size[1], pattern, (mode & 1) != 0, (mode & 2) != 0, argv[3][0] == '1',
                                        argv[4][0] == '1', layout, failures);
                    }
                }
            }
        }
    }
    else
    {
        std::fprintf(
            stderr,
            "Usage: atlas_sampling shader.vert.spv shader.frag.spv power_two_pages(0|1) quantize_blend(0|1) layout(0=logical|1=native|2=array|3=array_2d_view|4=single_level_spatial|5=single_level_precise|6=single_level_array|7=tile_bilinear|8=tile_quantized_bilinear|9=tile_quantized_native|10=tile_double_even|11=tile_double_half_up|12=tile_float_half_up|13=normalized_fixed24|14=tile_float_floor) anisotropy(0|1)\nLayouts 10, 11 and 13 require a ZENASSET_PROBE_FLOAT64 build. ZEN_VULKAN_DEVICE selects a GPU by name substring; unmatched requests fail.\n");
    }
    std::printf("{\"kind\":\"summary\",\"execution_ok\":%s,\"failed_comparisons\":%u,\"validation_errors\":%u}\n",
                valid ? "true" : "false", failures, validationErrors.load());
    return !valid || validationErrors != 0 ? 2 : failures != 0 ? 1 : 0;
}
