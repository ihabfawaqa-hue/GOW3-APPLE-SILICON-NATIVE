#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct FormatResult {
    std::string name;
    VkFormat format{};
    VkFormatFeatureFlags optimal{};
    bool image_create{};
};

struct Results {
    std::string stage = "startup";
    std::string error;
    std::string device;
    std::string driver;
    std::string driver_id;
    uint32_t api_version{};
    uint32_t driver_version{};
    uint32_t device_count{};
    bool instance_created{};
    bool metal_surface_created{};
    bool source_queue_supports_present{};
    bool portability_enumeration_available{};
    bool portability_enumeration_enabled{};
    bool macos_portability_patch_enabled{};
    bool source_instance_configuration{};
    bool source_instance_device_enumerated{};
    bool portability_subset_advertised{};
    bool source_enables_portability_subset{};
    bool source_required_extensions_present{};
    bool source_required_features_present{};
    bool source_buffer_cache_features_present{};
    bool source_optional_feature_requests_valid{true};
    bool device_created{};
    bool source_required_device_create_succeeded{};
    bool swapchain_created{};
    bool swapchain_image_presented{};
    bool push_descriptor_compute_passed{};
    bool offscreen_clear_readback_passed{};
    bool image_alias_readback_passed{};
    bool view_reinterpretation_created{};
    std::string source_device_create_error;
    uint32_t queue_family = UINT32_MAX;
    uint32_t first_pixel[4]{};
    std::map<std::string, bool> extensions;
    std::map<std::string, bool> features;
    std::map<std::string, bool> optional_feature_requests;
    std::map<std::string, bool> workloads;
    std::vector<FormatResult> formats;
};

std::string JsonEscape(const std::string& input) {
    std::ostringstream out;
    for (const unsigned char c : input) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out << "\\u00" << hex[c >> 4] << hex[c & 0x0f];
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    return out.str();
}

const char* Bool(bool value) { return value ? "true" : "false"; }

std::string Version(uint32_t version) {
    return std::to_string(VK_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_VERSION_MINOR(version)) + "." +
           std::to_string(VK_VERSION_PATCH(version));
}

const char* Verdict(const Results& r) {
    const bool required = r.instance_created && r.source_required_extensions_present &&
                          r.source_required_features_present && r.source_required_device_create_succeeded &&
                          r.device_created && r.source_queue_supports_present &&
                          r.swapchain_image_presented && r.push_descriptor_compute_passed &&
                          r.offscreen_clear_readback_passed;
    if (required && r.image_alias_readback_passed && r.view_reinterpretation_created &&
        r.source_optional_feature_requests_valid) return "PASS";
    return r.device_created ? "PARTIAL" : "FAIL";
}

std::string ResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    default: return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
    }
}

void SetWorkload(Results& results, const std::string& key, bool passed,
                 const std::string& detail) {
    results.workloads[key] = passed;
    if (!passed && !detail.empty()) {
        if (!results.error.empty()) results.error += "; ";
        results.error += key + ": " + detail;
    }
}

void WriteJson(const Results& r, const std::string& output_path) {
    std::ostringstream out;
    out << "{\n  \"schema\": 1,\n"
        << "  \"stage\": \"" << JsonEscape(r.stage) << "\",\n"
        << "  \"error\": \"" << JsonEscape(r.error) << "\",\n"
        << "  \"device\": \"" << JsonEscape(r.device) << "\",\n"
        << "  \"driver\": \"" << JsonEscape(r.driver) << "\",\n"
        << "  \"driver_id\": \"" << JsonEscape(r.driver_id) << "\",\n"
        << "  \"api_version\": \"" << Version(r.api_version) << "\",\n"
        << "  \"driver_version\": \"" << Version(r.driver_version) << "\",\n"
        << "  \"device_count\": " << r.device_count << ",\n"
        << "  \"queue_family\": ";
    if (r.queue_family == UINT32_MAX) out << "null";
    else out << r.queue_family;
    out << ",\n  \"instance_created\": " << Bool(r.instance_created)
        << ",\n  \"metal_surface_created\": " << Bool(r.metal_surface_created)
        << ",\n  \"source_queue_supports_present\": "
        << Bool(r.source_queue_supports_present)
        << ",\n  \"portability_enumeration_available\": "
        << Bool(r.portability_enumeration_available)
        << ",\n  \"portability_enumeration_enabled\": "
        << Bool(r.portability_enumeration_enabled)
        << ",\n  \"macos_portability_patch_enabled\": "
        << Bool(r.macos_portability_patch_enabled)
        << ",\n  \"source_instance_configuration\": "
        << Bool(r.source_instance_configuration)
        << ",\n  \"source_instance_device_enumerated\": "
        << Bool(r.source_instance_device_enumerated)
        << ",\n  \"portability_subset_advertised\": "
        << Bool(r.portability_subset_advertised)
        << ",\n  \"source_enables_portability_subset\": "
        << Bool(r.source_enables_portability_subset)
        << ",\n  \"source_required_extensions_present\": "
        << Bool(r.source_required_extensions_present)
        << ",\n  \"source_required_features_present\": "
        << Bool(r.source_required_features_present)
        << ",\n  \"source_buffer_cache_features_present\": "
        << Bool(r.source_buffer_cache_features_present)
        << ",\n  \"source_optional_feature_requests_valid\": "
        << Bool(r.source_optional_feature_requests_valid)
        << ",\n  \"device_created\": " << Bool(r.device_created)
        << ",\n  \"source_required_device_create_succeeded\": "
        << Bool(r.source_required_device_create_succeeded)
        << ",\n  \"source_device_create_error\": \""
        << JsonEscape(r.source_device_create_error) << "\",\n  \"swapchain_created\": "
        << Bool(r.swapchain_created) << ",\n  \"swapchain_image_presented\": "
        << Bool(r.swapchain_image_presented)
        << ",\n  \"push_descriptor_compute_storage_image_readback_passed\": "
        << Bool(r.push_descriptor_compute_passed)
        << ",\n  \"offscreen_clear_readback_passed\": "
        << Bool(r.offscreen_clear_readback_passed)
        << ",\n  \"image_alias_readback_passed\": "
        << Bool(r.image_alias_readback_passed)
        << ",\n  \"view_reinterpretation_created\": "
        << Bool(r.view_reinterpretation_created)
        << ",\n  \"first_pixel_rgba\": [" << r.first_pixel[0] << ", "
        << r.first_pixel[1] << ", " << r.first_pixel[2] << ", "
        << r.first_pixel[3] << "],\n";

    const auto write_bool_map = [&](const char* name, const auto& map, bool comma) {
        out << "  \"" << name << "\": {";
        bool first = true;
        for (const auto& [key, value] : map) {
            out << (first ? "\n" : ",\n") << "    \"" << JsonEscape(key)
                << "\": " << Bool(value);
            first = false;
        }
        if (!first) out << '\n' << "  ";
        out << "}" << (comma ? ",\n" : "\n");
    };
    write_bool_map("required_and_reported_extensions", r.extensions, true);
    write_bool_map("required_and_reported_features", r.features, true);
    write_bool_map("upstream_unconditional_optional_feature_requests", r.optional_feature_requests, true);
    write_bool_map("workloads", r.workloads, true);
    out << "  \"formats\": [";
    for (size_t i = 0; i < r.formats.size(); ++i) {
        const auto& f = r.formats[i];
        out << (i ? ",\n" : "\n") << "    {\"format\": \"" << f.name
            << "\", \"optimal_tiling_features\": " << f.optimal
            << ", \"sampled\": "
            << Bool((f.optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0)
            << ", \"storage\": "
            << Bool((f.optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0)
            << ", \"color_attachment\": "
            << Bool((f.optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0)
            << ", \"depth_stencil_attachment\": "
            << Bool((f.optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
            << ", \"transfer_src\": "
            << Bool((f.optimal & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0)
            << ", \"transfer_dst\": "
            << Bool((f.optimal & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0)
            << ", \"image_create\": " << Bool(f.image_create) << "}";
    }
    if (!r.formats.empty()) out << '\n' << "  ";
    out << "],\n  \"verdict\": \"";
    out << Verdict(r);
    out << "\"\n}\n";

    if (output_path.empty() || output_path == "-") {
        std::cout << out.str();
    } else {
        std::ofstream file(output_path);
        if (!file) {
            std::cerr << "could not open output path: " << output_path << '\n';
            std::cout << out.str();
            return;
        }
        file << out.str();
    }
}

bool HasInstanceExtension(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data()) != VK_SUCCESS) return false;
    return std::any_of(props.begin(), props.end(), [name](const auto& p) {
        return std::strcmp(p.extensionName, name) == 0;
    });
}

std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice physical) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, props.data()) != VK_SUCCESS) return {};
    return props;
}

bool HasDeviceExtension(const std::vector<VkExtensionProperties>& props, const char* name) {
    return std::any_of(props.begin(), props.end(), [name](const auto& p) {
        return std::strcmp(p.extensionName, name) == 0;
    });
}

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t type_bits,
                        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0) continue;
        const auto flags = props.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

