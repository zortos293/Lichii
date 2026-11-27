// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <astcenc.h>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

constexpr u32 DivCeil(u32 n, u32 d) { return (n + d - 1) / d; }

void Check(VkResult result) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Vulkan error " + std::to_string(result));
    }
}

void Check(astcenc_error result) {
    if (result != ASTCENC_SUCCESS) {
        throw std::runtime_error(astcenc_get_error_string(result));
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                     VkDebugUtilsMessageTypeFlagsEXT,
                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    ++*static_cast<u32*>(user);
    std::cerr << "Validation: " << data->pMessage << '\n';
    return VK_FALSE;
}

struct Pipeline {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
};

struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    std::span<std::uint8_t> bytes;

    void Destroy(VkDevice device) const {
        vkUnmapMemory(device, memory);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};

struct Vulkan {
    explicit Vulkan(const fs::path& shaders, bool refine_spec, u32 fused_words)
        : fused_words{fused_words} {
        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                    nullptr,
                                    "astc_bc_probe",
                                    0,
                                    nullptr,
                                    0,
                                    VK_API_VERSION_1_3};
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extensions[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                                 VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        const VkDebugUtilsMessengerCreateInfoEXT debug_info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            nullptr,
            0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            Debug,
            &errors};
        const VkValidationFeatureEnableEXT sync =
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        const VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                                 &debug_info, 1, &sync};
        const VkInstanceCreateInfo instance_info{
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &validation, 0, &app, 1, &layer, 2, extensions};
        Check(vkCreateInstance(&instance_info, nullptr, &instance));
        auto create_debug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        Check(create_debug(instance, &debug_info, nullptr, &debug));
        u32 count{};
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDevice physical{};
        VkPhysicalDeviceDriverProperties driver{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               &driver};
        for (auto candidate : devices) {
            vkGetPhysicalDeviceProperties2(candidate, &properties);
            if (driver.driverID == VK_DRIVER_ID_MESA_LLVMPIPE) {
                physical = candidate;
                break;
            }
        }
        if (!physical) {
            throw std::runtime_error("No Mesa lavapipe device found");
        }
        std::cout << "Device: " << properties.properties.deviceName << "; " << driver.driverInfo
                  << '\n';
        storage_alignment = properties.properties.limits.minStorageBufferOffsetAlignment;
        timestamp_period = properties.properties.limits.timestampPeriod;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        const auto family = std::ranges::find_if(families, [](const auto& q) {
            return (q.queueFlags & VK_QUEUE_COMPUTE_BIT) && q.timestampValidBits == 64;
        });
        if (family == families.end()) {
            throw std::runtime_error("No compute queue with 64-bit timestamps");
        }
        const u32 queue_family = static_cast<u32>(family - families.begin());
        const float priority = 1.f;
        const VkDeviceQueueCreateInfo queue_info{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queue_family, 1, &priority};
        VkPhysicalDeviceImageRobustnessFeatures robustness{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, nullptr, VK_TRUE};
        VkPhysicalDeviceFeatures features{};
        features.robustBufferAccess = VK_TRUE;
        features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        const VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                             &robustness,
                                             0,
                                             1,
                                             &queue_info,
                                             0,
                                             nullptr,
                                             0,
                                             nullptr,
                                             &features};
        Check(vkCreateDevice(physical, &device_info, nullptr, &device));
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                                0, queue_family};
        Check(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
        const VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
                                               VK_QUERY_TYPE_TIMESTAMP, 4};
        Check(vkCreateQueryPool(device, &query_info, nullptr, &queries));

        constexpr auto SSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        constexpr auto IMAGE = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        decode = MakePipeline(shaders / "astc_decoder.spv", {SSBO, IMAGE}, 8 * sizeof(u32));
        encode = MakePipeline(shaders / "bc_encoder.spv", {IMAGE, SSBO}, 6 * sizeof(u32));
        for (u32 kind = 0; kind < fused.size(); ++kind) {
            constexpr VkSpecializationMapEntry map[]{{0, 0, 4}, {1, 4, 4}};
            const u32 spec_data[]{kind, 0};
            const VkSpecializationInfo spec{refine_spec ? 2u : 1u, map, sizeof(spec_data),
                                            spec_data};
            fused[kind] = MakePipeline(shaders / "astc_bc.spv", {SSBO, SSBO},
                                       fused_words * sizeof(u32), &spec);
        }
    }

    ~Vulkan() {
        for (const auto& p : {decode, encode, fused[0], fused[1]}) {
            vkDestroyPipeline(device, p.pipeline, nullptr);
            vkDestroyPipelineLayout(device, p.layout, nullptr);
            vkDestroyDescriptorSetLayout(device, p.set_layout, nullptr);
        }
        vkDestroyQueryPool(device, queries, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        auto destroy_debug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy_debug(instance, debug, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    Pipeline MakePipeline(const fs::path& path, std::array<VkDescriptorType, 2> types,
                          u32 push_size, const VkSpecializationInfo* spec = nullptr) const {
        const auto size = fs::file_size(path);
        if (size % sizeof(u32) != 0) {
            throw std::runtime_error("Unaligned SPIR-V: " + path.string());
        }
        std::vector<u32> code(size / sizeof(u32));
        std::ifstream input(path, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(code.data()), size)) {
            throw std::runtime_error("Cannot read SPIR-V: " + path.string());
        }
        const VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                   nullptr, 0, size, code.data()};
        VkShaderModule module{};
        Check(vkCreateShaderModule(device, &module_info, nullptr, &module));
        const VkDescriptorSetLayoutBinding bindings[]{
            {0, types[0], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, types[1], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, bindings};
        Pipeline result{};
        Check(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &result.set_layout));
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
        const VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                     nullptr,
                                                     0,
                                                     1,
                                                     &result.set_layout,
                                                     1,
                                                     &push};
        Check(vkCreatePipelineLayout(device, &layout_info, nullptr, &result.layout));
        const VkComputePipelineCreateInfo pipeline_info{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            nullptr,
            0,
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
             VK_SHADER_STAGE_COMPUTE_BIT, module, "main", spec},
            result.layout};
        Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &result.pipeline));
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkDeviceMemory Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags) const {
        for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                                req.size, i};
                VkDeviceMemory result{};
                Check(vkAllocateMemory(device, &info, nullptr, &result));
                return result;
            }
        }
        throw std::runtime_error("No compatible Vulkan memory type");
    }

    Buffer MakeBuffer(size_t size) const {
        const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      nullptr,
                                      0,
                                      size,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VK_SHARING_MODE_EXCLUSIVE};
        Buffer result{};
        Check(vkCreateBuffer(device, &info, nullptr, &result.buffer));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, result.buffer, &req);
        result.memory = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkBindBufferMemory(device, result.buffer, result.memory, 0));
        void* ptr{};
        Check(vkMapMemory(device, result.memory, 0, size, 0, &ptr));
        result.bytes = {static_cast<std::uint8_t*>(ptr), size};
        return result;
    }

    VkDescriptorSet Set(VkDescriptorPool descriptors, const Pipeline& p,
                        const VkDescriptorBufferInfo* b0, const VkDescriptorBufferInfo* b1,
                        const VkImageView* view) const {
        const VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               nullptr, descriptors, 1, &p.set_layout};
        VkDescriptorSet result{};
        Check(vkAllocateDescriptorSets(device, &info, &result));
        const VkDescriptorImageInfo image_info{VK_NULL_HANDLE, view ? *view : VK_NULL_HANDLE,
                                               VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet writes[]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, result, 0, 0, 1,
             b0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             b0 ? nullptr : &image_info, b0},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, result, 1, 0, 1,
             b1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             b1 ? nullptr : &image_info, b1}};
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        return result;
    }

    VkInstance instance{};
    VkDebugUtilsMessengerEXT debug{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    VkQueryPool queries{};
    Pipeline decode{};
    Pipeline encode{};
    std::array<Pipeline, 2> fused{};
    VkDeviceSize storage_alignment{};
    float timestamp_period{};
    u32 errors{};
    u32 fused_words;
};

struct Level {
    u32 width;
    u32 height;
    u32 ax;
    u32 ay;
    u32 cx;
    u32 cy;
    u32 gob_height;
    u32 row_stride;
    u32 x_stride;
    u32 in_offset;
    u32 out_block;
};

using Histogram = std::map<std::string, u64>;
constexpr u32 Layers = 4;

struct Texture {
    u32 bw;
    u32 bh;
    bool srgb;
    bool native;
    std::vector<Level> levels;
    Bytes input;
    u32 layer_stride{};
    u32 blocks{};
};

u32 Swizzle(const Texture& t, const Level& l, u32 x, u32 y, u32 z) {
    constexpr u32 astc_x[]{0, 32, 256, 288};
    constexpr u32 astc_y[]{0, 16, 64, 80, 128, 144, 192, 208};
    u32 in_gob = astc_x[x % 4] + astc_y[y % 8];
    if (t.native) {
        in_gob = (in_gob & ~96u) | ((in_gob & 32u) << 1) | ((in_gob & 64u) >> 1);
    }
    const u32 column = (x / 4) * l.x_stride;
    const u32 gob_y = y / 8;
    return l.in_offset + z * t.layer_stride + (gob_y >> l.gob_height) * l.row_stride + column +
           (gob_y % (1u << l.gob_height)) * 512 + in_gob;
}