std::vector<uint32_t> ReadShader() {
    std::ifstream file(GOW3_PROBE_SHADER_PATH, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0 || (size % 4) != 0) return {};
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), size);
    return file ? words : std::vector<uint32_t>{};
}

struct ImageResources {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

bool CreateImage(VkPhysicalDevice physical, VkDevice device, VkFormat format,
                 VkImageUsageFlags usage, VkImageCreateFlags flags, ImageResources& out,
                 VkImageViewType view_type, VkFormat view_format, std::string& error);
void DestroyImage(VkDevice device, ImageResources& image);

bool RunPushDescriptorCompute(VkPhysicalDevice physical, VkDevice device,
                              VkQueue queue, uint32_t family, std::string& error) {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    ImageResources storage_image{};
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    void* image_mapped = nullptr;
    bool passed = false;
    auto fail = [&](const char* stage, VkResult result) {
        error = std::string(stage) + ": " + ResultName(result);
    };

    do {
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(uint32_t),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
        if (result != VK_SUCCESS) { fail("create storage buffer", result); break; }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        const uint32_t type = FindMemoryType(physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) { error = "no host-visible memory type for result buffer"; break; }
        const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = type,
        };
        result = vkAllocateMemory(device, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) { fail("allocate result memory", result); break; }
        result = vkBindBufferMemory(device, buffer, memory, 0);
        if (result != VK_SUCCESS) { fail("bind result buffer", result); break; }
        result = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS) { fail("map result memory", result); break; }
        *static_cast<uint32_t*>(mapped) = 0;
        if (!CreateImage(physical, device, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
                storage_image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, error)) break;
        const VkBufferCreateInfo readback_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 64,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        result = vkCreateBuffer(device, &readback_info, nullptr, &readback_buffer);
        if (result != VK_SUCCESS) { fail("create storage image readback buffer", result); break; }
        VkMemoryRequirements readback_requirements{};
        vkGetBufferMemoryRequirements(device, readback_buffer, &readback_requirements);
        const uint32_t readback_type = FindMemoryType(physical, readback_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (readback_type == UINT32_MAX) { error = "no host-visible storage image readback memory"; break; }
        const VkMemoryAllocateInfo readback_allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = readback_requirements.size,
            .memoryTypeIndex = readback_type,
        };
        result = vkAllocateMemory(device, &readback_allocation, nullptr, &readback_memory);
        if (result != VK_SUCCESS) { fail("allocate storage image readback memory", result); break; }
        result = vkBindBufferMemory(device, readback_buffer, readback_memory, 0);
        if (result != VK_SUCCESS) { fail("bind storage image readback buffer", result); break; }
        result = vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &image_mapped);
        if (result != VK_SUCCESS) { fail("map storage image readback memory", result); break; }
        std::memset(image_mapped, 0, 64);
        const std::array bindings = {
            VkDescriptorSetLayoutBinding{.binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            VkDescriptorSetLayoutBinding{.binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        };
        const VkDescriptorSetLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };
        result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &set_layout);
        if (result != VK_SUCCESS) { fail("create push descriptor layout", result); break; }
        const VkPipelineLayoutCreateInfo pipeline_layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &set_layout,
        };
        result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout);
        if (result != VK_SUCCESS) { fail("create pipeline layout", result); break; }
        const auto code = ReadShader();
        if (code.empty()) { error = "could not read compiled compute shader"; break; }
        const VkShaderModuleCreateInfo shader_info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = code.size() * sizeof(uint32_t),
            .pCode = code.data(),
        };
        result = vkCreateShaderModule(device, &shader_info, nullptr, &shader);
        if (result != VK_SUCCESS) { fail("create shader module", result); break; }
        const VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main",
        };
        const VkComputePipelineCreateInfo pipeline_info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = stage,
            .layout = pipeline_layout,
        };
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        if (result != VK_SUCCESS) { fail("create compute pipeline", result); break; }
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = family,
        };
        result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
        if (result != VK_SUCCESS) { fail("create compute command pool", result); break; }
        VkCommandBuffer command = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo command_alloc{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        result = vkAllocateCommandBuffers(device, &command_alloc, &command);
        if (result != VK_SUCCESS) { fail("allocate compute command", result); break; }
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS) { fail("begin compute command", result); break; }
        const VkImageMemoryBarrier image_to_general{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = storage_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image_to_general);
        const VkDescriptorBufferInfo buffer_descriptor{buffer, 0, sizeof(uint32_t)};
        const VkDescriptorImageInfo image_descriptor{
            .sampler = VK_NULL_HANDLE,
            .imageView = storage_image.view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const std::array writes = {
            VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buffer_descriptor},
            VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 1, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &image_descriptor},
        };
        const auto push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
        if (!push) { error = "vkCmdPushDescriptorSetKHR entry point is unavailable"; break; }
        push(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0,
             static_cast<uint32_t>(writes.size()), writes.data());
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdDispatch(command, 1, 1, 1);
        const VkImageMemoryBarrier image_to_copy{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = storage_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image_to_copy);
        const VkBufferImageCopy image_copy{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {4, 4, 1},
        };
        vkCmdCopyImageToBuffer(command, storage_image.image, VK_IMAGE_LAYOUT_GENERAL,
            readback_buffer, 1, &image_copy);
        const VkMemoryBarrier host_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, nullptr, 0, nullptr);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) { fail("end compute command", result); break; }
        const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(device, &fence_info, nullptr, &fence);
        if (result != VK_SUCCESS) { fail("create compute fence", result); break; }
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command,
        };
        result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result != VK_SUCCESS) { fail("submit compute workload", result); break; }
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL);
        if (result != VK_SUCCESS) { fail("wait for compute workload", result); break; }
        VkPhysicalDeviceMemoryProperties memory_props{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
        if ((memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            const VkMappedMemoryRange range{
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = memory,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            result = vkInvalidateMappedMemoryRanges(device, 1, &range);
            if (result != VK_SUCCESS) { fail("invalidate compute result", result); break; }
        }
        if ((memory_props.memoryTypes[readback_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            const VkMappedMemoryRange range{
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = readback_memory,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            result = vkInvalidateMappedMemoryRanges(device, 1, &range);
            if (result != VK_SUCCESS) { fail("invalidate storage image readback", result); break; }
        }
        const auto* rgba = static_cast<const uint8_t*>(image_mapped);
        passed = *static_cast<uint32_t*>(mapped) == 0x47573321u && rgba[0] == 255 &&
            rgba[1] >= 127 && rgba[1] <= 128 && rgba[2] >= 63 && rgba[2] <= 64 && rgba[3] == 255;
        if (!passed) error = "buffer/image result mismatch: buffer=" +
            std::to_string(*static_cast<uint32_t*>(mapped)) + ", rgba=" +
            std::to_string(rgba[0]) + "," + std::to_string(rgba[1]) + "," +
            std::to_string(rgba[2]) + "," + std::to_string(rgba[3]);
    } while (false);

    if (image_mapped) vkUnmapMemory(device, readback_memory);
    if (mapped) vkUnmapMemory(device, memory);
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (shader) vkDestroyShaderModule(device, shader, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    if (readback_buffer) vkDestroyBuffer(device, readback_buffer, nullptr);
    if (readback_memory) vkFreeMemory(device, readback_memory, nullptr);
    DestroyImage(device, storage_image);
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    return passed;
}

bool CreateImage(VkPhysicalDevice physical, VkDevice device, VkFormat format, VkImageUsageFlags usage,
                 VkImageCreateFlags flags, ImageResources& out, VkImageViewType view_type,
                 VkFormat view_format, std::string& error) {
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = flags,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {4, 4, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(device, &info, nullptr, &out.image);
    if (result != VK_SUCCESS) { error = "create image: " + ResultName(result); return false; }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, out.image, &requirements);
    const uint32_t type = FindMemoryType(physical, requirements.memoryTypeBits,
        0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { error = "no compatible image memory type"; return false; }
    const VkMemoryAllocateInfo allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    result = vkAllocateMemory(device, &allocation, nullptr, &out.memory);
    if (result != VK_SUCCESS) { error = "allocate image memory: " + ResultName(result); return false; }
    result = vkBindImageMemory(device, out.image, out.memory, 0);
    if (result != VK_SUCCESS) { error = "bind image memory: " + ResultName(result); return false; }
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out.image,
        .viewType = view_type,
        .format = view_format,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    result = vkCreateImageView(device, &view_info, nullptr, &out.view);
    if (result != VK_SUCCESS) { error = "create image view: " + ResultName(result); return false; }
    return true;
}

void DestroyImage(VkDevice device, ImageResources& image) {
    if (image.view) vkDestroyImageView(device, image.view, nullptr);
    if (image.image) vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    image = {};
}

bool RunImageReadback(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                      uint32_t family, bool alias, bool& view_reinterpretation_created,
                      uint32_t pixel[4], std::string& error) {
    ImageResources image_a{}, image_b{};
    VkImageView reinterpretation = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool passed = false;
    const VkImageUsageFlags image_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    do {
        if (!alias) {
            if (!CreateImage(physical, device, VK_FORMAT_R8G8B8A8_UNORM, image_usage, 0,
                             image_a, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, error)) break;
        }
        if (alias) {
            // A second VkImage bound to the exact same allocation models two guest
            // interpretations sharing one GPU-memory range. The compatible sRGB view
            // additionally checks mutable-format reinterpretation on that allocation.
            const VkImageCreateInfo image_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .flags = VK_IMAGE_CREATE_ALIAS_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .extent = {4, 4, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = image_usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            VkResult result = vkCreateImage(device, &image_info, nullptr, &image_a.image);
            if (result != VK_SUCCESS) { error = "create alias image A: " + ResultName(result); break; }
            result = vkCreateImage(device, &image_info, nullptr, &image_b.image);
            if (result != VK_SUCCESS) { error = "create aliased image: " + ResultName(result); break; }
            VkMemoryRequirements req_a{}, req_b{};
            vkGetImageMemoryRequirements(device, image_a.image, &req_a);
            vkGetImageMemoryRequirements(device, image_b.image, &req_b);
            const uint32_t type = FindMemoryType(physical,
                req_a.memoryTypeBits & req_b.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type == UINT32_MAX) { error = "no shared image-alias memory type"; break; }
            VkMemoryRequirements allocation_requirements = req_a;
            if (req_b.size > allocation_requirements.size) allocation_requirements.size = req_b.size;
            if (req_b.alignment > allocation_requirements.alignment) allocation_requirements.alignment = req_b.alignment;
            const VkMemoryAllocateInfo allocation{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = allocation_requirements.size,
                .memoryTypeIndex = type,
            };
            result = vkAllocateMemory(device, &allocation, nullptr, &image_b.memory);
            if (result != VK_SUCCESS) { error = "allocate alias memory: " + ResultName(result); break; }
            // The two create calls had separate allocations; replace both with one shared block.
            result = vkBindImageMemory(device, image_a.image, image_b.memory, 0);
            if (result != VK_SUCCESS) { error = "bind alias image A: " + ResultName(result); break; }
            result = vkBindImageMemory(device, image_b.image, image_b.memory, 0);
            if (result != VK_SUCCESS) { error = "bind alias image B: " + ResultName(result); break; }
            const VkImageViewCreateInfo base_view_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image_a.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            result = vkCreateImageView(device, &base_view_info, nullptr, &image_a.view);
            if (result != VK_SUCCESS) { error = "create alias image A view: " + ResultName(result); break; }
            const VkImageViewCreateInfo view_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image_b.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_SRGB,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            result = vkCreateImageView(device, &view_info, nullptr, &reinterpretation);
            if (result != VK_SUCCESS) { error = "create UNORM/SRGB reinterpretation view: " + ResultName(result); break; }
            view_reinterpretation_created = true;
        }
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 64,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
        if (result != VK_SUCCESS) { error = "create image readback buffer: " + ResultName(result); break; }
        VkMemoryRequirements buffer_req{};
        vkGetBufferMemoryRequirements(device, buffer, &buffer_req);
        const uint32_t buffer_type = FindMemoryType(physical, buffer_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (buffer_type == UINT32_MAX) { error = "no host-visible image readback memory"; break; }
        const VkMemoryAllocateInfo buffer_allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = buffer_req.size,
            .memoryTypeIndex = buffer_type,
        };
        result = vkAllocateMemory(device, &buffer_allocation, nullptr, &buffer_memory);
        if (result != VK_SUCCESS) { error = "allocate image readback memory: " + ResultName(result); break; }
        result = vkBindBufferMemory(device, buffer, buffer_memory, 0);
        if (result != VK_SUCCESS) { error = "bind image readback buffer: " + ResultName(result); break; }
        result = vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS) { error = "map image readback memory: " + ResultName(result); break; }
        std::memset(mapped, 0, 64);
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = family,
        };
        result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
        if (result != VK_SUCCESS) { error = "create image command pool: " + ResultName(result); break; }
        VkCommandBuffer command = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo command_alloc{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        result = vkAllocateCommandBuffers(device, &command_alloc, &command);
        if (result != VK_SUCCESS) { error = "allocate image command: " + ResultName(result); break; }
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS) { error = "begin image command: " + ResultName(result); break; }
        const auto prepare_image = [&](VkImage image) {
            const VkImageMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        };
        prepare_image(image_a.image);
        if (alias) prepare_image(image_b.image);
        VkClearColorValue clear{};
        clear.float32[0] = 0.25f;
        clear.float32[1] = 0.50f;
        clear.float32[2] = 0.75f;
        clear.float32[3] = 1.00f;
        const VkImageSubresourceRange clear_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, image_a.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
            &clear_range);
        const VkMemoryBarrier alias_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &alias_barrier, 0, nullptr, 0, nullptr);
        const VkBufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {4, 4, 1},
        };
        vkCmdCopyImageToBuffer(command, alias ? image_b.image : image_a.image,
            VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
        const VkMemoryBarrier host_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, nullptr, 0, nullptr);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) { error = "end image command: " + ResultName(result); break; }
        const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(device, &fence_info, nullptr, &fence);
        if (result != VK_SUCCESS) { error = "create image fence: " + ResultName(result); break; }
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command,
        };
        result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result != VK_SUCCESS) { error = "submit image workload: " + ResultName(result); break; }
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL);
        if (result != VK_SUCCESS) { error = "wait image workload: " + ResultName(result); break; }
        VkPhysicalDeviceMemoryProperties mem_props{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
        if ((mem_props.memoryTypes[buffer_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            const VkMappedMemoryRange range{
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = buffer_memory,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            result = vkInvalidateMappedMemoryRanges(device, 1, &range);
            if (result != VK_SUCCESS) { error = "invalidate image readback: " + ResultName(result); break; }
        }
        auto* bytes = static_cast<uint8_t*>(mapped);
        for (unsigned i = 0; i < 4; ++i) pixel[i] = bytes[i];
        passed = pixel[0] == 64 && pixel[1] == 128 && pixel[2] >= 190 &&
                 pixel[2] <= 192 && pixel[3] == 255;
        if (!passed) error = "readback mismatch: " + std::to_string(pixel[0]) + "," +
            std::to_string(pixel[1]) + "," + std::to_string(pixel[2]) + "," +
            std::to_string(pixel[3]);
    } while (false);

    if (mapped) vkUnmapMemory(device, buffer_memory);
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (buffer_memory) vkFreeMemory(device, buffer_memory, nullptr);
    if (reinterpretation) vkDestroyImageView(device, reinterpretation, nullptr);
    DestroyImage(device, image_a);
    if (image_b.view) vkDestroyImageView(device, image_b.view, nullptr);
    if (image_b.image) vkDestroyImage(device, image_b.image, nullptr);
    if (image_b.memory) vkFreeMemory(device, image_b.memory, nullptr);
    return passed;
}

bool RunSwapchain(VkPhysicalDevice physical, VkDevice device, VkSurfaceKHR surface,
                  VkQueue queue, uint32_t family, Results& results, std::string& error) {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    if (result != VK_SUCCESS) { error = "query surface capabilities: " + ResultName(result); return false; }
    uint32_t format_count = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
    if (result != VK_SUCCESS || format_count == 0) { error = "no swapchain surface format"; return false; }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
    if (result != VK_SUCCESS) { error = "read surface formats: " + ResultName(result); return false; }
    VkSurfaceFormatKHR format = formats.front();
    const auto preferred = std::find_if(formats.begin(), formats.end(), [](const auto& f) {
        return f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM;
    });
    if (preferred != formats.end()) format = *preferred;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(640u, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(360u, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & usage) == 0) usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((caps.supportedUsageFlags & usage) == 0) { error = "surface has no transfer-dst or color-attachment usage"; return false; }
    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha),
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
    if (result != VK_SUCCESS) { error = "create swapchain: " + ResultName(result); return false; }
    results.swapchain_created = true;
    uint32_t swap_image_count = 0;
    result = vkGetSwapchainImagesKHR(device, swapchain, &swap_image_count, nullptr);
    std::vector<VkImage> images(swap_image_count);
    if (result == VK_SUCCESS) result = vkGetSwapchainImagesKHR(device, swapchain, &swap_image_count, images.data());
    if (result != VK_SUCCESS || images.empty()) { error = "get swapchain images: " + ResultName(result); vkDestroySwapchainKHR(device, swapchain, nullptr); return false; }
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore complete = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    result = vkCreateSemaphore(device, &semaphore_info, nullptr, &acquired);
    if (result == VK_SUCCESS) result = vkCreateSemaphore(device, &semaphore_info, nullptr, &complete);
    if (result == VK_SUCCESS) {
        const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
        result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
    }
    if (result == VK_SUCCESS) {
        const VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        result = vkAllocateCommandBuffers(device, &alloc, &command);
    }
    uint32_t index = 0;
    if (result == VK_SUCCESS) result = vkAcquireNextImageKHR(device, swapchain, 5000000000ULL, acquired, VK_NULL_HANDLE, &index);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkResult record = vkBeginCommandBuffer(command, &begin);
        if (record == VK_SUCCESS) {
            const VkImageMemoryBarrier to_present{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = 0,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = images[index],
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_present);
            record = vkEndCommandBuffer(command);
        }
        if (record != VK_SUCCESS) result = record;
        else {
            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            const VkSubmitInfo submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &acquired,
                .pWaitDstStageMask = &wait_stage,
                .commandBufferCount = 1,
                .pCommandBuffers = &command,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &complete,
            };
            result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
            if (result == VK_SUCCESS) {
                const VkPresentInfoKHR present{
                    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &complete,
                    .swapchainCount = 1,
                    .pSwapchains = &swapchain,
                    .pImageIndices = &index,
                };
                result = vkQueuePresentKHR(queue, &present);
                if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                    result = vkQueueWaitIdle(queue);
                    results.swapchain_image_presented = result == VK_SUCCESS;
                }
            }
        }
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) error = "acquire/submit/present: " + ResultName(result);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (acquired) vkDestroySemaphore(device, acquired, nullptr);
    if (complete) vkDestroySemaphore(device, complete, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    return results.swapchain_image_presented;
}