u32 Noise(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}

Bytes Pixels(const Texture& t, const Level& l, u32 z) {
    Bytes data(l.width * l.height * 4);
    for (u32 y = 0; y < l.height; ++y) {
        for (u32 x = 0; x < l.width; ++x) {
            const u32 noise = Noise(x + y * 4099 + l.width * 131 + l.height * 8191);
            const u32 cell = Noise(x / 4 + y / 3 * 4099);
            const std::array<std::array<u32, 4>, Layers> colors{{
                {x * 7 + y * 3, x * 3 + y * 9, x * 13 - y * 5, 255},
                {noise, (noise >> 8) ^ cell, (noise >> 16) + x * 7, x * 23 + y * 11},
                {cell + x, (cell >> 8) + y, (cell >> 16) + x + y, (x + y) % 9 < 5 ? 39u : 225u},
                {53u + 17u * (x / t.bw), 247u + 23u * (y / t.bh), 80u + 5u * (x / t.bw),
                 25u + 31u * (y / t.bh)},
            }};
            for (u32 c = 0; c < 4; ++c) {
                data[(y * l.width + x) * 4 + c] = static_cast<std::uint8_t>(colors[z][c]);
            }
        }
    }
    return data;
}

enum class Corpus { Ldr, HdrCem15, UnormThreshold };