std::vector<const char*> UpstreamOptionalExtensions() {
    return {
        VK_KHR_PIPELINE_BINARY_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_8_EXTENSION_NAME,
        VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME,
        VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
        VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
        VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME,
        VK_AMD_SHADER_EXPLICIT_VERTEX_PARAMETER_EXTENSION_NAME,
        VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
        VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
        VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME,
        VK_AMD_SHADER_IMAGE_LOAD_STORE_LOD_EXTENSION_NAME,
        VK_AMD_GCN_SHADER_EXTENSION_NAME,
        VK_AMD_SHADER_TRINARY_MINMAX_EXTENSION_NAME,
        VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME,
        VK_AMD_MIXED_ATTACHMENT_SAMPLES_EXTENSION_NAME,
        VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
        VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME,
        VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
        VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME,
        VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME,
        VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME,
        VK_KHR_SHADER_CLOCK_EXTENSION_NAME,
    };
}

} // namespace

int main(int argc, char** argv) {
    Results results;
    std::string output_path = "-";
    bool source_instance = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--source-instance") == 0) {
            source_instance = true;
        } else {
            results.error = "usage: gow3-vulkan-probe [--source-instance] [--output PATH|-]";
            WriteJson(results, output_path);
            return 2;
        }
    }
    results.source_instance_configuration = source_instance;
    results.macos_portability_patch_enabled = !source_instance;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        results.error = std::string("SDL_Init: ") + SDL_GetError();
        WriteJson(results, output_path);
        return 2;
    }
    SDL_Window* window = SDL_CreateWindow("GoW3 Vulkan capability probe", 480, 300,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        results.error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        SDL_Quit();
        WriteJson(results, output_path);
        return 2;
    }
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        results.error = std::string("SDL_Vulkan_LoadLibrary: ") + SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        WriteJson(results, output_path);
        return 2;
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    int exit_code = 2;
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS) loader_version = VK_API_VERSION_1_0;
    results.api_version = loader_version;
    if (VK_VERSION_MAJOR(loader_version) < 1 ||
        (VK_VERSION_MAJOR(loader_version) == 1 && VK_VERSION_MINOR(loader_version) < 3)) {
        results.error = "loader does not expose Vulkan 1.3";
        goto done;
    }
    {
        uint32_t extension_count = 0;
        const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
        if (!extensions || !extension_count) {
            results.error = std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError();
            goto done;
        }
        std::vector<const char*> enabled_extensions(extensions, extensions + extension_count);
        results.portability_enumeration_available = HasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        if (!source_instance && results.portability_enumeration_available) {
            enabled_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            results.portability_enumeration_enabled = true;
        }
        const VkApplicationInfo app{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "gow3-vulkan-probe",
            .applicationVersion = 1,
            .pEngineName = "GoW3 source-derived Vulkan probe",
            .engineVersion = 1,
            .apiVersion = VK_API_VERSION_1_3,
        };
        const VkInstanceCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = static_cast<VkInstanceCreateFlags>(results.portability_enumeration_enabled
                ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0),
            .pApplicationInfo = &app,
            .enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size()),
            .ppEnabledExtensionNames = enabled_extensions.data(),
        };
        VkResult result = vkCreateInstance(&create, nullptr, &instance);
        if (result != VK_SUCCESS) {
            results.error = "vkCreateInstance: " + ResultName(result);
            results.stage = "instance-create";
            if (source_instance) exit_code = 1;
            goto done;
        }
        results.instance_created = true;
        exit_code = 1;
    }
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        results.error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        goto done;
    }
    results.metal_surface_created = true;
    {
        uint32_t count = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (result != VK_SUCCESS || count == 0) {
            results.error = "enumerate physical devices: " + ResultName(result);
            results.stage = "physical-device-enumeration";
            exit_code = 1;
            goto done;
        }
        std::vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (result != VK_SUCCESS) { results.error = "read physical devices: " + ResultName(result); goto done; }
        results.device_count = count;
        results.source_instance_device_enumerated = count != 0;
        physical = devices.front();
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical = candidate;
                break;
            }
        }
        VkPhysicalDeviceDriverProperties driver{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 props2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver,
        };
        vkGetPhysicalDeviceProperties2(physical, &props2);
        results.device = props2.properties.deviceName;
        results.driver = driver.driverName;
        results.driver_id = std::to_string(static_cast<int>(driver.driverID));
        results.api_version = props2.properties.apiVersion;
        results.driver_version = props2.properties.driverVersion;
        const auto device_exts = DeviceExtensions(physical);
        const std::array required_exts = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
            VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
            VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
        };
        results.source_required_extensions_present = true;
        for (const char* name : required_exts) {
            const bool present = HasDeviceExtension(device_exts, name);
            results.extensions[name] = present;
            results.source_required_extensions_present &= present;
        }
        constexpr const char* portability_subset = "VK_KHR_portability_subset";
        results.portability_subset_advertised = HasDeviceExtension(device_exts, portability_subset);
        results.source_enables_portability_subset = !source_instance && results.portability_subset_advertised;
        for (const char* name : UpstreamOptionalExtensions()) {
            results.extensions[name] = HasDeviceExtension(device_exts, name);
        }

        VkPhysicalDeviceFeatures2 core{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        };
        VkPhysicalDeviceVulkan11Features f11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        };
        VkPhysicalDeviceVulkan12Features f12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        };
        VkPhysicalDeviceVulkan13Features f13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        };
        VkPhysicalDeviceRobustness2FeaturesEXT robustness{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        };
        VkPhysicalDeviceVertexAttributeDivisorFeatures divisor{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES,
        };
        VkPhysicalDevicePipelineBinaryFeaturesKHR pipeline_binary{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR,
        };
        core.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        void** tail = &f13.pNext;
        if (results.extensions[VK_EXT_ROBUSTNESS_2_EXTENSION_NAME]) { *tail = &robustness; tail = &robustness.pNext; }
        if (results.extensions[VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME]) { *tail = &divisor; tail = &divisor.pNext; }
        if (results.extensions[VK_KHR_PIPELINE_BINARY_EXTENSION_NAME]) { *tail = &pipeline_binary; tail = &pipeline_binary.pNext; }
        vkGetPhysicalDeviceFeatures2(physical, &core);
        results.features["core_robustBufferAccess"] = core.features.robustBufferAccess;
        results.features["core_sparseBinding"] = core.features.sparseBinding;
        results.features["core_sparseResidencyBuffer"] = core.features.sparseResidencyBuffer;
        results.features["robustBufferAccess2"] = robustness.robustBufferAccess2;
        results.features["robustImageAccess2"] = robustness.robustImageAccess2;
        results.features["nullDescriptor"] = robustness.nullDescriptor;
        results.features["vertexAttributeInstanceRateDivisor"] = divisor.vertexAttributeInstanceRateDivisor;
        results.features["shaderStorageImageWriteWithoutFormat"] = core.features.shaderStorageImageWriteWithoutFormat;
        results.features["shaderStorageImageExtendedFormats"] = core.features.shaderStorageImageExtendedFormats;
        results.features["shaderFloat64"] = core.features.shaderFloat64;
        results.features["shaderInt64"] = core.features.shaderInt64;
        results.features["shaderInt16"] = core.features.shaderInt16;
        results.features["core11_storageBuffer16BitAccess"] = f11.storageBuffer16BitAccess;
        results.features["core12_shaderFloat16"] = f12.shaderFloat16;
        results.features["core12_shaderInt8"] = f12.shaderInt8;
        results.features["core12_scalarBlockLayout"] = f12.scalarBlockLayout;
        results.features["core12_timelineSemaphore"] = f12.timelineSemaphore;
        results.features["core12_bufferDeviceAddress"] = f12.bufferDeviceAddress;
        results.features["core13_synchronization2"] = f13.synchronization2;
        results.features["core13_dynamicRendering"] = f13.dynamicRendering;
        results.features["pipelineBinaries"] = pipeline_binary.pipelineBinaries;
        results.source_buffer_cache_features_present = core.features.sparseBinding &&
            core.features.sparseResidencyBuffer && f12.bufferDeviceAddress;
        results.source_required_features_present = robustness.robustBufferAccess2 &&
            robustness.robustImageAccess2 && robustness.nullDescriptor &&
            divisor.vertexAttributeInstanceRateDivisor && results.source_buffer_cache_features_present;

        // The pinned source unconditionally sets these extension feature bits to true
        // whenever their extension name is present, without querying the bit first.
        const auto check_optional_feature = [&](const char* extension, const char* feature,
                                                auto feature_struct, auto member) {
            if (!HasDeviceExtension(device_exts, extension)) return;
            VkPhysicalDeviceFeatures2 query{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                            .pNext = &feature_struct};
            vkGetPhysicalDeviceFeatures2(physical, &query);
            const bool supported = feature_struct.*member;
            results.optional_feature_requests[std::string(extension) + "." + feature] = supported;
            results.source_optional_feature_requests_valid &= supported;
        };
        VkPhysicalDeviceCustomBorderColorFeaturesEXT border{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
        check_optional_feature(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, "customBorderColors", border,
                               &VkPhysicalDeviceCustomBorderColorFeaturesEXT::customBorderColors);
        border = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
        check_optional_feature(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, "customBorderColorWithoutFormat", border,
                               &VkPhysicalDeviceCustomBorderColorFeaturesEXT::customBorderColorWithoutFormat);
        VkPhysicalDeviceDepthClipControlFeaturesEXT depth_control{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
        check_optional_feature(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, "depthClipControl", depth_control,
                               &VkPhysicalDeviceDepthClipControlFeaturesEXT::depthClipControl);
        VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_enable{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
        check_optional_feature(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, "depthClipEnable", depth_enable,
                               &VkPhysicalDeviceDepthClipEnableFeaturesEXT::depthClipEnable);
        VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex_dynamic{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
        check_optional_feature(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, "vertexInputDynamicState", vertex_dynamic,
                               &VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT::vertexInputDynamicState);
        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
        check_optional_feature(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, "fragmentShaderBarycentric", barycentric,
                               &VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR::fragmentShaderBarycentric);
        VkPhysicalDeviceProvokingVertexFeaturesEXT provoking{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
        check_optional_feature(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, "provokingVertexLast", provoking,
                               &VkPhysicalDeviceProvokingVertexFeaturesEXT::provokingVertexLast);
        VkPhysicalDeviceMaintenance5FeaturesKHR maintenance{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        check_optional_feature(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, "maintenance5", maintenance,
                               &VkPhysicalDeviceMaintenance5FeaturesKHR::maintenance5);
        VkPhysicalDeviceMaintenance8FeaturesKHR maintenance8{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
        check_optional_feature(VK_KHR_MAINTENANCE_8_EXTENSION_NAME, "maintenance8", maintenance8,
                               &VkPhysicalDeviceMaintenance8FeaturesKHR::maintenance8);
        const bool pipeline_binaries_requested =
            results.extensions[VK_KHR_MAINTENANCE_5_EXTENSION_NAME] && maintenance.maintenance5 &&
            results.extensions[VK_KHR_PIPELINE_BINARY_EXTENSION_NAME] && pipeline_binary.pipelineBinaries;
        if (pipeline_binaries_requested) {
            results.optional_feature_requests[std::string(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) + ".pipelineBinaries"] =
                pipeline_binary.pipelineBinaries;
            results.source_optional_feature_requests_valid &= pipeline_binary.pipelineBinaries;
        }
        VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback_layout{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
        check_optional_feature(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME,
            "attachmentFeedbackLoopLayout", feedback_layout,
            &VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT::attachmentFeedbackLoopLayout);
        VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT feedback_dynamic{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
        check_optional_feature(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME,
            "attachmentFeedbackLoopDynamicState", feedback_dynamic,
            &VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT::attachmentFeedbackLoopDynamicState);
        VkPhysicalDeviceImageViewMinLodFeaturesEXT image_min_lod{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
        check_optional_feature(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME, "minLod", image_min_lod,
                               &VkPhysicalDeviceImageViewMinLodFeaturesEXT::minLod);

        const std::array format_list = {
            std::pair{"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM},
            std::pair{"R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB},
            std::pair{"B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM},
            std::pair{"A2B10G10R10_UNORM", VK_FORMAT_A2B10G10R10_UNORM_PACK32},
            std::pair{"R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT},
            std::pair{"R32G32B32A32_SFLOAT", VK_FORMAT_R32G32B32A32_SFLOAT},
            std::pair{"D16_UNORM", VK_FORMAT_D16_UNORM},
            std::pair{"D16_UNORM_S8_UINT", VK_FORMAT_D16_UNORM_S8_UINT},
            std::pair{"D32_SFLOAT", VK_FORMAT_D32_SFLOAT},
            std::pair{"D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT},
            std::pair{"D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT},
            std::pair{"BC1_RGBA_UNORM", VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
            std::pair{"BC2_UNORM", VK_FORMAT_BC2_UNORM_BLOCK},
            std::pair{"BC3_UNORM", VK_FORMAT_BC3_UNORM_BLOCK},
            std::pair{"BC4_UNORM", VK_FORMAT_BC4_UNORM_BLOCK},
            std::pair{"BC5_UNORM", VK_FORMAT_BC5_UNORM_BLOCK},
            std::pair{"BC6H_UFLOAT", VK_FORMAT_BC6H_UFLOAT_BLOCK},
            std::pair{"BC7_UNORM", VK_FORMAT_BC7_UNORM_BLOCK},
        };
        for (const auto& [name, format] : format_list) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &props);
            const bool depth = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            const VkImageUsageFlags usage = depth
                ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkImageFormatProperties image_properties{};
            const bool image_create = vkGetPhysicalDeviceImageFormatProperties(physical, format,
                VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &image_properties) == VK_SUCCESS;
            results.formats.push_back({name, format, props.optimalTilingFeatures, image_create});
        }

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
        for (uint32_t i = 0; i < family_count; ++i) {
            VkBool32 present = VK_FALSE;
            const VkResult support = vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
            if (support == VK_SUCCESS && present &&
                (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                results.queue_family = i;
                results.source_queue_supports_present = true;
                break;
            }
        }
        if (results.queue_family == UINT32_MAX) {
            results.error = "no single graphics+compute queue family also supports presentation";
            goto done;
        }
        {
            const float priority = 1.0f;
            const VkDeviceQueueCreateInfo queue_info{
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = results.queue_family,
                .queueCount = 1,
                .pQueuePriorities = &priority,
            };
            VkPhysicalDeviceRobustness2FeaturesEXT source_robustness{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                .robustBufferAccess2 = VK_TRUE,
                .robustImageAccess2 = VK_TRUE,
                .nullDescriptor = VK_TRUE,
            };
            VkPhysicalDeviceVertexAttributeDivisorFeatures source_divisor{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES,
                .vertexAttributeInstanceRateDivisor = VK_TRUE,
            };
            VkPhysicalDeviceVulkan12Features source_v12{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                .bufferDeviceAddress = VK_TRUE,
            };
            source_divisor.pNext = &source_v12;
            source_robustness.pNext = &source_divisor;
            VkPhysicalDeviceFeatures2 source_features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &source_robustness,
                .features{
                    .sparseBinding = VK_TRUE,
                    .sparseResidencyBuffer = VK_TRUE,
                },
            };
            std::vector<const char*> source_extensions;
            for (const char* name : required_exts) {
                if (HasDeviceExtension(device_exts, name)) source_extensions.push_back(name);
            }
            if (!source_instance && results.portability_subset_advertised) {
                source_extensions.push_back(portability_subset);
            }
            VkResult source_create_result = VK_ERROR_EXTENSION_NOT_PRESENT;
            if (results.source_required_extensions_present) {
                const VkDeviceCreateInfo source_info{
                    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                    .pNext = &source_features,
                    .queueCreateInfoCount = 1,
                    .pQueueCreateInfos = &queue_info,
                    .enabledExtensionCount = static_cast<uint32_t>(source_extensions.size()),
                    .ppEnabledExtensionNames = source_extensions.data(),
                };
                source_create_result = vkCreateDevice(physical, &source_info, nullptr, &device);
            }
            results.source_required_device_create_succeeded = source_create_result == VK_SUCCESS;
            if (source_create_result != VK_SUCCESS) {
                results.source_device_create_error = !results.source_required_extensions_present
                    ? "one or more pinned source device extensions are missing"
                    : !results.source_required_features_present
                        ? "vkCreateDevice rejected the pinned required feature set: " + ResultName(source_create_result)
                        : ResultName(source_create_result);
            }

            // A capability-only diagnostic device permits independent queue, shader,
            // storage-buffer, and image tests when the renderer's stronger robustness
            // contract is missing. It is never counted as a renderer-compatible device.
            if (!device) {
                std::vector<const char*> diagnostic_extensions;
                for (const char* name : required_exts) {
                    if (HasDeviceExtension(device_exts, name)) diagnostic_extensions.push_back(name);
                }
                if (results.portability_subset_advertised) {
                    diagnostic_extensions.push_back(portability_subset);
                }
                VkPhysicalDeviceRobustness2FeaturesEXT diagnostic_robustness{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                    .robustBufferAccess2 = robustness.robustBufferAccess2,
                    .robustImageAccess2 = robustness.robustImageAccess2,
                    .nullDescriptor = robustness.nullDescriptor,
                };
                VkPhysicalDeviceVertexAttributeDivisorFeatures diagnostic_divisor{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES,
                    .vertexAttributeInstanceRateDivisor = divisor.vertexAttributeInstanceRateDivisor,
                };
                VkPhysicalDeviceFeatures2 diagnostic_features{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                };
                void** tail = &diagnostic_features.pNext;
                if (results.extensions[VK_EXT_ROBUSTNESS_2_EXTENSION_NAME]) {
                    *tail = &diagnostic_robustness;
                    tail = &diagnostic_robustness.pNext;
                }
                if (results.extensions[VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME]) {
                    *tail = &diagnostic_divisor;
                }
                const VkDeviceCreateInfo diagnostic_info{
                    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                    .pNext = &diagnostic_features,
                    .queueCreateInfoCount = 1,
                    .pQueueCreateInfos = &queue_info,
                    .enabledExtensionCount = static_cast<uint32_t>(diagnostic_extensions.size()),
                    .ppEnabledExtensionNames = diagnostic_extensions.data(),
                };
                const VkResult diagnostic_result = vkCreateDevice(
                    physical, &diagnostic_info, nullptr, &device);
                if (diagnostic_result != VK_SUCCESS) {
                    results.error = "diagnostic vkCreateDevice: " + ResultName(diagnostic_result);
                    results.stage = "device-create";
                    goto done;
                }
            }
            results.device_created = true;
            vkGetDeviceQueue(device, results.queue_family, 0, &queue);
        }
        {
            std::string workload_error;
            results.swapchain_image_presented = RunSwapchain(physical, device, surface,
                queue, results.queue_family, results, workload_error);
            SetWorkload(results, "acquire_submit_present", results.swapchain_image_presented,
                workload_error);
        }
        {
            std::string workload_error;
            results.push_descriptor_compute_passed = RunPushDescriptorCompute(
                physical, device, queue, results.queue_family, workload_error);
            SetWorkload(results, "push_descriptor_compute_storage_image_host_readback",
                results.push_descriptor_compute_passed, workload_error);
        }
        {
            std::string workload_error;
            bool unused_view_reinterpretation = false;
            results.offscreen_clear_readback_passed = RunImageReadback(physical, device,
                queue, results.queue_family, false, unused_view_reinterpretation,
                results.first_pixel, workload_error);
            SetWorkload(results, "offscreen_clear_barrier_copy_fence_readback",
                results.offscreen_clear_readback_passed, workload_error);
        }
        {
            uint32_t alias_pixel[4]{};
            std::string workload_error;
            results.image_alias_readback_passed = RunImageReadback(physical, device,
                queue, results.queue_family, true, results.view_reinterpretation_created,
                alias_pixel, workload_error);
            SetWorkload(results, "aliased_image_clear_and_reinterpretation_readback",
                results.image_alias_readback_passed, workload_error);
        }
        results.stage = "workloads-complete";
        exit_code = std::strcmp(Verdict(results), "PASS") == 0 ? 0 : 1;
    }

done:
    if (device) vkDestroyDevice(device, nullptr);
    if (surface && instance) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    if (window) SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    WriteJson(results, output_path);
    return exit_code;
}