Texture MakeTexture(u32 bw, u32 bh, bool srgb, bool native, Histogram& modes,
                    Corpus corpus = Corpus::Ldr) {
    Texture t{bw, bh, srgb, native};
    const bool ldr = corpus == Corpus::Ldr;
    const bool hdr = corpus == Corpus::HdrCem15;
    const u32 width = ldr ? (native ? 129 : 64) : 13;
    const u32 height = ldr ? (native ? 109 : 48) : 9;
    for (int mip = 0; mip < std::bit_width(std::max(width, height)); ++mip) {
        const u32 w = std::max(width >> mip, 1u);
        const u32 h = std::max(height >> mip, 1u);
        const u32 ax = DivCeil(w, bw);
        const u32 ay = DivCeil(h, bh);
        const u32 cx = DivCeil(w, 4);
        const u32 cy = DivCeil(h, 4);
        const u32 gob_height = native && mip == 0 && ay > 8 ? 1u : 0u;
        const u32 x_stride = 512u << gob_height;
        const u32 row_stride = DivCeil(ax, 4) * x_stride;
        t.levels.push_back(
            {w, h, ax, ay, cx, cy, gob_height, row_stride, x_stride, t.layer_stride, t.blocks});
        t.layer_stride += DivCeil(ay, 8u << gob_height) * row_stride;
        t.blocks += cx * cy * Layers;
    }
    t.input.resize(t.layer_stride * Layers);
    astcenc_config config{};
    const auto profile = hdr ? ASTCENC_PRF_HDR : srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
    Check(astcenc_config_init(profile, bw, bh, 1, ASTCENC_PRE_MEDIUM, 0, &config));
    astcenc_context* context{};
    Check(astcenc_context_alloc(&config, 1, &context));
    constexpr astcenc_swizzle identity{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    for (const auto& l : t.levels) {
        Bytes linear(l.ax * l.ay * 16);
        for (u32 z = 0; z < Layers; ++z) {
            if (ldr) {
                Bytes pixels = Pixels(t, l, z);
                void* slices[]{pixels.data()};
                astcenc_image image{l.width, l.height, 1, ASTCENC_TYPE_U8, slices};
                Check(astcenc_compress_image(context, &image, &identity, linear.data(),
                                             linear.size(), 0));
                Check(astcenc_compress_reset(context));
            }
            for (u32 y = 0; y < l.ay; ++y) {
                for (u32 x = 0; x < l.ax; ++x) {
                    auto* block = linear.data() + (y * l.ax + x) * 16;
                    if (hdr) {
                        constexpr std::uint8_t hdr_cem15[]{
                            0x42, 0xe0, 0x01, 0, 0, 0, 0, 0, 0x70, 0x71, 0x01, 0, 0, 0, 0, 0,
                        };
                        std::ranges::copy(hdr_cem15, block);
                    } else if (!ldr) {
                        constexpr std::uint8_t void_extent[]{0xfc, 0xfd, 255, 255,
                                                             255,  255,  255, 255};
                        std::ranges::copy(void_extent, block);
                        const u32 bases[]{(x * 12 + z * 17) % 255, (y * 19 + z * 29) % 255,
                                          (x * 23 + y * 37) % 255, 127};
                        for (u32 c = 0; c < 4; ++c) {
                            const u32 value = bases[c] * 257 + 128 + ((x + y + z + c) & 1);
                            block[8 + c * 2] = value & 255;
                            block[9 + c * 2] = value >> 8;
                        }
                    }
                    astcenc_block_info info{};
                    Check(astcenc_get_block_info(context, block, &info));
                    if (ldr && info.is_constant_block && (x + y + z) % 2 == 0) {
                        for (u32 c = 0; c < 4; ++c) {
                            block[8 + c * 2] = (x + y + c) % 2 == 0 ? 0 : 255;
                        }
                        Check(astcenc_get_block_info(context, block, &info));
                    }
                    if (info.is_error_block || info.is_hdr_block != hdr) {
                        throw std::runtime_error(
                            "Corpus contains an invalid or unexpected ASTC block");
                    }
                    ++modes["blocks"];
                    if (info.is_constant_block) {
                        ++modes["void_extent"];
                    } else {
                        const u32 mode = block[0] | ((block[1] & 7u) << 8);
                        ++modes["block_mode:" + std::to_string(mode)];
                        ++modes["partitions:" + std::to_string(info.partition_count)];
                        ++modes[info.is_dual_plane_block ? "dual_plane" : "single_plane"];
                        ++modes["weight_levels:" + std::to_string(info.weight_level_count)];
                        for (u32 p = 0; p < info.partition_count; ++p) {
                            ++modes["cem:" + std::to_string(info.color_endpoint_modes[p])];
                        }
                    }
                    const auto at = Swizzle(t, l, x, y, z);
                    std::memcpy(t.input.data() + at, block, 16);
                }
            }
        }
    }
    astcenc_context_free(context);
    return t;
}

struct Output {
    Bytes previous;
    Bytes candidate;
    double previous_us;
    double candidate_us;
};

Output Transcode(const Vulkan& vk, const Texture& t, u32 kind) {
    const u32 block_bytes = kind == 1 ? 16 : 8;
    const size_t bytes = t.blocks * block_bytes;
    const auto offset = std::max<VkDeviceSize>(vk.storage_alignment, 64);
    constexpr u32 PushOffset = 2;
    constexpr size_t PushBytes = PushOffset * 8;
    const Buffer input = vk.MakeBuffer(offset + t.input.size());
    const Buffer previous = vk.MakeBuffer(offset + PushBytes + bytes + 64);
    const Buffer candidate = vk.MakeBuffer(previous.bytes.size());
    std::ranges::copy(t.input, input.bytes.begin() + offset);
    std::ranges::fill(previous.bytes, 0xa5);
    std::ranges::fill(candidate.bytes, 0x5a);
    const VkDescriptorBufferInfo previous_info{previous.buffer, offset, PushBytes + bytes};
    const VkDescriptorBufferInfo candidate_info{candidate.buffer, offset, PushBytes + bytes};

    constexpr VkFormat Format = VK_FORMAT_R8G8B8A8_UNORM;
    const u32 mip_count = static_cast<u32>(t.levels.size());
    const VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                       nullptr,
                                       0,
                                       VK_IMAGE_TYPE_2D,
                                       Format,
                                       {t.levels[0].width, t.levels[0].height, 1},
                                       mip_count,
                                       Layers,
                                       VK_SAMPLE_COUNT_1_BIT,
                                       VK_IMAGE_TILING_OPTIMAL,
                                       VK_IMAGE_USAGE_STORAGE_BIT,
                                       VK_SHARING_MODE_EXCLUSIVE};
    VkImage image{};
    Check(vkCreateImage(vk.device, &image_info, nullptr, &image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(vk.device, image, &req);
    const auto image_memory = vk.Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(vk.device, image, image_memory, 0));
    std::vector<VkImageView> views(mip_count);
    const VkDescriptorPoolSize pool_sizes[]{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mip_count * 4},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, mip_count * 2}};
    const VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, mip_count * 3, 2, pool_sizes};
    VkDescriptorPool descriptors{};
    Check(vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &descriptors));
    std::vector<std::array<VkDescriptorSet, 3>> sets(mip_count);
    for (u32 i = 0; i < mip_count; ++i) {
        const VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                              nullptr,
                                              0,
                                              image,
                                              VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                              Format,
                                              {},
                                              {VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, Layers}};
        Check(vkCreateImageView(vk.device, &view_info, nullptr, &views[i]));
        const auto in_offset = t.levels[i].in_offset;
        const VkDescriptorBufferInfo in_info{input.buffer, offset + in_offset,
                                             t.input.size() - in_offset};
        sets[i] = {vk.Set(descriptors, vk.decode, &in_info, nullptr, &views[i]),
                   vk.Set(descriptors, vk.encode, nullptr, &previous_info, &views[i]),
                   vk.Set(descriptors, vk.fused[kind], &in_info, &candidate_info, nullptr)};
    }

    const VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                            vk.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd{};
    Check(vkAllocateCommandBuffers(vk.device, &alloc, &cmd));
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    Check(vkBeginCommandBuffer(cmd, &begin));
    const auto dispatch = [&](const Pipeline& pipeline, VkDescriptorSet set,
                              std::span<const u32> pc, u32 x, u32 y) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           static_cast<u32>(pc.size_bytes()), pc.data());
        vkCmdDispatch(cmd, x, y, Layers);
    };
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, Layers};
    const VkImageMemoryBarrier prepare{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                       nullptr,
                                       0,
                                       VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_GENERAL,
                                       VK_QUEUE_FAMILY_IGNORED,
                                       VK_QUEUE_FAMILY_IGNORED,
                                       image,
                                       all};
    const VkImageMemoryBarrier read_image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                          nullptr,
                                          VK_ACCESS_SHADER_WRITE_BIT,
                                          VK_ACCESS_SHADER_READ_BIT,
                                          VK_IMAGE_LAYOUT_GENERAL,
                                          VK_IMAGE_LAYOUT_GENERAL,
                                          VK_QUEUE_FAMILY_IGNORED,
                                          VK_QUEUE_FAMILY_IGNORED,
                                          image,
                                          all};
    const VkMemoryBarrier host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    constexpr auto Compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const auto start = [&](u32 query) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk.queries, query);
    };
    const auto finish = [&](u32 query) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk.queries, query);
    };
    vkCmdResetQueryPool(cmd, vk.queries, 0, 4);
    const auto previous_pass = [&] {
        start(0);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Compute, 0, 0, nullptr, 0,
                             nullptr, 1, &prepare);
        for (u32 i = 0; i < mip_count; ++i) {
            const auto& l = t.levels[i];
            const u32 pc[]{t.bw,
                           t.bh,
                           t.layer_stride,
                           l.row_stride,
                           static_cast<u32>(std::countr_zero(l.x_stride)),
                           l.gob_height,
                           (1u << l.gob_height) - 1,
                           t.native};
            dispatch(vk.decode, sets[i][0], pc, DivCeil(l.ax, 8), DivCeil(l.ay, 8));
        }
        vkCmdPipelineBarrier(cmd, Compute, Compute, 0, 0, nullptr, 0, nullptr, 1, &read_image);
        for (u32 i = 0; i < mip_count; ++i) {
            const auto& l = t.levels[i];
            const u32 pc[]{l.cx,     l.cy, l.width,
                           l.height, kind, PushOffset + l.out_block * (block_bytes / 8)};
            dispatch(vk.encode, sets[i][1], pc, DivCeil(l.cx, 8), DivCeil(l.cy, 8));
        }
        finish(1);
    };
    const auto fused_pass = [&] {
        start(2);
        for (u32 i = 0; i < mip_count; ++i) {
            const auto& l = t.levels[i];
            u32 pc[]{l.width,
                     l.height,
                     l.ax,
                     l.ay,
                     t.srgb,
                     t.bw,
                     t.bh,
                     DivCeil(1u << 18, t.bw),
                     DivCeil(1u << 18, t.bh),
                     (1024 + t.bw / 2) / (t.bw - 1),
                     (1024 + t.bh / 2) / (t.bh - 1),
                     t.layer_stride,
                     l.row_stride,
                     static_cast<u32>(std::countr_zero(l.x_stride)),
                     l.gob_height,
                     (1u << l.gob_height) - 1,
                     t.native,
                     PushOffset + l.out_block * (block_bytes / 8)};
            if (vk.fused_words == 17) {
                std::move(pc + 5, pc + 18, pc + 4);
            }
            const bool astc_groups = ((t.bw | t.bh) & 3u) == 0;
            dispatch(vk.fused[kind], sets[i][2], std::span(pc, vk.fused_words),
                     DivCeil(astc_groups ? l.ax : l.cx, 64), astc_groups ? l.ay : l.cy);
        }
        finish(3);
    };
    const auto separate = [&] {
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 0,
                             nullptr);
    };
    if (t.native == t.srgb) {
        previous_pass();
        separate();
        fused_pass();
    } else {
        fused_pass();
        separate();
        previous_pass();
    }
    vkCmdPipelineBarrier(cmd, Compute, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0, nullptr, 0,
                         nullptr);
    Check(vkEndCommandBuffer(cmd));
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd};
    Check(vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE));
    Check(vkQueueWaitIdle(vk.queue));
    std::array<u64, 4> ticks{};
    Check(vkGetQueryPoolResults(vk.device, vk.queries, 0, ticks.size(), sizeof(ticks), ticks.data(),
                                sizeof(u64), VK_QUERY_RESULT_64_BIT));
    const auto extract = [&](const Buffer& b, u32 sentinel) {
        const auto data = b.bytes.subspan(offset + PushBytes, bytes);
        const auto in_guard = [sentinel](auto byte) { return byte == sentinel; };
        if (!std::ranges::all_of(b.bytes.first(offset + PushBytes), in_guard) ||
            !std::ranges::all_of(b.bytes.last(64), in_guard)) {
            throw std::runtime_error("Unexpected write outside BC output");
        }
        return Bytes(data.begin(), data.end());
    };
    Output result{extract(previous, 0xa5), extract(candidate, 0x5a),
                  (ticks[1] - ticks[0]) * vk.timestamp_period / 1000.0,
                  (ticks[3] - ticks[2]) * vk.timestamp_period / 1000.0};
    vkFreeCommandBuffers(vk.device, vk.pool, 1, &cmd);
    vkDestroyDescriptorPool(vk.device, descriptors, nullptr);
    for (auto view : views) {
        vkDestroyImageView(vk.device, view, nullptr);
    }
    vkDestroyImage(vk.device, image, nullptr);
    vkFreeMemory(vk.device, image_memory, nullptr);
    input.Destroy(vk.device);
    previous.Destroy(vk.device);
    candidate.Destroy(vk.device);
    if (vk.errors) {
        throw std::runtime_error("Vulkan validation reported errors");
    }
    return result;
}

u64 ReadLE(std::span<const std::uint8_t> b) {
    u64 value = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        value |= u64{b[i]} << (8 * i);
    }
    return value;
}

std::string Hex(std::span<const std::uint8_t> b) {
    std::string result;
    for (auto byte : b) {
        constexpr char digits[] = "0123456789abcdef";
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

std::array<u32, 4> BcTexel(std::span<const std::uint8_t> block, u32 i) {
    constexpr u32 Scale = 210;
    const bool alpha = block.size() == 16;
    const auto color = block.last(8);
    const auto c0 = ReadLE(color.first(2));
    const auto c1 = ReadLE(color.subspan(2, 2));
    const auto index = (ReadLE(color.last(4)) >> (2 * i)) & 3u;
    const auto rgb = [](u64 c) -> std::array<u32, 3> {
        const u32 r = (c >> 11) & 31;
        const u32 g = (c >> 5) & 63;
        const u32 b = c & 31;
        return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
    };
    const auto p = rgb(c0);
    const auto q = rgb(c1);
    const bool four = alpha || c0 > c1;
    std::array<u32, 4> rgba{};
    rgba[3] = Scale * 255;
    for (u32 c = 0; c < 3; ++c) {
        if (index <= 1) {
            rgba[c] = Scale * (index == 0 ? p[c] : q[c]);
        } else if (four) {
            rgba[c] = (Scale / 3) * (index == 2 ? 2 * p[c] + q[c] : p[c] + 2 * q[c]);
        } else if (index == 2) {
            rgba[c] = (Scale / 2) * (p[c] + q[c]);
        } else {
            rgba[3] = 0;
        }
    }
    if (alpha) {
        const u32 a = block[0];
        const u32 b = block[1];
        const u32 j = (ReadLE(block.subspan(2, 6)) >> (3 * i)) & 7u;
        if (j <= 1) {
            rgba[3] = Scale * (j == 0 ? a : b);
        } else if (a > b) {
            rgba[3] = (Scale / 7) * ((8 - j) * a + (j - 1) * b);
        } else if (j <= 5) {
            rgba[3] = (Scale / 5) * ((6 - j) * a + (j - 1) * b);
        } else {
            rgba[3] = j == 6 ? 0 : 255 * Scale;
        }
    }
    return rgba;
}

struct Stats {
    u64 blocks{};
    u64 bytes{};
    u64 changed{};
    u64 changed_bytes{};
    u64 edge{};
    u64 changed_edge{};
    u64 visible_changed{};
    u64 padding_only{};
    u64 equivalent{};
    double previous_us{};
    double candidate_us{};
    Histogram modes;
};

void Print(const Stats& s) {
    std::cout << " blocks=" << s.blocks << " bytes=" << s.bytes << " changed=" << s.changed
              << " changed_bytes=" << s.changed_bytes << " edge=" << s.edge
              << " changed_edge=" << s.changed_edge << " visible_changed=" << s.visible_changed
              << " padding_only=" << s.padding_only << " equivalent=" << s.equivalent << std::fixed
              << std::setprecision(1) << " previous_us=" << s.previous_us
              << " candidate_us=" << s.candidate_us << '\n';
}

void CountMode(Histogram& hist, const std::string& side, std::span<const std::uint8_t> block) {
    if (block.size() == 16) {
        ++hist[side + (block[0] > block[1] ? ":alpha8" : ":alpha6")];
    } else if (ReadLE(block.first(2)) > ReadLE(block.subspan(2, 2))) {
        ++hist[side + ":four_color"];
    } else {
        ++hist[side + ":three_color"];
        const auto indices = ReadLE(block.last(4));
        if (indices == 0xffffffffu) {
            ++hist[side + ":all_transparent"];
        } else if ((indices & (indices >> 1) & 0x55555555u) != 0) {
            ++hist[side + ":partly_transparent"];
        }
    }
}

Stats Compare(const Texture& t, u32 kind, const Output& out, const std::string& name) {
    Stats s{.blocks = t.blocks,
            .bytes = out.previous.size(),
            .previous_us = out.previous_us,
            .candidate_us = out.candidate_us};
    const u32 size = kind == 1 ? 16 : 8;
    for (u32 mip = 0; mip < t.levels.size(); ++mip) {
        const auto& l = t.levels[mip];
        for (u32 z = 0; z < Layers; ++z) {
            for (u32 y = 0; y < l.cy; ++y) {
                for (u32 x = 0; x < l.cx; ++x) {
                    const size_t offset = size * (l.out_block + (z * l.cy + y) * l.cx + x);
                    const auto prev = std::span(out.previous).subspan(offset, size);
                    const auto next = std::span(out.candidate).subspan(offset, size);
                    CountMode(s.modes, "previous", prev);
                    CountMode(s.modes, "candidate", next);
                    const bool edge = x * 4 + 4 > l.width || y * 4 + 4 > l.height;
                    s.edge += edge;
                    if (std::ranges::equal(prev, next)) {
                        continue;
                    }
                    s.changed_edge += edge;
                    bool visible = false;
                    bool padding = false;
                    for (u32 i = 0; i < 16; ++i) {
                        if (BcTexel(prev, i) != BcTexel(next, i)) {
                            const bool inside = x * 4 + i % 4 < l.width && y * 4 + i / 4 < l.height;
                            visible |= inside;
                            padding |= !inside;
                        }
                    }
                    s.visible_changed += visible;
                    s.padding_only += !visible && padding;
                    s.equivalent += !visible && !padding;
                    for (u32 n = 0; n < size; ++n) {
                        s.changed_bytes += prev[n] != next[n];
                    }
                    if (s.changed++ == 0) {
                        std::cout << "first " << name << " mip=" << mip << " extent=" << l.width
                                  << 'x' << l.height << " layer=" << z << " block=" << x << ',' << y
                                  << " edge=" << edge << " visible=" << visible
                                  << " padding=" << padding << " previous=" << Hex(prev)
                                  << " candidate=" << Hex(next) << " astc=";
                        for (u32 ay = y * 4 / t.bh; ay <= std::min(y * 4 + 3, l.height - 1) / t.bh;
                             ++ay) {
                            for (u32 ax = x * 4 / t.bw;
                                 ax <= std::min(x * 4 + 3, l.width - 1) / t.bw; ++ax) {
                                std::cout << ' '
                                          << Hex(std::span(t.input).subspan(
                                                 Swizzle(t, l, ax, ay, z), 16));
                            }
                        }
                        std::cout << '\n';
                    }
                }
            }
        }
    }
    std::cout << "case " << name;
    Print(s);
    return s;
}

void Accumulate(Stats& a, const Stats& b) {
    a.blocks += b.blocks;
    a.bytes += b.bytes;
    a.changed += b.changed;
    a.changed_bytes += b.changed_bytes;
    a.edge += b.edge;
    a.changed_edge += b.changed_edge;
    a.visible_changed += b.visible_changed;
    a.padding_only += b.padding_only;
    a.equivalent += b.equivalent;
    a.previous_us += b.previous_us;
    a.candidate_us += b.candidate_us;
    for (const auto& [key, count] : b.modes) {
        a.modes[key] += count;
    }
}

int main(int argc, char** argv) try {
    if (argc < 6) {
        throw std::runtime_error("usage: astc_bc_probe <shaders> <refine 0/1> <push words 17/18> "
                                 "<extended 0/1> <WxH>...");
    }
    const u32 push_words = std::stoul(argv[3]);
    if (push_words != 17 && push_words != 18) {
        throw std::runtime_error("Unknown fused push layout");
    }
    std::map<std::string, Stats> totals;
    std::map<std::string, Histogram> astc_modes;
    std::ofstream previous(fs::path(argv[1]) / "previous.bc", std::ios::binary);
    std::ofstream candidate(fs::path(argv[1]) / "candidate.bc", std::ios::binary);
    if (!previous || !candidate) {
        throw std::runtime_error("Cannot open BC output files");
    }
    {
        const Vulkan vk(argv[1], std::string(argv[2]) == "1", push_words);
        const auto compare = [&](const Texture& t, u32 kind, const std::string& name) {
            const auto out = Transcode(vk, t, kind);
            previous.write(reinterpret_cast<const char*>(out.previous.data()), out.previous.size());
            candidate.write(reinterpret_cast<const char*>(out.candidate.data()),
                            out.candidate.size());
            return Compare(t, kind, out, name);
        };
        for (int i = 5; i < argc; ++i) {
            u32 bw{}, bh{};
            if (std::sscanf(argv[i], "%ux%u", &bw, &bh) != 2 || bw < 4 || bh < 4) {
                throw std::runtime_error("Invalid footprint " + std::string(argv[i]));
            }
            for (bool srgb : {false, true}) {
                const std::string profile = srgb ? "srgb" : "linear";
                for (bool native : {false, true}) {
                    const auto t = MakeTexture(bw, bh, srgb, native, astc_modes[profile]);
                    for (u32 kind : {1, 0}) {
                        if (i == 5 && !srgb && !native) {
                            Transcode(vk, t, kind);
                        }
                        const std::string mode = kind == 1 ? "bc3" : "bc1";
                        const std::string name = mode + '/' + profile + '/' + argv[i] + '/' +
                                                 (native ? "native-129x109" : "legacy-64x48");
                        const auto s = compare(t, kind, name);
                        Accumulate(totals[mode + '/' + profile], s);
                        Accumulate(totals["all"], s);
                    }
                }
            }
        }
        if (std::string(argv[4]) == "1") {
            for (const Corpus corpus : {Corpus::HdrCem15, Corpus::UnormThreshold}) {
                const std::string profile =
                    corpus == Corpus::HdrCem15 ? "hdr-cem15" : "unorm-threshold";
                const auto t = MakeTexture(4, 4, false, true, astc_modes[profile], corpus);
                for (const u32 kind : {1, 0}) {
                    const std::string mode = kind == 1 ? "bc3" : "bc1";
                    const auto name = mode + '/' + profile;
                    Accumulate(totals[name], compare(t, kind, name));
                }
            }
        }
    }
    previous.close();
    candidate.close();
    if (!previous || !candidate) {
        throw std::runtime_error("Cannot write BC output files");
    }
    for (const auto& [mode, stats] : totals) {
        std::cout << "total " << mode;
        Print(stats);
        for (const auto& [key, count] : stats.modes) {
            std::cout << "bc_mode " << mode << ' ' << key << '=' << count << '\n';
        }
    }
    for (const auto& [profile, hist] : astc_modes) {
        for (const auto& [key, count] : hist) {
            std::cout << "astc_mode " << profile << ' ' << key << '=' << count << '\n';
        }
    }
    return std::ranges::any_of(totals, [](const auto& entry) { return entry.second.changed != 0; })
               ? 1
               : 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 2;
}
