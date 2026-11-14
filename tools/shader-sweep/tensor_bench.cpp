// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game-free Vulkan runner for production-emitted tensor shaders and independent CPU fixtures.
// Pipeline creation uses host wall time; dispatch durations use GPU timestamps. Driver caches
// are inherited and may be updated normally, but the tool never clears them.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <spirv/unified1/spirv.hpp11>

#include "common/cityhash.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"

#include "tensor_bench_shader.h"

namespace {

using Json = nlohmann::json;
namespace TB = TensorBench;

constexpr int ExitPass = 0;
constexpr int ExitFailure = 1;
constexpr int ExitUsage = 2;
constexpr int ExitSkipped = 77;

/// The guest warp size mirrored from Vulkan::GuestWarpSize (vulkan_device.h).
constexpr std::uint32_t kWarpSize = 32;

/// Per the shader contract, each invocation holds eight input words (A[4], B[2], C[2]) before
/// the two output words D[2] at offsets 8 and 9 of the ten-word stride.
constexpr std::uint32_t kInputWordsPerInvocation = 8;

/// Upper bound for GPU waits so a wedged submission cannot hang the tool forever.
constexpr std::uint64_t kFenceTimeoutNs = 10'000'000'000;

void LogLine(std::string_view message) {
    fmt::print(stderr, "tensor-bench: {}\n", message);
}

/// CLI misuse: reported as JSON status "error" with exit code 2.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string Join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            joined += separator;
        }
        joined += parts[i];
    }
    return joined;
}

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if (values.size() % 2 != 0) {
        return values[middle];
    }
    return (values[middle - 1] + values[middle]) / 2.0;
}

void LogPipelineBuildTimes(const std::vector<double>& samples) {
    LogLine(fmt::format("pipeline: {} build(s), first {:.3f} ms (wall-clock, cache inherited)",
                        samples.size(), samples.front()));
    if (samples.size() > 1) {
        const double mean =
            std::accumulate(samples.begin() + 1, samples.end(), 0.0) / (samples.size() - 1);
        LogLine(fmt::format("subsequent pipeline creations: mean {:.3f} ms", mean));
    }
}

// -----------------------------------------------------------------------------
// Options and CLI parsing
// -----------------------------------------------------------------------------

struct Options {
    std::optional<std::uint32_t> device;
    bool list_devices{false};
    TB::Mode mode{TB::Mode::Software};
    TB::Pattern pattern{TB::Pattern::Integers};
    std::uint32_t depth{16};
    std::uint32_t operations{1};
    std::uint32_t local_size{64};
    std::uint32_t workgroups{64};
    std::uint32_t iterations{16};
    std::uint32_t compile_repeats{3};
    std::uint32_t seed{1};
    bool validation{false};
    std::string emit_spirv;
    bool emit_only{false};
    /// Non-empty selects --compile-pipeline replay: compile-only, no fixture, no dispatch.
    std::string compile_pipeline;
    std::string pipeline_cache;
    /// Synthetic/emulation flags that were explicitly given and conflict with
    /// --compile-pipeline; rejected after parsing instead of silently ignored.
    std::vector<std::string> pipeline_conflicts;
};

std::string_view ModeName(TB::Mode mode) {
    return mode == TB::Mode::Cooperative ? "cooperative" : "software";
}

std::string_view PatternName(TB::Pattern pattern) {
    return pattern == TB::Pattern::Fractions ? "fractions" : "integers";
}

std::uint32_t ParseUint32(const std::string& text, const char* name, std::uint32_t lo,
                          std::uint32_t hi) {
    if (text.empty()) {
        throw UsageError(fmt::format("--{} requires a value", name));
    }
    std::string digits = text;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        digits = text.substr(2);
    }
    errno = 0;
    char* end{};
    const unsigned long long parsed = std::strtoull(digits.c_str(), &end, base);
    if (errno == ERANGE || end != digits.c_str() + digits.size() || end == digits.c_str()) {
        throw UsageError(fmt::format("--{}: '{}' is not a valid unsigned integer", name, text));
    }
    if (parsed < lo || parsed > hi) {
        throw UsageError(fmt::format("--{}: {} outside valid range {}..{}", name, text, lo, hi));
    }
    return static_cast<std::uint32_t>(parsed);
}

std::uint32_t ParseEnumUint32(const std::string& text, const char* name,
                              std::initializer_list<std::uint32_t> allowed) {
    const std::uint32_t value = ParseUint32(text, name, 0, std::numeric_limits<std::uint32_t>::max());
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
        std::vector<std::string> names;
        for (const std::uint32_t candidate : allowed) {
            names.push_back(std::to_string(candidate));
        }
        throw UsageError(fmt::format("--{}: {} outside valid set ({})", name, text,
                                     Join(names, ", ")));
    }
    return value;
}

Options ParseOptions(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    Options options;
    for (size_t i = 0; i < args.size();) {
        std::string name = args[i];
        std::optional<std::string> inline_value;
        if (name.rfind("--", 0) == 0) {
            if (const size_t equals = name.find('='); equals != std::string::npos) {
                inline_value = name.substr(equals + 1);
                name = name.substr(0, equals);
            }
        }
        ++i;
        const auto value = [&](const char* flag) -> std::string {
            if (inline_value) {
                return *inline_value;
            }
            if (i >= args.size()) {
                throw UsageError(fmt::format("--{} requires a value", flag));
            }
            return args[i++];
        };
        const auto flag_value = [&](const char* flag) {
            if (inline_value) {
                throw UsageError(fmt::format("--{} does not take a value", flag));
            }
        };
        if (name == "--help" || name == "-h") {
            // Handled before parsing; accepted here so it never reads as an unknown flag.
        } else if (name == "--list-devices") {
            flag_value("list-devices");
            options.pipeline_conflicts.push_back(name);
            options.list_devices = true;
        } else if (name == "--device") {
            options.device = ParseUint32(value("device"), "device", 0,
                                         std::numeric_limits<std::uint32_t>::max());
        } else if (name == "--mode") {
            const std::string mode = value("mode");
            options.pipeline_conflicts.push_back(name);
            if (mode == "software") {
                options.mode = TB::Mode::Software;
            } else if (mode == "cooperative") {
                options.mode = TB::Mode::Cooperative;
            } else {
                throw UsageError(fmt::format("--mode: '{}' is not software or cooperative", mode));
            }
        } else if (name == "--pattern") {
            const std::string pattern = value("pattern");
            options.pipeline_conflicts.push_back(name);
            if (pattern == "integers") {
                options.pattern = TB::Pattern::Integers;
            } else if (pattern == "fractions") {
                options.pattern = TB::Pattern::Fractions;
            } else {
                throw UsageError(fmt::format("--pattern: '{}' is not integers or fractions", pattern));
            }
        } else if (name == "--depth") {
            options.pipeline_conflicts.push_back(name);
            options.depth = ParseEnumUint32(value("depth"), "depth", {8, 16});
        } else if (name == "--operations") {
            options.pipeline_conflicts.push_back(name);
            options.operations = ParseUint32(value("operations"), "operations", 1, 64);
        } else if (name == "--local-size") {
            options.pipeline_conflicts.push_back(name);
            options.local_size = ParseEnumUint32(value("local-size"), "local-size", {32, 64});
        } else if (name == "--workgroups") {
            options.pipeline_conflicts.push_back(name);
            options.workgroups = ParseUint32(value("workgroups"), "workgroups", 1, 4096);
        } else if (name == "--iterations") {
            options.pipeline_conflicts.push_back(name);
            options.iterations = ParseUint32(value("iterations"), "iterations", 1, 1000);
        } else if (name == "--compile-repeats") {
            options.compile_repeats =
                ParseUint32(value("compile-repeats"), "compile-repeats", 1, 20);
        } else if (name == "--seed") {
            options.pipeline_conflicts.push_back(name);
            options.seed = ParseUint32(value("seed"), "seed", 0,
                                       std::numeric_limits<std::uint32_t>::max());
        } else if (name == "--validation") {
            flag_value("validation");
            options.validation = true;
        } else if (name == "--emit-spirv") {
            options.pipeline_conflicts.push_back(name);
            options.emit_spirv = value("emit-spirv");
        } else if (name == "--emit-only") {
            flag_value("emit-only");
            options.pipeline_conflicts.push_back(name);
            options.emit_only = true;
        } else if (name == "--compile-pipeline") {
            options.compile_pipeline = value("compile-pipeline");
            if (options.compile_pipeline.empty()) {
                throw UsageError("--compile-pipeline requires a path to a replay JSON");
            }
            if (!options.pipeline_conflicts.empty()) {
                throw UsageError(fmt::format(
                    "--compile-pipeline replays one fixed module and cannot be combined with {}",
                    Join(options.pipeline_conflicts, ", ")));
            }
        } else if (name == "--pipeline-cache") {
            options.pipeline_cache = value("pipeline-cache");
            if (options.pipeline_cache.empty()) {
                throw UsageError("--pipeline-cache requires a file path");
            }
        } else {
            throw UsageError(fmt::format("unknown argument '{}' (see --help)", name));
        }
    }
    if (!options.compile_pipeline.empty() && !options.pipeline_conflicts.empty()) {
        // A flag can also precede --compile-pipeline on the command line.
        throw UsageError(fmt::format(
            "--compile-pipeline replays one fixed module and cannot be combined with {}",
            Join(options.pipeline_conflicts, ", ")));
    }
    if (!options.pipeline_cache.empty() && options.compile_pipeline.empty()) {
        throw UsageError("--pipeline-cache requires --compile-pipeline");
    }
    if (options.emit_only) {
        if (options.device || options.list_devices || options.validation) {
            throw UsageError("--emit-only runs without a Vulkan instance: --device, "
                             "--list-devices and --validation are not allowed");
        }
        if (options.emit_spirv.empty()) {
            throw UsageError("--emit-only requires --emit-spirv PATH");
        }
    }
    return options;
}

void PrintHelp() {
    fmt::print(stdout,
               "tensor-bench — Vulkan compute benchmark for Lichii tensor shaders\n"
               "\n"
               "Usage: tensor-bench [options]\n"
               "\n"
               "Flags:\n"
               "  --help                Show this help and exit.\n"
               "  --list-devices        Enumerate Vulkan physical devices as JSON and exit.\n"
               "  --device N            Physical device index (see --list-devices). Default:\n"
               "                        the first AMD discrete GPU; if none exists, the tool\n"
               "                        exits with an error asking for an explicit --device\n"
               "                        (software rasterizers are never auto-selected).\n"
               "  --mode M              software | cooperative (default software)\n"
               "  --pattern P           integers | fractions (default integers)\n"
               "  --depth D             8 | 16 (default 16)\n"
               "  --operations N        1..64 (default 1)\n"
               "  --local-size N        32 | 64 (default 64)\n"
               "  --workgroups N        1..4096 (default 64)\n"
               "  --iterations N        1..1000 (default 16)\n"
               "  --compile-repeats N   1..20 (default 3)\n"
               "  --seed N              uint32 fixture seed (default 1)\n"
               "  --validation          Enable VK_LAYER_KHRONOS_validation; validation errors\n"
               "                        fail the run.\n"
               "  --compile-pipeline PATH\n"
               "                       Compile-only replay of a shader-sweep --pipeline\n"
               "                       replay.json: validates context, module bytes and\n"
               "                       hash, then creates the compute pipeline\n"
               "                       --compile-repeats times. No fixture buffers, no\n"
               "                       queue submission, no dispatch, no correctness\n"
               "                       claim. Allows --device, --compile-repeats,\n"
               "                       --validation and --pipeline-cache.\n"
               "  --pipeline-cache PATH Load/save a raw application VkPipelineCache blob;\n"
               "                        compile-pipeline only. Default: no application cache.\n"
               "  --emit-spirv PATH     Write the SPIR-V module to PATH.\n"
               "  --emit-only           Emit the module and JSON metadata without creating a\n"
               "                        Vulkan instance or touching a device (requires\n"
               "                        --emit-spirv).\n"
               "\n"
               "Exit codes: 0 pass, 1 fail, 2 usage/selection error, 77 unsupported (skipped).\n"
               "One JSON document goes to stdout; logs go to stderr.\n");
}

// -----------------------------------------------------------------------------
// Vulkan helpers
// -----------------------------------------------------------------------------

/// Scoped Vulkan handle: destroys in ~Scoped (or on move-assign) even when exceptions unwind,
/// keeping teardown in reverse dependency order.
template <typename Handle, typename Deleter>
class Scoped {
public:
    Scoped() = default;
    Scoped(Handle handle, Deleter deleter) : handle_{handle}, deleter_{std::move(deleter)} {}
    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;
    Scoped(Scoped&& other) noexcept
        : handle_{std::exchange(other.handle_, VK_NULL_HANDLE)},
          deleter_{std::move(other.deleter_)} {}
    Scoped& operator=(Scoped&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
            deleter_ = std::move(other.deleter_);
        }
        return *this;
    }
    ~Scoped() {
        Reset();
    }

    void Reset() {
        if (handle_ != VK_NULL_HANDLE) {
            deleter_(handle_);
            handle_ = VK_NULL_HANDLE;
        }
    }
    Handle Get() const {
        return handle_;
    }
    Handle operator*() const {
        return handle_;
    }
    explicit operator bool() const {
        return handle_ != VK_NULL_HANDLE;
    }

private:
    Handle handle_ = VK_NULL_HANDLE;
    Deleter deleter_{};
};

const char* ToString(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_VALIDATION_FAILED_EXT:
        return "VK_ERROR_VALIDATION_FAILED_EXT";
    case VK_ERROR_UNKNOWN:
        return "VK_ERROR_UNKNOWN";
    default:
        return "unknown VkResult";
    }
}

void CheckVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(fmt::format("{} failed: {}", what, ToString(result)));
    }
}

bool ContainsExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

bool ContainsLayer(const std::vector<VkLayerProperties>& layers, const char* name) {
    return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, name) == 0;
    });
}

std::vector<VkLayerProperties> EnumerateInstanceLayers() {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
        return {};
    }
    std::vector<VkLayerProperties> layers(count);
    CheckVk(vkEnumerateInstanceLayerProperties(&count, layers.data()),
            "vkEnumerateInstanceLayerProperties");
    return layers;
}

std::vector<VkExtensionProperties> EnumerateInstanceExtensions(const char* layer = nullptr) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        return {};
    }
    std::vector<VkExtensionProperties> extensions(count);
    CheckVk(vkEnumerateInstanceExtensionProperties(layer, &count, extensions.data()),
            "vkEnumerateInstanceExtensionProperties");
    return extensions;
}

std::vector<VkExtensionProperties> EnumerateDeviceExtensions(VkPhysicalDevice physical) {
    std::uint32_t count = 0;
    CheckVk(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr),
            "vkEnumerateDeviceExtensionProperties");
    if (count == 0) {
        return {};
    }
    std::vector<VkExtensionProperties> extensions(count);
    CheckVk(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()),
            "vkEnumerateDeviceExtensionProperties");
    return extensions;
}

std::vector<VkPhysicalDevice> EnumeratePhysicalDevices(VkInstance instance) {
    std::uint32_t count = 0;
    CheckVk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (count == 0) {
        return {};
    }
    std::vector<VkPhysicalDevice> devices(count);
    CheckVk(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
            "vkEnumeratePhysicalDevices");
    return devices;
}

std::string ApiVersionString(std::uint32_t version) {
    return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

std::string_view DeviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    default:
        return "other";
    }
}

std::string_view VendorName(std::uint32_t vendor_id) {
    switch (vendor_id) {
    case 0x1002:
        return "AMD";
    case 0x10DE:
        return "NVIDIA";
    case 0x8086:
        return "Intel";
    case 0x13B5:
        return "ARM";
    case 0x5143:
        return "Qualcomm";
    case 0x1010:
        return "Imagination";
    case 0x10005:
        return "Mesa";
    default:
        return {};
    }
}

std::vector<std::string> SubgroupOperationNames(std::uint32_t mask) {
    std::vector<std::string> names;
    const auto add = [&](std::uint32_t bit, std::string_view name) {
        if ((mask & bit) != 0) {
            names.emplace_back(name);
        }
    };
    add(VK_SUBGROUP_FEATURE_BASIC_BIT, "basic");
    add(VK_SUBGROUP_FEATURE_VOTE_BIT, "vote");
    add(VK_SUBGROUP_FEATURE_ARITHMETIC_BIT, "arithmetic");
    add(VK_SUBGROUP_FEATURE_BALLOT_BIT, "ballot");
    add(VK_SUBGROUP_FEATURE_SHUFFLE_BIT, "shuffle");
    add(VK_SUBGROUP_FEATURE_QUAD_BIT, "quad");
    add(VK_SUBGROUP_FEATURE_PARTITIONED_BIT_NV, "partitioned");
    return names;
}

// -----------------------------------------------------------------------------
// Physical device information
// -----------------------------------------------------------------------------

struct PhysicalInfo {
    VkPhysicalDevice handle{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures core_features{};
    VkPhysicalDeviceDriverProperties driver{};
    VkPhysicalDeviceSubgroupProperties subgroup{};
    VkPhysicalDeviceSubgroupSizeControlProperties subgroup_size_control{};
    VkPhysicalDeviceShaderIntegerDotProductProperties integer_dot_product{};
    VkPhysicalDeviceCooperativeMatrixPropertiesKHR cooperative_matrix{};
    std::vector<VkExtensionProperties> extensions;
    VkPhysicalDeviceShaderFloat16Int8Features float16{};
    VkPhysicalDeviceVulkanMemoryModelFeatures memory_model{};
    VkPhysicalDevice16BitStorageFeatures storage16{};
    VkPhysicalDeviceVariablePointersFeatures variable_pointers{};
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup_size_control_features{};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative_matrix_features{};
    std::vector<VkQueueFamilyProperties> queue_families;
    std::uint32_t compute_family = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t compute_timestamp_valid_bits{};

    bool HasExtension(const char* name) const {
        return ContainsExtension(extensions, name);
    }
};

/// Collects properties/features for one physical device. Extension feature structs are only
/// chained into the pNext lists when the device (or its API version) supports them, so the
/// queries stay valid on every device.
PhysicalInfo CollectPhysicalInfo(VkPhysicalDevice physical) {
    PhysicalInfo info;
    info.handle = physical;
    vkGetPhysicalDeviceProperties(physical, &info.properties);
    info.extensions = EnumerateDeviceExtensions(physical);
    const bool core12 = info.properties.apiVersion >= VK_API_VERSION_1_2;
    const bool core11 = info.properties.apiVersion >= VK_API_VERSION_1_1;
    const bool has_cooperative_matrix = ContainsExtension(
        info.extensions, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    const bool has_subgroup_size_control = ContainsExtension(
        info.extensions, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    const bool has_integer_dot_product = ContainsExtension(
        info.extensions, VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);

    info.driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    info.subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    info.subgroup_size_control.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
    info.integer_dot_product.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES;
    info.cooperative_matrix.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR;

    void* property_next = nullptr;
    const auto push_property = [&](auto& structure) {
        structure.pNext = property_next;
        property_next = &structure;
    };
    if (has_cooperative_matrix) {
        push_property(info.cooperative_matrix);
    }
    if (has_integer_dot_product) {
        push_property(info.integer_dot_product);
    }
    if (has_subgroup_size_control) {
        push_property(info.subgroup_size_control);
    }
    if (core12) {
        push_property(info.driver);
    }
    if (core11) {
        push_property(info.subgroup);
    }
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = property_next;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    info.properties = properties2.properties;

    info.float16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    info.memory_model.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
    info.storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    info.variable_pointers.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES;
    info.subgroup_size_control_features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
    info.cooperative_matrix_features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;

    void* feature_next = nullptr;
    const auto push_feature = [&](auto& structure) {
        structure.pNext = feature_next;
        feature_next = &structure;
    };
    if (has_cooperative_matrix) {
        push_feature(info.cooperative_matrix_features);
    }
    if (has_subgroup_size_control) {
        push_feature(info.subgroup_size_control_features);
    }
    if (core12) {
        push_feature(info.memory_model);
        push_feature(info.float16);
        push_feature(info.storage16);
        push_feature(info.variable_pointers);
    }
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = feature_next;
    vkGetPhysicalDeviceFeatures2(physical, &features2);
    info.core_features = features2.features;

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    info.queue_families.resize(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count,
                                             info.queue_families.data());
    // Prefer a dedicated compute family; otherwise take the first compute-capable family.
    std::uint32_t first_compute = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t dedicated_compute = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t i = 0; i < info.queue_families.size(); ++i) {
        const VkQueueFlags flags = info.queue_families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) == 0) {
            continue;
        }
        if (first_compute == std::numeric_limits<std::uint32_t>::max()) {
            first_compute = i;
        }
        if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
            dedicated_compute == std::numeric_limits<std::uint32_t>::max()) {
            dedicated_compute = i;
        }
    }
    info.compute_family =
        dedicated_compute != std::numeric_limits<std::uint32_t>::max() ? dedicated_compute
                                                                       : first_compute;
    if (info.compute_family != std::numeric_limits<std::uint32_t>::max()) {
        info.compute_timestamp_valid_bits =
            info.queue_families[info.compute_family].timestampValidBits;
    }
    return info;
}

/// Queries the cooperative matrix shapes through the dynamically loaded entry point. The
/// entry point is only requested when the physical device advertises the extension, matching
/// the production rule that the loader may expose it for devices that do not support it.
bool HasTensorShape16x16x16(VkInstance instance, const PhysicalInfo& info) {
    if (!info.HasExtension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) {
        return false;
    }
    const auto enumerate =
        reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
    if (enumerate == nullptr) {
        return false;
    }
    std::uint32_t count = 0;
    if (enumerate(info.handle, &count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }
    std::vector<VkCooperativeMatrixPropertiesKHR> shapes(
        count, VkCooperativeMatrixPropertiesKHR{
                   VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
    if (enumerate(info.handle, &count, shapes.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(shapes.begin(), shapes.end(), [](const VkCooperativeMatrixPropertiesKHR& s) {
        return s.MSize == 16 && s.NSize == 16 && s.KSize == 16 &&
               s.AType == VK_COMPONENT_TYPE_FLOAT16_KHR && s.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
               s.CType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
               s.ResultType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
               s.saturatingAccumulation == VK_FALSE && s.scope == VK_SCOPE_SUBGROUP_KHR;
    });
}

/// Whether the compute stage can be launched with an explicitly required 32-lane subgroup
/// (VK_EXT_subgroup_size_control: requiredSubgroupSizeStages includes compute and 32 lies
/// within [minSubgroupSize, maxSubgroupSize]).
bool CanForceWarpSizeSubgroup(const PhysicalInfo& info) {
    return info.HasExtension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) &&
           info.subgroup_size_control_features.subgroupSizeControl &&
           info.subgroup_size_control_features.computeFullSubgroups &&
           (info.subgroup_size_control.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) !=
               0 &&
           info.subgroup_size_control.minSubgroupSize <= kWarpSize &&
           info.subgroup_size_control.maxSubgroupSize >= kWarpSize;
}

/// Capability gates for the requested case. Returns every failed requirement; an empty
/// vector means the device can run the case. Software mode requires Vulkan 1.2 with
/// compute-stage subgroup shuffle; cooperative mode additionally mirrors the production
/// cooperative-matrix gates (including the proprietary AMD emulated-matrix guard).
std::vector<std::string> SkipReasons(VkInstance instance, const PhysicalInfo& info,
                                     const Options& options,
                                     const TB::ShaderModule& shader) {
    std::vector<std::string> reasons;
    const VkPhysicalDeviceLimits& limits = info.properties.limits;
    const std::uint32_t invocations = options.workgroups * options.local_size;
    const std::uint64_t buffer_bytes =
        static_cast<std::uint64_t>(invocations) * TB::WordsPerInvocation * sizeof(std::uint32_t);

    if (info.properties.apiVersion < VK_API_VERSION_1_2) {
        reasons.push_back(fmt::format("device supports Vulkan {} but Vulkan 1.2 is required",
                                      ApiVersionString(info.properties.apiVersion)));
    }
    if (info.compute_family == std::numeric_limits<std::uint32_t>::max()) {
        reasons.push_back("no compute-capable queue family");
    }
    if (!info.core_features.shaderImageGatherExtended) {
        reasons.push_back("production SPIR-V requires shaderImageGatherExtended");
    }
    if ((info.subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0) {
        reasons.push_back("subgroup operations not supported in the compute stage");
    }
    if ((info.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) == 0) {
        reasons.push_back("subgroup shuffle operation not supported");
    }
    if (!CanForceWarpSizeSubgroup(info) && info.subgroup.subgroupSize != kWarpSize) {
        reasons.push_back("the benchmark requires fixed 32-lane compute subgroups");
    }
    if (CanForceWarpSizeSubgroup(info) &&
        options.local_size / kWarpSize >
            info.subgroup_size_control.maxComputeWorkgroupSubgroups) {
        reasons.push_back("local size exceeds maxComputeWorkgroupSubgroups at 32 lanes");
    }
    if (options.local_size > limits.maxComputeWorkGroupInvocations) {
        reasons.push_back(fmt::format("local size {} exceeds maxComputeWorkGroupInvocations {}",
                                      options.local_size, limits.maxComputeWorkGroupInvocations));
    }
    if (options.local_size > limits.maxComputeWorkGroupSize[0]) {
        reasons.push_back(fmt::format("local size {} exceeds maxComputeWorkGroupSize[0] {}",
                                      options.local_size, limits.maxComputeWorkGroupSize[0]));
    }
    if (options.workgroups > limits.maxComputeWorkGroupCount[0]) {
        reasons.push_back(fmt::format("workgroups {} exceed maxComputeWorkGroupCount[0] {}",
                                      options.workgroups, limits.maxComputeWorkGroupCount[0]));
    }
    if (buffer_bytes > limits.maxStorageBufferRange) {
        reasons.push_back(fmt::format("storage buffer of {} bytes exceeds maxStorageBufferRange {}",
                                      buffer_bytes, limits.maxStorageBufferRange));
    }
    if (shader.shared_memory_bytes > limits.maxComputeSharedMemorySize) {
        reasons.push_back(fmt::format(
            "shader needs {} bytes of shared memory but maxComputeSharedMemorySize is {}",
            shader.shared_memory_bytes, limits.maxComputeSharedMemorySize));
    }

    if (options.mode != TB::Mode::Cooperative) {
        return reasons;
    }

    const bool has_cooperative_matrix = info.HasExtension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    if (!has_cooperative_matrix) {
        reasons.push_back("VK_KHR_cooperative_matrix extension is missing");
    } else {
        if ((info.cooperative_matrix.cooperativeMatrixSupportedStages &
             VK_SHADER_STAGE_COMPUTE_BIT) == 0) {
            reasons.push_back("cooperative matrices not supported in the compute stage");
        }
        if (!HasTensorShape16x16x16(instance, info)) {
            reasons.push_back(
                "no 16x16x16 FP16 cooperative-matrix shape with subgroup scope and "
                "non-saturating accumulation");
        }
        if (!info.cooperative_matrix_features.cooperativeMatrix) {
            reasons.push_back("cooperativeMatrix feature not supported");
        }
    }
    if (!info.float16.shaderFloat16) {
        reasons.push_back("shaderFloat16 feature not supported");
    }
    if (!info.memory_model.vulkanMemoryModel) {
        reasons.push_back("vulkanMemoryModel feature not supported");
    }
    if (!info.memory_model.vulkanMemoryModelDeviceScope) {
        reasons.push_back("vulkanMemoryModelDeviceScope feature not supported");
    }
    if (!info.HasExtension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) {
        reasons.push_back("VK_EXT_subgroup_size_control extension is missing");
    } else if (!CanForceWarpSizeSubgroup(info)) {
        reasons.push_back(fmt::format(
            "compute stage cannot be forced to {}-lane subgroups (requiredSubgroupSizeStages, "
            "full subgroups, or the {}..{} subgroup size range do not allow it)",
            kWarpSize, info.subgroup_size_control.minSubgroupSize,
            info.subgroup_size_control.maxSubgroupSize));
    }
    // Production guard: AMD's driver advertises VK_KHR_cooperative_matrix even on GPUs without
    // matrix cores (GCN, RDNA1, RDNA2) and crashes building these pipelines; RDNA3+ reports
    // accelerated mixed-signedness dot4.
    const std::uint32_t driver_id = info.driver.driverID;
    const bool emulated_amd_matrices =
        (driver_id == VK_DRIVER_ID_AMD_PROPRIETARY || driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE) &&
        !info.integer_dot_product.integerDotProduct4x8BitPackedMixedSignednessAccelerated;
    if (emulated_amd_matrices) {
        reasons.push_back(
            "VK_KHR_cooperative_matrix is emulated on this AMD GPU (no accelerated "
            "mixed-signedness dot4); matches the production device gate");
    }
    return reasons;
}

/// Device selection: explicit --device index wins; otherwise the first AMD discrete GPU.
/// Software rasterizers (llvmpipe reports a CPU device) never qualify as "AMD discrete",
/// and nothing is ever relabeled.
std::uint32_t ChooseDeviceIndex(const std::vector<PhysicalInfo>& devices, const Options& options) {
    if (options.device) {
        if (*options.device >= devices.size()) {
            throw UsageError(fmt::format(
                "--device {} is out of range ({} device(s) found; use --list-devices)",
                *options.device, devices.size()));
        }
        return *options.device;
    }
    const auto amd_discrete = std::find_if(devices.begin(), devices.end(),
                                           [](const PhysicalInfo& candidate) {
                                               return candidate.properties.vendorID == 0x1002 &&
                                                      candidate.properties.deviceType ==
                                                          VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
                                           });
    if (amd_discrete == devices.end()) {
        std::vector<std::string> summaries;
        for (std::uint32_t i = 0; i < devices.size(); ++i) {
            const std::string_view vendor = VendorName(devices[i].properties.vendorID);
            summaries.push_back(fmt::format(
                "{}: {} ({}{}, vendor 0x{:04X})", i, devices[i].properties.deviceName,
                DeviceTypeName(devices[i].properties.deviceType),
                vendor.empty() ? "" : fmt::format(", {}", vendor),
                devices[i].properties.vendorID));
        }
        throw UsageError(fmt::format(
            "no default AMD discrete GPU found; pass --device N to choose one: {}",
            Join(summaries, "; ")));
    }
    return static_cast<std::uint32_t>(std::distance(devices.begin(), amd_discrete));
}

// -----------------------------------------------------------------------------
// Validation layer plumbing
// -----------------------------------------------------------------------------

struct ValidationState {
    std::atomic<std::uint32_t> errors{0};
    std::atomic<std::uint32_t> warnings{0};
    /// Compile-only replays set this so a validation ERROR aborts the driver call being
    /// validated instead of letting an invalid shader/layout reach vkCreateComputePipelines.
    /// The synthetic run path keeps the default (VK_FALSE, calls proceed and are sampled).
    std::atomic<bool> abort_on_error{false};
};

std::string_view DebugSeverityName(VkDebugUtilsMessageSeverityFlagBitsEXT severity) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        return "error";
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        return "warning";
    }
    return "info";
}

/// Destroys the debug messenger through the dynamically loaded driver entry point before the
/// instance dies.
struct MessengerDeleter {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy = nullptr;
    void operator()(VkDebugUtilsMessengerEXT messenger) const {
        destroy(instance, messenger, nullptr);
    }
};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    auto* state = static_cast<ValidationState*>(user_data);
    const bool validation_error = (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0 &&
                                  (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0) {
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
            state->errors.fetch_add(1, std::memory_order_relaxed);
        } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
            state->warnings.fetch_add(1, std::memory_order_relaxed);
        }
    }
    fmt::print(stderr, "tensor-bench: validation[{}|{}] {}\n", DebugSeverityName(severity),
               data->pMessageIdName != nullptr ? data->pMessageIdName : "unknown",
               data->pMessage != nullptr ? data->pMessage : "");
    if (validation_error && state->abort_on_error.load(std::memory_order_relaxed)) {
        LogLine("validation error: aborting the driver call (compile-only replay)");
        return VK_TRUE;
    }
    return VK_FALSE;
}

Scoped<VkInstance, void (*)(VkInstance)> CreateInstance(bool want_validation,
                                                        bool& validation_layer_enabled,
                                                        bool& debug_utils_enabled) {
    const auto layers = EnumerateInstanceLayers();
    const auto extensions = EnumerateInstanceExtensions();
    validation_layer_enabled =
        want_validation && ContainsLayer(layers, "VK_LAYER_KHRONOS_validation");
    debug_utils_enabled =
        want_validation && ContainsExtension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (want_validation && !validation_layer_enabled) {
        throw std::runtime_error("validation requested but VK_LAYER_KHRONOS_validation is not installed");
    }
    if (want_validation && !debug_utils_enabled) {
        throw std::runtime_error("validation requested but VK_EXT_debug_utils is unavailable");
    }

    std::vector<const char*> enabled_layers;
    std::vector<const char*> enabled_extensions;
    if (validation_layer_enabled) {
        enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        const auto layer_extensions = EnumerateInstanceExtensions("VK_LAYER_KHRONOS_validation");
        if (!ContainsExtension(layer_extensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            throw std::runtime_error("validation layer does not provide synchronization validation");
        }
        enabled_extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    if (debug_utils_enabled) {
        enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "tensor-bench";
    application.applicationVersion = 1;
    application.pEngineName = "tensor-bench";
    application.engineVersion = 1;
    application.apiVersion = VK_API_VERSION_1_2;

    const VkValidationFeatureEnableEXT sync_validation =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation_features.enabledValidationFeatureCount = 1;
    validation_features.pEnabledValidationFeatures = &sync_validation;
    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pNext = want_validation ? &validation_features : nullptr;
    create_info.pApplicationInfo = &application;
    create_info.enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size());
    create_info.ppEnabledLayerNames = enabled_layers.data();
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_extensions.size());
    create_info.ppEnabledExtensionNames = enabled_extensions.data();
    VkInstance instance = VK_NULL_HANDLE;
    CheckVk(vkCreateInstance(&create_info, nullptr, &instance), "vkCreateInstance");
    return Scoped<VkInstance, void (*)(VkInstance)>{
        instance, [](VkInstance owned) { vkDestroyInstance(owned, nullptr); }};
}

/// Creates the debug-utils messenger when validation is active; it is destroyed through the
/// dynamically loaded entry point before the instance dies (see MessengerDeleter).
Scoped<VkDebugUtilsMessengerEXT, MessengerDeleter> CreateMessenger(VkInstance instance,
                                                                   ValidationState& validation,
                                                                   bool debug_utils_enabled) {
    if (!debug_utils_enabled) {
        return {};
    }
    const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (create_messenger == nullptr || destroy_messenger == nullptr) {
        throw std::runtime_error("VK_EXT_debug_utils entry points unavailable");
    }
    VkDebugUtilsMessengerCreateInfoEXT messenger_create{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger_create.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_create.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_create.pfnUserCallback = DebugUtilsCallback;
    messenger_create.pUserData = &validation;
    VkDebugUtilsMessengerEXT handle = VK_NULL_HANDLE;
    CheckVk(create_messenger(instance, &messenger_create, nullptr, &handle),
            "vkCreateDebugUtilsMessengerEXT");
    return Scoped<VkDebugUtilsMessengerEXT, MessengerDeleter>{
        handle, MessengerDeleter{instance, destroy_messenger}};
}

// -----------------------------------------------------------------------------
// Device creation
// -----------------------------------------------------------------------------

/// Device features a compile-only replay module needs, filled from the bounded capability
/// mapping plus the replay context. Null in the synthetic path, which keeps today's
/// opportunistic feature enabling unchanged.
struct ModuleRequirements {
    bool uses_warp_ops{false};
    bool storage_image_write_without_format{false};
    bool storage_image_read_without_format{false};
    bool int8{false};
    bool int16{false};
    bool int64{false};
    bool float64{false};
    bool variable_pointers_storage_buffer{false};
};

struct DevicePlan {
    Scoped<VkDevice, void (*)(VkDevice)> device;
    std::uint32_t queue_family{};
    bool force_warp_size_subgroup{false};
};

DevicePlan CreateDevice(const PhysicalInfo& info, const Options& options,
                        const ModuleRequirements* module = nullptr) {
    const bool cooperative = options.mode == TB::Mode::Cooperative;
    const bool use_subgroup_size_control =
        info.HasExtension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) &&
        info.subgroup_size_control_features.subgroupSizeControl;
    // Synthetic cooperative mode and warp-ops replay modules mirror the production subgroup
    // decision (vk_compute_pipeline.cpp): force the guest warp size only when the runtime
    // would, and only for shaders that observe the subgroup.
    const bool force_warp_size_subgroup =
        use_subgroup_size_control && CanForceWarpSizeSubgroup(info) &&
        (module == nullptr || module->uses_warp_ops);
    if (cooperative && !force_warp_size_subgroup) {
        // SkipReasons gates this before CreateDevice is reached.
        throw std::runtime_error("cooperative mode requires a settable 32-lane compute subgroup");
    }

    std::vector<const char*> enabled_extensions;
    if (use_subgroup_size_control) {
        enabled_extensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    }
    if (cooperative) {
        enabled_extensions.push_back(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    }

    VkDeviceQueueCreateInfo queue_create_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_create_info.queueFamilyIndex = info.compute_family;
    queue_create_info.queueCount = 1;
    const float queue_priority = 1.0F;
    queue_create_info.pQueuePriorities = &queue_priority;

    // Enable supported features only; unsupported bits would make vkCreateDevice fail. In
    // software mode these are opportunistic enables (the shader decides what it needs), in
    // cooperative mode SkipReasons has already verified the required bits.
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    cooperative_features.cooperativeMatrix = info.cooperative_matrix_features.cooperativeMatrix;
    // Mirrors production: robust buffer access stays off for cooperative matrices.
    cooperative_features.cooperativeMatrixRobustBufferAccess = VK_FALSE;
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup_size_control_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    subgroup_size_control_features.subgroupSizeControl =
        info.subgroup_size_control_features.subgroupSizeControl;
    subgroup_size_control_features.computeFullSubgroups =
        info.subgroup_size_control_features.computeFullSubgroups;
    VkPhysicalDeviceVulkanMemoryModelFeatures memory_model_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
    memory_model_features.vulkanMemoryModel = info.memory_model.vulkanMemoryModel;
    memory_model_features.vulkanMemoryModelDeviceScope = info.memory_model.vulkanMemoryModelDeviceScope;
    VkPhysicalDeviceShaderFloat16Int8Features float16_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    float16_features.shaderFloat16 = info.float16.shaderFloat16;
    VkPhysicalDevice16BitStorageFeatures storage16_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    storage16_features.storageBuffer16BitAccess = info.storage16.storageBuffer16BitAccess;
    storage16_features.uniformAndStorageBuffer16BitAccess =
        info.storage16.uniformAndStorageBuffer16BitAccess;
    storage16_features.storagePushConstant16 = info.storage16.storagePushConstant16;
    storage16_features.storageInputOutput16 = info.storage16.storageInputOutput16;
    VkPhysicalDeviceVariablePointersFeatures variable_pointers_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};

    void* feature_next = nullptr;
    const auto push_feature = [&](auto& structure) {
        structure.pNext = feature_next;
        feature_next = &structure;
    };
    if (cooperative) {
        push_feature(cooperative_features);
    }
    if (use_subgroup_size_control) {
        push_feature(subgroup_size_control_features);
    }
    if (info.properties.apiVersion >= VK_API_VERSION_1_2) {
        push_feature(memory_model_features);
        push_feature(float16_features);
        push_feature(storage16_features);
        if (module != nullptr && module->variable_pointers_storage_buffer) {
            variable_pointers_features.variablePointersStorageBuffer =
                info.variable_pointers.variablePointersStorageBuffer;
            push_feature(variable_pointers_features);
        }
    }

    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.shaderImageGatherExtended = info.core_features.shaderImageGatherExtended;
    if (module != nullptr) {
        // Only bits the capability mapping demanded and the device queried as supported;
        // every required bit was gated by CompileSkipReasons before this point.
        enabled_features.shaderStorageImageWriteWithoutFormat =
            module->storage_image_write_without_format &&
            info.core_features.shaderStorageImageWriteWithoutFormat;
        enabled_features.shaderStorageImageReadWithoutFormat =
            module->storage_image_read_without_format &&
            info.core_features.shaderStorageImageReadWithoutFormat;
        enabled_features.shaderInt16 = module->int16 && info.core_features.shaderInt16;
        enabled_features.shaderInt64 = module->int64 && info.core_features.shaderInt64;
        enabled_features.shaderFloat64 = module->float64 && info.core_features.shaderFloat64;
        float16_features.shaderInt8 = module->int8 && info.float16.shaderInt8;
    }
    VkDeviceCreateInfo device_create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_create_info.pNext = feature_next;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;
    device_create_info.enabledExtensionCount =
        static_cast<std::uint32_t>(enabled_extensions.size());
    device_create_info.ppEnabledExtensionNames = enabled_extensions.data();
    device_create_info.pEnabledFeatures = &enabled_features;
    VkDevice device = VK_NULL_HANDLE;
    CheckVk(vkCreateDevice(info.handle, &device_create_info, nullptr, &device), "vkCreateDevice");
    return DevicePlan{
        .device = Scoped<VkDevice, void (*)(VkDevice)>{
            device, [](VkDevice owned) { vkDestroyDevice(owned, nullptr); }},
        .queue_family = info.compute_family,
        .force_warp_size_subgroup = force_warp_size_subgroup};
}

// -----------------------------------------------------------------------------
// Host-visible buffer
// -----------------------------------------------------------------------------

std::uint32_t FindMemoryType(VkPhysicalDevice physical, std::uint32_t type_bits,
                             VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const VkMemoryType& type = memory_properties.memoryTypes[i];
        if ((type_bits & (1U << i)) != 0 && (type.propertyFlags & required) == required) {
            return i;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

/// Host-visible storage buffer. Destruction order is buffer, unmap, free: the buffer (a child
/// of the memory) dies before the memory itself.
class HostBuffer {
public:
    HostBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize bytes) : device_{device} {
        try {
            VkBufferCreateInfo buffer_create_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_create_info.size = bytes;
            buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            CheckVk(vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_),
                    "vkCreateBuffer");

            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(device, buffer_, &requirements);
            VkMemoryPropertyFlags wanted =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            std::uint32_t memory_type = FindMemoryType(physical, requirements.memoryTypeBits, wanted);
            if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
                wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
                memory_type = FindMemoryType(physical, requirements.memoryTypeBits, wanted);
            }
            if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("no host-visible memory type for the storage buffer");
            }
            coherent_ = (wanted & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

            VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate_info.allocationSize = requirements.size;
            allocate_info.memoryTypeIndex = memory_type;
            CheckVk(vkAllocateMemory(device, &allocate_info, nullptr, &memory_),
                    "vkAllocateMemory");
            CheckVk(vkBindBufferMemory(device, buffer_, memory_, 0), "vkBindBufferMemory");
            CheckVk(vkMapMemory(device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "vkMapMemory");
        } catch (...) {
            Release();
            throw;
        }
    }

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    HostBuffer(HostBuffer&& other) noexcept {
        Swap(other);
    }
    HostBuffer& operator=(HostBuffer&& other) noexcept {
        if (this != &other) {
            Release();
            Swap(other);
        }
        return *this;
    }
    ~HostBuffer() {
        Release();
    }

    void Release() {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer_, nullptr);
            buffer_ = VK_NULL_HANDLE;
        }
        if (mapped_ != nullptr) {
            vkUnmapMemory(device_, memory_);
            mapped_ = nullptr;
        }
        if (memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory_, nullptr);
            memory_ = VK_NULL_HANDLE;
        }
        device_ = VK_NULL_HANDLE;
    }

    void* Mapped() const {
        return mapped_;
    }
    VkBuffer Get() const {
        return buffer_;
    }
    bool Coherent() const {
        return coherent_;
    }

    void Flush() const {
        if (Coherent()) {
            return;
        }
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory_;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        CheckVk(vkFlushMappedMemoryRanges(device_, 1, &range), "vkFlushMappedMemoryRanges");
    }

    void Invalidate() const {
        if (Coherent()) {
            return;
        }
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory_;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        CheckVk(vkInvalidateMappedMemoryRanges(device_, 1, &range),
                "vkInvalidateMappedMemoryRanges");
    }

private:
    void Swap(HostBuffer& other) noexcept {
        std::swap(device_, other.device_);
        std::swap(buffer_, other.buffer_);
        std::swap(memory_, other.memory_);
        std::swap(mapped_, other.mapped_);
        std::swap(coherent_, other.coherent_);
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    bool coherent_ = true;
};

// -----------------------------------------------------------------------------
// JSON builders
// -----------------------------------------------------------------------------

Json DeviceJson(const PhysicalInfo& info, std::uint32_t index) {
    const std::string_view vendor = VendorName(info.properties.vendorID);
    return Json{
        {"index", index},
        {"name", info.properties.deviceName},
        {"vendor_id", info.properties.vendorID},
        {"vendor_name", vendor.empty() ? Json(nullptr) : Json(vendor)},
        {"device_id", info.properties.deviceID},
        {"device_type", DeviceTypeName(info.properties.deviceType)},
        {"is_software", info.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU},
        {"api_version", ApiVersionString(info.properties.apiVersion)},
        {"driver_id", info.driver.driverID},
        {"driver_name", info.driver.driverName},
        {"driver_info", info.driver.driverInfo},
        {"subgroup",
         Json{{"size", info.subgroup.subgroupSize},
              {"min_subgroup_size", info.subgroup_size_control.minSubgroupSize},
              {"max_subgroup_size", info.subgroup_size_control.maxSubgroupSize},
              {"supported_stages", info.subgroup.supportedStages},
              {"supported_operations", info.subgroup.supportedOperations},
              {"required_size_stages", info.subgroup_size_control.requiredSubgroupSizeStages},
              {"supported_operation_names",
               SubgroupOperationNames(info.subgroup.supportedOperations)}}},
    };
}

Json CaseJson(const Options& options, const TB::Parameters& parameters,
              std::optional<std::uint32_t> required_subgroup_size) {
    return Json{
        {"mode", ModeName(options.mode)},
        {"pattern", PatternName(options.pattern)},
        {"depth", parameters.depth},
        {"operations", parameters.operations},
        {"local_size", parameters.local_size},
        {"workgroups", options.workgroups},
        {"iterations", options.iterations},
        {"compile_repeats", options.compile_repeats},
        {"seed", options.seed},
        {"required_subgroup_size",
         required_subgroup_size ? Json(*required_subgroup_size) : Json(nullptr)},
    };
}

Json ShaderJson(const TB::ShaderModule& shader, double emission_ms) {
    return Json{
        {"spirv_words", shader.words.size()},
        {"bytes", shader.words.size() * sizeof(std::uint32_t)},
        {"shared_memory_bytes", shader.shared_memory_bytes},
        {"emission_ms", emission_ms},
    };
}

// -----------------------------------------------------------------------------
// SPIR-V emission
// -----------------------------------------------------------------------------

/// Writes SPIR-V words as explicit little-endian bytes (SPIR-V words are little-endian on
/// disk regardless of host endianness).
void WriteSpirv(const std::string& path, const std::vector<std::uint32_t>& words) {
    std::vector<unsigned char> bytes(words.size() * sizeof(std::uint32_t));
    for (size_t i = 0; i < words.size(); ++i) {
        bytes[i * 4 + 0] = static_cast<unsigned char>((words[i] >> 0) & 0xFFU);
        bytes[i * 4 + 1] = static_cast<unsigned char>((words[i] >> 8) & 0xFFU);
        bytes[i * 4 + 2] = static_cast<unsigned char>((words[i] >> 16) & 0xFFU);
        bytes[i * 4 + 3] = static_cast<unsigned char>((words[i] >> 24) & 0xFFU);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error(fmt::format("cannot open {} for writing", path));
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        throw std::runtime_error(fmt::format("failed writing {}", path));
    }
}

std::optional<std::uint64_t> PeakRssKib() {
#ifdef __linux__
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            const size_t start = line.find_first_not_of(" \t", 6);
            if (start == std::string::npos) {
                return std::nullopt;
            }
            return std::strtoull(line.c_str() + start, nullptr, 10);
        }
    }
#endif
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// Compile-pipeline replay (--compile-pipeline)
//
// Explicitly compile-only: no fixture buffers, no queue submission, no dispatch, no
// correctness claim. Input framing and identity are checked before Vulkan; use spirv-val
// separately for full SPIR-V validation.
// -----------------------------------------------------------------------------

constexpr std::uint64_t kMaxReplayModuleBytes = 64 * 1024 * 1024;
constexpr std::uint64_t kMaxReplayJsonBytes = 4 * 1024 * 1024;

/// Production compute push-constant layout (pipeline_helper.h CreatePipelineLayout).
std::uint32_t ProductionPushConstantBytes() {
    using Shader::Backend::SPIRV::RenderAreaLayout;
    using Shader::Backend::SPIRV::RescalingLayout;
    return static_cast<std::uint32_t>(sizeof(RescalingLayout)) -
           static_cast<std::uint32_t>(sizeof(RescalingLayout::down_factor)) +
           static_cast<std::uint32_t>(sizeof(RenderAreaLayout));
}

/// Frozen compile_context of a successful compute replay (schema_version 1,
/// profile "offline-spirv-1.4").
struct CompileContext {
    std::array<std::uint32_t, 3> workgroup_size{};
    std::uint32_t shared_memory_bytes{};
    bool uses_warp_ops{};
    bool uses_cooperative_matrix{};
    struct Binding {
        std::string type_name;
        VkDescriptorType type{};
        std::uint32_t count{};
    };
    std::vector<Binding> bindings;
    std::uint32_t push_constant_bytes{};
    std::uint64_t spirv_hash{};
};

/// A validated replay: JSON context plus the reflected, hash-checked SPIR-V module.
struct ReplayModule {
    std::string source_path;
    std::string spirv_path;
    std::vector<std::uint32_t> words;
    std::vector<spv::Capability> capabilities;
    CompileContext context;
};

const Json& RequiredField(const Json& object, const char* name, std::string_view scope) {
    const auto found = object.find(name);
    if (found == object.end()) {
        throw std::runtime_error(fmt::format("{}: missing '{}'", scope, name));
    }
    return *found;
}

std::uint32_t RequiredUint(const Json& object, const char* name, std::string_view scope,
                           std::uint64_t lo, std::uint64_t hi) {
    const Json& field = RequiredField(object, name, scope);
    if (!field.is_number_unsigned()) {
        throw std::runtime_error(fmt::format("{}: '{}' must be an unsigned integer", scope, name));
    }
    const std::uint64_t value = field.get<std::uint64_t>();
    if (value < lo || value > hi) {
        throw std::runtime_error(
            fmt::format("{}: '{}' is {} but must be in the range {}..{}", scope, name, value, lo,
                        hi));
    }
    return static_cast<std::uint32_t>(value);
}

std::string RequiredString(const Json& object, const char* name, std::string_view scope) {
    const Json& field = RequiredField(object, name, scope);
    if (!field.is_string()) {
        throw std::runtime_error(fmt::format("{}: '{}' must be a string", scope, name));
    }
    return field.get<std::string>();
}

bool RequiredBool(const Json& object, const char* name, std::string_view scope) {
    const Json& field = RequiredField(object, name, scope);
    if (!field.is_boolean()) {
        throw std::runtime_error(fmt::format("{}: '{}' must be a boolean", scope, name));
    }
    return field.get<bool>();
}

std::optional<VkDescriptorType> DescriptorTypeFromName(std::string_view name) {
    if (name == "uniform_buffer") {
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    if (name == "storage_buffer") {
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    if (name == "uniform_texel_buffer") {
        return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    }
    if (name == "storage_texel_buffer") {
        return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    }
    if (name == "combined_image_sampler") {
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    if (name == "storage_image") {
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return std::nullopt;
}

/// Reads the module words as explicit little-endian (SPIR-V words are little-endian on disk).
std::vector<std::uint32_t> DecodeSpirvWords(const std::vector<unsigned char>& bytes,
                                            const std::string& path) {
    if (bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) {
        throw std::runtime_error(fmt::format(
            "{}: module size {} is not a non-empty multiple of four bytes", path, bytes.size()));
    }
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    for (size_t i = 0; i < words.size(); ++i) {
        words[i] = static_cast<std::uint32_t>(bytes[i * 4 + 0]) |
                   static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 8 |
                   static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 16 |
                   static_cast<std::uint32_t>(bytes[i * 4 + 3]) << 24;
    }
    return words;
}

std::vector<spv::Capability> ReflectCapabilities(const std::vector<std::uint32_t>& words,
                                                const std::string& path,
                                                const CompileContext& context) {
    if (words.size() < 5) {
        throw std::runtime_error(fmt::format("{}: module is shorter than the five-word header",
                                             path));
    }
    if (words[0] != 0x07230203) {
        throw std::runtime_error(fmt::format("{}: wrong SPIR-V magic number 0x{:08X}", path,
                                             words[0]));
    }
    if ((words[1] & 0xFF000000U) != 0) {
        throw std::runtime_error(fmt::format("{}: reserved version bits are set", path));
    }
    if (words[1] != 0x00010400) {
        throw std::runtime_error(fmt::format(
            "{}: the offline profile emits SPIR-V 1.4 but the module declares version {}.{}",
            path, (words[1] >> 16) & 0xFFU, (words[1] >> 8) & 0xFFU));
    }
    if (words[3] == 0 || words[3] > 0x400000) {
        throw std::runtime_error(fmt::format("{}: invalid SPIR-V id bound {}", path, words[3]));
    }
    if (words[4] != 0) {
        throw std::runtime_error(fmt::format("{}: reserved schema word is {}", path, words[4]));
    }
    std::vector<spv::Capability> capabilities;
    std::uint32_t entry_point{};
    std::uint32_t local_size_entry_point{};
    std::optional<std::array<std::uint32_t, 3>> local_size;
    for (size_t index = 5; index < words.size();) {
        const std::uint32_t word_count = words[index] >> 16;
        if (word_count == 0 || word_count > words.size() - index) {
            throw std::runtime_error(fmt::format(
                "{}: instruction at word {} claims {} words but the module has {}", path, index,
                word_count, words.size() - index));
        }
        const auto opcode = static_cast<spv::Op>(words[index] & 0xFFFFU);
        if (opcode == spv::Op::OpEntryPoint) {
            if (entry_point != 0 || word_count < 5 ||
                words[index + 1] != static_cast<std::uint32_t>(spv::ExecutionModel::GLCompute) ||
                words[index + 3] != 0x6e69616d || words[index + 4] != 0) {
                throw std::runtime_error(fmt::format(
                    "{}: expected exactly one GLCompute entry point named main", path));
            }
            entry_point = words[index + 2];
        } else if (opcode == spv::Op::OpExecutionMode && word_count >= 3 &&
                   words[index + 2] == static_cast<std::uint32_t>(spv::ExecutionMode::LocalSize)) {
            if (word_count != 6 || local_size) {
                throw std::runtime_error(fmt::format("{}: invalid or duplicate LocalSize", path));
            }
            local_size_entry_point = words[index + 1];
            local_size = std::array{words[index + 3], words[index + 4], words[index + 5]};
        } else if (opcode == spv::Op::OpCapability) {
            if (word_count != 2) {
                throw std::runtime_error(
                    fmt::format("{}: OpCapability at word {} must have exactly one operand", path,
                                index));
            }
            capabilities.push_back(static_cast<spv::Capability>(words[index + 1]));
        }
        index += word_count;
    }
    if (entry_point == 0 || local_size_entry_point != entry_point || !local_size ||
        *local_size != context.workgroup_size) {
        throw std::runtime_error(fmt::format(
            "{}: compute entry point and literal LocalSize must match compile_context", path));
    }
    std::sort(capabilities.begin(), capabilities.end());
    capabilities.erase(std::unique(capabilities.begin(), capabilities.end()),
                       capabilities.end());
    return capabilities;
}

std::string CapabilityLabel(spv::Capability capability) {
    switch (capability) {
    case spv::Capability::Shader:
        return "Shader";
    case spv::Capability::SampledBuffer:
        return "SampledBuffer";
    case spv::Capability::ImageQuery:
        return "ImageQuery";
    case spv::Capability::ImageGatherExtended:
        return "ImageGatherExtended";
    case spv::Capability::StorageImageReadWithoutFormat:
        return "StorageImageReadWithoutFormat";
    case spv::Capability::StorageImageWriteWithoutFormat:
        return "StorageImageWriteWithoutFormat";
    case spv::Capability::Int8:
        return "Int8";
    case spv::Capability::Int16:
        return "Int16";
    case spv::Capability::Int64:
        return "Int64";
    case spv::Capability::Float16:
        return "Float16";
    case spv::Capability::Float64:
        return "Float64";
    case spv::Capability::GroupNonUniform:
        return "GroupNonUniform";
    case spv::Capability::GroupNonUniformVote:
        return "GroupNonUniformVote";
    case spv::Capability::GroupNonUniformArithmetic:
        return "GroupNonUniformArithmetic";
    case spv::Capability::GroupNonUniformBallot:
        return "GroupNonUniformBallot";
    case spv::Capability::GroupNonUniformShuffle:
        return "GroupNonUniformShuffle";
    case spv::Capability::GroupNonUniformShuffleRelative:
        return "GroupNonUniformShuffleRelative";
    case spv::Capability::VariablePointersStorageBuffer:
        return "VariablePointersStorageBuffer";
    case spv::Capability::StorageBuffer16BitAccess:
        return "StorageBuffer16BitAccess";
    case spv::Capability::VulkanMemoryModel:
        return "VulkanMemoryModel";
    case spv::Capability::VulkanMemoryModelDeviceScope:
        return "VulkanMemoryModelDeviceScope";
    case spv::Capability::CooperativeMatrixKHR:
        return "CooperativeMatrixKHR";
    default:
        return fmt::format("Capability({})", static_cast<unsigned>(capability));
    }
}

std::uint64_t ParseSpirvHash(const Json& context, std::string_view scope) {
    const std::string text = RequiredString(context, "spirv_hash", scope);
    if (text.size() != 16) {
        throw std::runtime_error(fmt::format(
            "{}: spirv_hash must be 16 lower-case hex characters", scope));
    }
    std::uint64_t hash = 0;
    for (const char digit : text) {
        hash <<= 4;
        if (digit >= '0' && digit <= '9') {
            hash |= static_cast<std::uint64_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            hash |= static_cast<std::uint64_t>(digit - 'a' + 10);
        } else {
            throw std::runtime_error(fmt::format(
                "{}: spirv_hash must be 16 lower-case hex characters", scope));
        }
    }
    return hash;
}

CompileContext ParseCompileContext(const Json& document) {
    const Json& context = RequiredField(document, "compile_context", "replay");
    if (!context.is_object()) {
        throw std::runtime_error("replay: 'compile_context' must be an object");
    }
    const std::string_view scope = "replay.compile_context";
    if (RequiredUint(context, "schema_version", scope, 1, 1) != 1) {
        throw std::runtime_error(fmt::format("{}: schema_version must be 1", scope));
    }
    if (RequiredString(context, "profile", scope) != "offline-spirv-1.4") {
        throw std::runtime_error(fmt::format("{}: profile must be \"offline-spirv-1.4\"", scope));
    }
    CompileContext parsed;
    const Json& workgroup_size = RequiredField(context, "workgroup_size", scope);
    if (!workgroup_size.is_array() || workgroup_size.size() != 3) {
        throw std::runtime_error(
            fmt::format("{}: workgroup_size must be an array of three unsigned integers", scope));
    }
    std::uint64_t invocations = 1;
    for (size_t dim = 0; dim < 3; ++dim) {
        const Json& field = workgroup_size[dim];
        if (!field.is_number_unsigned()) {
            throw std::runtime_error(fmt::format(
                "{}: workgroup_size[{}] must be an unsigned integer", scope, dim));
        }
        const std::uint64_t value = field.get<std::uint64_t>();
        if (value < 1 || value > 65535) {
            throw std::runtime_error(fmt::format(
                "{}: workgroup_size[{}] is {} but must be in the range 1..65535", scope, dim,
                value));
        }
        parsed.workgroup_size[dim] = static_cast<std::uint32_t>(value);
        invocations *= value;
    }
    if (invocations > 1'048'576) {
        throw std::runtime_error(fmt::format(
            "{}: workgroup_size multiplies to {} invocations, above the static sanity bound",
            scope, invocations));
    }
    parsed.shared_memory_bytes =
        RequiredUint(context, "shared_memory_bytes", scope, 0, 1024 * 1024);
    parsed.uses_warp_ops = RequiredBool(context, "uses_warp_ops", scope);
    parsed.uses_cooperative_matrix =
        RequiredBool(context, "uses_cooperative_matrix", scope);
    if (parsed.uses_cooperative_matrix) {
        throw std::runtime_error("offline-spirv-1.4 does not emit cooperative-matrix shaders");
    }
    const Json& bindings = RequiredField(context, "bindings", scope);
    if (!bindings.is_array() || bindings.size() > 1024) {
        throw std::runtime_error(fmt::format(
            "{}: bindings must be an array with at most 1024 entries", scope));
    }
    for (size_t i = 0; i < bindings.size(); ++i) {
        const Json& binding = bindings[i];
        if (!binding.is_object()) {
            throw std::runtime_error(fmt::format("{}: bindings[{}] must be an object", scope, i));
        }
        const std::string binding_scope = fmt::format("{}.bindings[{}]", scope, i);
        if (RequiredUint(binding, "binding", binding_scope, 0,
                         std::numeric_limits<std::uint32_t>::max()) != i) {
            throw std::runtime_error(fmt::format(
                "{}: set 0 bindings must be sequential, expected binding {}", binding_scope, i));
        }
        const std::string type_name = RequiredString(binding, "type", binding_scope);
        const std::optional<VkDescriptorType> type = DescriptorTypeFromName(type_name);
        if (!type) {
            throw std::runtime_error(fmt::format(
                "{}: '{}' is not a supported descriptor type", binding_scope, type_name));
        }
        CompileContext::Binding parsed_binding;
        parsed_binding.type_name = type_name;
        parsed_binding.type = *type;
        parsed_binding.count = RequiredUint(binding, "count", binding_scope, 1,
                                            std::numeric_limits<std::uint32_t>::max());
        parsed.bindings.push_back(std::move(parsed_binding));
    }
    parsed.push_constant_bytes = RequiredUint(context, "push_constant_bytes", scope, 0, 1024 * 1024);
    const std::uint32_t production_bytes = ProductionPushConstantBytes();
    if (parsed.push_constant_bytes != production_bytes) {
        throw std::runtime_error(fmt::format(
            "{}: push_constant_bytes is {} but the production compute layout needs {}", scope,
            parsed.push_constant_bytes, production_bytes));
    }
    parsed.spirv_hash = ParseSpirvHash(context, scope);
    return parsed;
}

// Check the replay contract and module identity before opening Vulkan.
ReplayModule LoadReplayModule(const std::string& replay_path) {
    ReplayModule replay;
    replay.source_path = replay_path;
    std::ifstream file(replay_path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(fmt::format("cannot open replay JSON {}", replay_path));
    }
    const std::streamoff json_size = file.tellg();
    if (json_size < 0 || static_cast<std::uint64_t>(json_size) > kMaxReplayJsonBytes) {
        throw std::runtime_error(fmt::format("{}: replay JSON size exceeds the input bound",
                                             replay_path));
    }
    file.seekg(0);
    Json document;
    try {
        document = Json::parse(file);
    } catch (const Json::parse_error& error) {
        throw std::runtime_error(
            fmt::format("{}: JSON parse failed: {}", replay_path, error.what()));
    }
    if (!document.is_object()) {
        throw std::runtime_error(fmt::format("{}: top level must be a JSON object", replay_path));
    }
    const std::string_view scope = "replay";
    if (RequiredString(document, "status", scope) != "ok") {
        // Failed replays carry no compile_context; the metadata producer must be rerun.
        throw std::runtime_error(fmt::format(
            "{}: status is not \"ok\"; a failed replay carries no compile context", replay_path));
    }
    if (RequiredUint(document, "schema_version", scope, 1, 1) != 1) {
        throw std::runtime_error(fmt::format("{}: schema_version must be 1", scope));
    }
    // Only compute replays carry a single stage and a compile_context; the profile lives in
    // the context and is checked there.
    if (RequiredString(document, "kind", scope) != "compute") {
        throw std::runtime_error(fmt::format("{}: only \"compute\" replays are supported", scope));
    }
    replay.context = ParseCompileContext(document);

    const Json& stages = RequiredField(document, "stages", scope);
    if (!stages.is_array() || stages.size() != 1) {
        throw std::runtime_error(
            fmt::format("{}: stages must hold exactly the one compute stage", replay_path));
    }
    const Json& stage = stages[0];
    if (!stage.is_object()) {
        throw std::runtime_error(fmt::format("{}: stages[0] must be an object", replay_path));
    }
    const std::string_view stage_scope = "replay.stages[0]";
    if (RequiredString(stage, "stage", stage_scope) != "compute") {
        throw std::runtime_error(fmt::format("{}: stage must be \"compute\"", stage_scope));
    }
    const std::string spirv_name = RequiredString(stage, "spirv", stage_scope);
    if (spirv_name.empty()) {
        throw std::runtime_error(fmt::format("{}: 'spirv' is empty", stage_scope));
    }
    const std::uint32_t spirv_words =
        RequiredUint(stage, "spirv_words", stage_scope, 1, kMaxReplayModuleBytes / 4);
    const std::filesystem::path relative(spirv_name);
    if (relative.is_absolute()) {
        throw std::runtime_error(fmt::format(
            "{}: 'spirv' must resolve relative to the replay JSON", stage_scope));
    }
    for (const auto& component : relative) {
        if (component == "..") {
            throw std::runtime_error(fmt::format("{}: 'spirv' must not traverse upwards",
                                                 stage_scope));
        }
    }
    replay.spirv_path =
        (std::filesystem::path(replay_path).parent_path() / relative).string();

    std::ifstream module_file(replay.spirv_path, std::ios::binary | std::ios::ate);
    if (!module_file) {
        throw std::runtime_error(fmt::format("cannot open SPIR-V module {}", replay.spirv_path));
    }
    const std::streamoff module_size = module_file.tellg();
    if (module_size < 0 || static_cast<std::uint64_t>(module_size) > kMaxReplayModuleBytes) {
        throw std::runtime_error(fmt::format("{}: module size {} is outside the static bound",
                                             replay.spirv_path, module_size));
    }
    std::vector<unsigned char> bytes(static_cast<size_t>(module_size));
    module_file.seekg(0);
    module_file.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    if (!module_file) {
        throw std::runtime_error(fmt::format("failed reading {}", replay.spirv_path));
    }

    replay.words = DecodeSpirvWords(bytes, replay.spirv_path);
    if (replay.words.size() != spirv_words) {
        throw std::runtime_error(fmt::format(
            "{}: module has {} words but stages[0].spirv_words says {}", replay.spirv_path,
            replay.words.size(), spirv_words));
    }
    replay.capabilities = ReflectCapabilities(replay.words, replay.spirv_path, replay.context);
    const std::uint64_t hash =
        Common::CityHash64(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (hash != replay.context.spirv_hash) {
        throw std::runtime_error(fmt::format(
            "{}: CityHash64 of the module bytes is {:016x} but compile_context.spirv_hash says "
            "{:016x}",
            replay.spirv_path, hash, replay.context.spirv_hash));
    }
    if (replay.capabilities.empty()) {
        throw std::runtime_error(fmt::format("{}: module declares no OpCapability instructions",
                                             replay.spirv_path));
    }
    return replay;
}

/// Bounded capability mapping against one physical device. Every declared capability must map
/// to a known device requirement; unknown capabilities produce a skip reason instead of being
/// sent to the driver. Never enables unsupported feature bits.
std::vector<std::string> CapabilityGates(const std::vector<spv::Capability>& capabilities,
                                         const PhysicalInfo& info,
                                         ModuleRequirements& requirements) {
    std::vector<std::string> reasons;
    const auto require = [&](bool supported, const std::string& what) {
        if (!supported) {
            reasons.push_back(what);
        }
    };
    for (const spv::Capability capability : capabilities) {
        switch (capability) {
        case spv::Capability::Shader:
        case spv::Capability::SampledBuffer:
        case spv::Capability::ImageQuery:
            // Core capabilities with no Vulkan feature requirement.
            break;
        case spv::Capability::StorageImageReadWithoutFormat:
            require(info.core_features.shaderStorageImageReadWithoutFormat,
                    "module declares StorageImageReadWithoutFormat but "
                    "shaderStorageImageReadWithoutFormat is not supported");
            requirements.storage_image_read_without_format = true;
            break;
        case spv::Capability::ImageGatherExtended:
            require(info.core_features.shaderImageGatherExtended,
                    "module declares ImageGatherExtended but shaderImageGatherExtended is not "
                    "supported");
            break;
        case spv::Capability::StorageImageWriteWithoutFormat:
            require(info.core_features.shaderStorageImageWriteWithoutFormat,
                    "module declares StorageImageWriteWithoutFormat but "
                    "shaderStorageImageWriteWithoutFormat is not supported");
            requirements.storage_image_write_without_format = true;
            break;
        case spv::Capability::Int8:
            require(info.float16.shaderInt8,
                    "module declares Int8 but the shaderInt8 feature is not supported");
            requirements.int8 = true;
            break;
        case spv::Capability::Int16:
            require(info.core_features.shaderInt16,
                    "module declares Int16 but the shaderInt16 feature is not supported");
            requirements.int16 = true;
            break;
        case spv::Capability::Int64:
            require(info.core_features.shaderInt64,
                    "module declares Int64 but the shaderInt64 feature is not supported");
            requirements.int64 = true;
            break;
        case spv::Capability::Float16:
            require(info.float16.shaderFloat16,
                    "module declares Float16 but the shaderFloat16 feature is not supported");
            break;
        case spv::Capability::Float64:
            require(info.core_features.shaderFloat64,
                    "module declares Float64 but the shaderFloat64 feature is not supported");
            requirements.float64 = true;
            break;
        case spv::Capability::GroupNonUniform:
            require((info.subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0,
                    "module declares GroupNonUniform but subgroups are not supported in the "
                    "compute stage");
            require((info.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0,
                    "module declares GroupNonUniform but basic subgroup operations "
                    "are not supported");
            break;
        case spv::Capability::GroupNonUniformVote:
            require((info.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) != 0,
                    "module declares GroupNonUniformVote but subgroup vote "
                    "operations are not supported");
            break;
        case spv::Capability::GroupNonUniformArithmetic:
            require((info.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0,
                    "module declares GroupNonUniformArithmetic but subgroup "
                    "arithmetic operations are not supported");
            break;
        case spv::Capability::GroupNonUniformBallot:
            require((info.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0,
                    "module declares GroupNonUniformBallot but subgroup ballot "
                    "operations are not supported");
            break;
        case spv::Capability::GroupNonUniformShuffle:
            require((info.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0,
                    "module declares GroupNonUniformShuffle but subgroup shuffle "
                    "operations are not supported");
            break;
        case spv::Capability::GroupNonUniformShuffleRelative:
            require((info.subgroup.supportedOperations &
                     VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT) != 0,
                    "module declares GroupNonUniformShuffleRelative but relative "
                    "subgroup shuffle operations are not supported");
            break;
        case spv::Capability::VariablePointersStorageBuffer:
            require(info.variable_pointers.variablePointersStorageBuffer,
                    "module declares VariablePointersStorageBuffer but "
                    "variablePointersStorageBuffer is not supported");
            requirements.variable_pointers_storage_buffer = true;
            break;
        case spv::Capability::StorageBuffer16BitAccess:
            require(info.storage16.storageBuffer16BitAccess,
                    "module declares StorageBuffer16BitAccess but storageBuffer16BitAccess is "
                    "not supported");
            break;
        case spv::Capability::VulkanMemoryModel:
            require(info.memory_model.vulkanMemoryModel,
                    "module declares VulkanMemoryModel but the vulkanMemoryModel feature is not "
                    "supported");
            break;
        case spv::Capability::VulkanMemoryModelDeviceScope:
            require(info.memory_model.vulkanMemoryModelDeviceScope,
                    "module declares VulkanMemoryModelDeviceScope but "
                    "vulkanMemoryModelDeviceScope is not supported");
            break;
        case spv::Capability::CooperativeMatrixKHR:
            // The fixed OfflineProfile never enables cooperative lowering, so an external
            // module declaring this is outside the replay contract; skip instead of
            // duplicating the production shape and AMD-emulated-matrix guards. The
            // synthetic --mode cooperative path is unchanged.
            reasons.push_back("module declares CooperativeMatrixKHR; cooperative replay is not "
                              "supported by the offline profile");
            break;
        default:
            reasons.push_back(fmt::format(
                "module declares {} which has no bounded device mapping", CapabilityLabel(capability)));
            break;
        }
    }
    return reasons;
}

/// Device limits for a compile-only replay (capability gates have already been collected).
std::vector<std::string> CompileSkipReasons(const PhysicalInfo& info, const ReplayModule& replay,
                                            std::vector<std::string> capability_reasons) {
    std::vector<std::string> reasons = std::move(capability_reasons);
    const VkPhysicalDeviceLimits& limits = info.properties.limits;
    const CompileContext& context = replay.context;

    if (info.properties.apiVersion < VK_API_VERSION_1_2) {
        reasons.push_back(fmt::format(
            "device supports Vulkan {} but the offline profile requires Vulkan 1.2",
            ApiVersionString(info.properties.apiVersion)));
    }
    if (info.compute_family == std::numeric_limits<std::uint32_t>::max()) {
        reasons.push_back("no compute-capable queue family");
    }
    std::uint64_t invocations = 1;
    for (size_t dim = 0; dim < 3; ++dim) {
        if (context.workgroup_size[dim] > limits.maxComputeWorkGroupSize[dim]) {
            reasons.push_back(fmt::format(
                "workgroup size {} exceeds maxComputeWorkGroupSize[{}] {}",
                context.workgroup_size[dim], dim, limits.maxComputeWorkGroupSize[dim]));
        }
        invocations *= context.workgroup_size[dim];
    }
    if (invocations > limits.maxComputeWorkGroupInvocations) {
        reasons.push_back(fmt::format(
            "workgroup of {} invocations exceeds maxComputeWorkGroupInvocations {}", invocations,
            limits.maxComputeWorkGroupInvocations));
    }
    if (context.shared_memory_bytes > limits.maxComputeSharedMemorySize) {
        reasons.push_back(fmt::format(
            "shader needs {} bytes of shared memory but maxComputeSharedMemorySize is {}",
            context.shared_memory_bytes, limits.maxComputeSharedMemorySize));
    }
    if (context.push_constant_bytes > limits.maxPushConstantsSize) {
        reasons.push_back(fmt::format(
            "pipeline needs {} push-constant bytes but maxPushConstantsSize is {}",
            context.push_constant_bytes, limits.maxPushConstantsSize));
    }
    if (context.uses_warp_ops) {
        const bool force_warp_size = CanForceWarpSizeSubgroup(info);
        if (!force_warp_size && info.subgroup.subgroupSize != kWarpSize) {
            reasons.push_back("warp operations require fixed 32-lane compute subgroups");
        }
        if (force_warp_size &&
            invocations > std::uint64_t{kWarpSize} *
                              info.subgroup_size_control.maxComputeWorkgroupSubgroups) {
            reasons.push_back("workgroup exceeds maxComputeWorkgroupSubgroups at 32 lanes");
        }
    }

    // Per-stage descriptor limits; vkCreateDescriptorSetLayout would reject excess counts.
    // Uniform texel buffers share the sampled-images budget with combined image samplers,
    // storage texel buffers share the storage-images budget with storage images, combined
    // image samplers also count against the samplers budget, and everything counts against
    // maxPerStageResources.
    std::uint64_t uniform_buffers = 0;
    std::uint64_t storage_buffers = 0;
    std::uint64_t samplers = 0;
    std::uint64_t sampled_images = 0;
    std::uint64_t storage_images = 0;
    std::uint64_t total_resources = 0;
    for (const CompileContext::Binding& binding : context.bindings) {
        total_resources += binding.count;
        switch (binding.type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            uniform_buffers += binding.count;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            storage_buffers += binding.count;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            sampled_images += binding.count;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            storage_images += binding.count;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            samplers += binding.count;
            sampled_images += binding.count;
            break;
        default:
            break;
        }
    }
    const auto check = [&](std::uint64_t count, std::uint32_t limit, const std::string& what) {
        if (count > limit) {
            reasons.push_back(fmt::format("{} descriptor count {} exceeds the per-stage limit {}",
                                          what, count, limit));
        }
    };
    check(uniform_buffers, limits.maxPerStageDescriptorUniformBuffers,
          "uniform_buffer");
    check(storage_buffers, limits.maxPerStageDescriptorStorageBuffers, "storage_buffer");
    check(samplers, limits.maxPerStageDescriptorSamplers, "combined_image_sampler sampler");
    check(sampled_images, limits.maxPerStageDescriptorSampledImages,
          "sampled image (texel buffer and combined)");
    check(storage_images, limits.maxPerStageDescriptorStorageImages,
          "storage image (image and texel buffer)");
    check(total_resources, limits.maxPerStageResources, "per-stage resource");
    return reasons;
}

constexpr std::uint64_t kMaxApplicationCacheBytes = 64 * 1024 * 1024;

std::uint32_t CacheHeaderWord(const std::vector<unsigned char>& data, size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           static_cast<std::uint32_t>(data[offset + 1]) << 8 |
           static_cast<std::uint32_t>(data[offset + 2]) << 16 |
           static_cast<std::uint32_t>(data[offset + 3]) << 24;
}

std::vector<unsigned char> LoadApplicationCache(const std::string& path,
                                                const ReplayModule& replay) {
    if (path.empty()) {
        return {};
    }
    const std::filesystem::path target{path};
    auto temporary = target;
    temporary += ".tmp";
    for (const auto& candidate : {target, temporary}) {
        for (const std::filesystem::path input : {replay.source_path, replay.spirv_path}) {
            if (std::filesystem::weakly_canonical(candidate) ==
                    std::filesystem::weakly_canonical(input) ||
                (std::filesystem::exists(candidate) &&
                 std::filesystem::equivalent(candidate, input))) {
                throw std::runtime_error("application cache output must not overwrite replay inputs");
            }
        }
    }
    if (!std::filesystem::exists(target)) {
        return {};
    }
    std::ifstream file(target, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(fmt::format("cannot open application cache {}", path));
    }
    const std::streamoff size = file.tellg();
    if (size < 32 || static_cast<std::uint64_t>(size) > kMaxApplicationCacheBytes) {
        throw std::runtime_error("application cache must contain 32 bytes to 64 MiB");
    }
    std::vector<unsigned char> data(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file || CacheHeaderWord(data, 0) != 32 ||
        CacheHeaderWord(data, 4) != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
        throw std::runtime_error("application cache has an invalid version-one Vulkan header");
    }
    return data;
}

void CheckApplicationCacheDevice(const std::vector<unsigned char>& data,
                                 const VkPhysicalDeviceProperties& properties) {
    if (!data.empty() &&
        (CacheHeaderWord(data, 8) != properties.vendorID ||
         CacheHeaderWord(data, 12) != properties.deviceID ||
         std::memcmp(data.data() + 16, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0)) {
        throw UsageError("application cache belongs to another Vulkan device or driver");
    }
}

size_t SaveApplicationCache(VkDevice device, VkPipelineCache cache, const std::string& path) {
    size_t size{};
    CheckVk(vkGetPipelineCacheData(device, cache, &size, nullptr), "vkGetPipelineCacheData(size)");
    if (size < 32 || size > kMaxApplicationCacheBytes) {
        throw std::runtime_error("driver application-cache data is outside the input bounds");
    }
    std::vector<unsigned char> data(size);
    CheckVk(vkGetPipelineCacheData(device, cache, &size, data.data()), "vkGetPipelineCacheData");
    const std::filesystem::path target{path};
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error(fmt::format("cannot write application cache {}", path));
        }
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
        file.close();
        if (!file) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error("failed to write application cache");
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        const std::string message = error.message();
        std::filesystem::remove(temporary, error);
        throw std::runtime_error(fmt::format("cannot replace application cache: {}", message));
    }
    return size;
}

int RunCompilePipeline(const Options& options, Json& document) {
    // Input, schema, module and hash validation happen before any Vulkan call, so a broken
    // ICD cannot mask an input error. Replay input failures are usage errors (exit 2);
    // device-dependent gates later stay runtime errors (exit 1 or skip 77).
    ReplayModule replay;
    std::vector<unsigned char> cache_data;
    try {
        replay = LoadReplayModule(options.compile_pipeline);
        cache_data = LoadApplicationCache(options.pipeline_cache, replay);
    } catch (const std::exception& error) {
        throw UsageError(error.what());
    }
    ModuleRequirements requirements;
    requirements.uses_warp_ops = replay.context.uses_warp_ops;

    Json capabilities = Json::array();
    for (const spv::Capability capability : replay.capabilities) {
        capabilities.push_back(CapabilityLabel(capability));
    }
    Json bindings = Json::array();
    for (size_t i = 0; i < replay.context.bindings.size(); ++i) {
        const CompileContext::Binding& binding = replay.context.bindings[i];
        bindings.push_back({{"binding", i},
                            {"type", binding.type_name},
                            {"count", binding.count}});
    }
    document["compile_only"] = true;
    document["profile"] = "offline-spirv-1.4";
    document["source"] = replay.source_path;
    document["spirv_path"] = replay.spirv_path;
    document["spirv_words"] = replay.words.size();
    document["bytes"] = replay.words.size() * sizeof(std::uint32_t);
    document["capabilities"] = std::move(capabilities);
    document["context"] = {
        {"workgroup_size", replay.context.workgroup_size},
        {"shared_memory_bytes", replay.context.shared_memory_bytes},
        {"uses_warp_ops", replay.context.uses_warp_ops},
        {"uses_cooperative_matrix", replay.context.uses_cooperative_matrix},
        {"bindings", std::move(bindings)},
        {"push_constant_bytes", replay.context.push_constant_bytes},
        {"spirv_hash", fmt::format("{:016x}", replay.context.spirv_hash)},
    };
    document["case"] = {{"compile_repeats", options.compile_repeats},
                        {"required_subgroup_size", nullptr}};
    document["pipeline_build_ms"] = Json::array();
    document["pipeline_lifetime"] = "destroy_before_recreate";
    document["dispatch_gpu_ms"] = nullptr;
    document["checked_words"] = 0;
    document["mismatched_words"] = 0;
    document["validation_enabled"] = false;
    document["validation_errors"] = 0;
    document["cache_environment"] = "inherited";
    document["application_cache"] = {
        {"enabled", !options.pipeline_cache.empty()},
        {"path", options.pipeline_cache.empty() ? Json(nullptr) : Json(options.pipeline_cache)},
        {"initial_bytes", cache_data.size()},
        {"saved_bytes", 0},
    };

    ValidationState validation;
    bool validation_layer_enabled = false;
    bool debug_utils_enabled = false;
    const auto instance =
        CreateInstance(options.validation, validation_layer_enabled, debug_utils_enabled);
    const bool validation_active = validation_layer_enabled;
    document["validation_enabled"] = validation_active;
    const Scoped<VkDebugUtilsMessengerEXT, MessengerDeleter> messenger =
        CreateMessenger(*instance, validation, debug_utils_enabled);
    validation.abort_on_error.store(validation_active, std::memory_order_relaxed);

    int exit_code = ExitPass;
    try {
        const auto handles = EnumeratePhysicalDevices(*instance);
        if (handles.empty()) {
            throw std::runtime_error("no Vulkan physical devices found");
        }
        std::vector<PhysicalInfo> devices;
        devices.reserve(handles.size());
        for (const VkPhysicalDevice handle : handles) {
            devices.push_back(CollectPhysicalInfo(handle));
        }
        const std::uint32_t chosen = ChooseDeviceIndex(devices, options);
        const PhysicalInfo& info = devices[chosen];
        document["device"] = DeviceJson(info, chosen);

        std::vector<std::string> reasons =
            CapabilityGates(replay.capabilities, info, requirements);
        const std::vector<std::string> limit_reasons =
            CompileSkipReasons(info, replay, std::move(reasons));
        if (!limit_reasons.empty()) {
            document["status"] = "skipped";
            document["reason"] = Join(limit_reasons, "; ");
            document["reasons"] = limit_reasons;
            LogLine(fmt::format("skipped: {}", Join(limit_reasons, "; ")));
            std::cout << document.dump(2) << '\n';
            return ExitSkipped;
        }
        CheckApplicationCacheDevice(cache_data, info.properties);

        DevicePlan plan = CreateDevice(info, options, &requirements);
        const VkDevice device = *plan.device;
        document["case"]["required_subgroup_size"] =
            plan.force_warp_size_subgroup ? Json(kWarpSize) : Json(nullptr);
        LogLine(fmt::format("device[{}]: {} (Vulkan {}, {})", chosen, info.properties.deviceName,
                            ApiVersionString(info.properties.apiVersion),
                            info.driver.driverName));
        LogLine(fmt::format(
            "compile-only: {} SPIR-V words, workgroup {}x{}x{}, {} binding(s), shared memory {} "
            "bytes{}",
            replay.words.size(), replay.context.workgroup_size[0],
            replay.context.workgroup_size[1], replay.context.workgroup_size[2],
            replay.context.bindings.size(), replay.context.shared_memory_bytes,
            plan.force_warp_size_subgroup ? ", required 32-lane subgroups" : ""));

        // Even with the abort-on-error callback, refuse to keep compiling after any observed
        // validation error; the driver never sees the next invalid call.
        const auto validation_gate = [&](const char* what) {
            if (validation_active && validation.errors.load(std::memory_order_relaxed) != 0) {
                throw std::runtime_error(fmt::format(
                    "validation reported errors before {}; not compiling further", what));
            }
        };
        Scoped<VkPipelineCache, std::function<void(VkPipelineCache)>> application_cache;
        if (!options.pipeline_cache.empty()) {
            validation_gate("vkCreatePipelineCache");
            const VkPipelineCacheCreateInfo cache_ci{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .initialDataSize = cache_data.size(),
                .pInitialData = cache_data.empty() ? nullptr : cache_data.data(),
            };
            VkPipelineCache cache = VK_NULL_HANDLE;
            CheckVk(vkCreatePipelineCache(device, &cache_ci, nullptr, &cache),
                    "vkCreatePipelineCache");
            application_cache = Scoped<VkPipelineCache, std::function<void(VkPipelineCache)>>{
                cache, [device](VkPipelineCache owned) {
                    vkDestroyPipelineCache(device, owned, nullptr);
                }};
        }

        Scoped<VkShaderModule, std::function<void(VkShaderModule)>> shader_module;
        {
            validation_gate("vkCreateShaderModule");
            VkShaderModuleCreateInfo shader_create_info{
                VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            shader_create_info.codeSize = replay.words.size() * sizeof(std::uint32_t);
            shader_create_info.pCode = replay.words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            CheckVk(vkCreateShaderModule(device, &shader_create_info, nullptr, &module),
                    "vkCreateShaderModule");
            shader_module = Scoped<VkShaderModule, std::function<void(VkShaderModule)>>{
                module, [device](VkShaderModule owned) {
                    vkDestroyShaderModule(device, owned, nullptr);
                }};
        }

        // Ordinary descriptor-set layout: set 0, sequential binding order, compute stage.
        std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
        layout_bindings.reserve(replay.context.bindings.size());
        for (size_t i = 0; i < replay.context.bindings.size(); ++i) {
            const CompileContext::Binding& binding = replay.context.bindings[i];
            layout_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = static_cast<std::uint32_t>(i),
                .descriptorType = binding.type,
                .descriptorCount = binding.count,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .pImmutableSamplers = nullptr,
            });
        }
        validation_gate("vkCreateDescriptorSetLayout");
        VkDescriptorSetLayoutCreateInfo set_layout_create_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_layout_create_info.bindingCount =
            static_cast<std::uint32_t>(layout_bindings.size());
        set_layout_create_info.pBindings = layout_bindings.data();
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        CheckVk(vkCreateDescriptorSetLayout(device, &set_layout_create_info, nullptr, &set_layout),
                "vkCreateDescriptorSetLayout");
        const auto destroy_set_layout = [device](VkDescriptorSetLayout owned) {
            vkDestroyDescriptorSetLayout(device, owned, nullptr);
        };
        Scoped<VkDescriptorSetLayout, decltype(destroy_set_layout)> set_layout_scoped{
            set_layout, destroy_set_layout};

        // Production push-constant range (pipeline_helper.h CreatePipelineLayout).
        validation_gate("vkCreatePipelineLayout");
        const VkPushConstantRange push_constant_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = replay.context.push_constant_bytes,
        };
        const VkDescriptorSetLayout set_layout_handle = *set_layout_scoped;
        VkPipelineLayoutCreateInfo layout_create_info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_create_info.setLayoutCount = layout_bindings.empty() ? 0U : 1U;
        layout_create_info.pSetLayouts = layout_bindings.empty() ? nullptr : &set_layout_handle;
        layout_create_info.pushConstantRangeCount = 1;
        layout_create_info.pPushConstantRanges = &push_constant_range;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        CheckVk(vkCreatePipelineLayout(device, &layout_create_info, nullptr, &pipeline_layout),
                "vkCreatePipelineLayout");
        const auto destroy_pipeline_layout = [device](VkPipelineLayout owned) {
            vkDestroyPipelineLayout(device, owned, nullptr);
        };
        Scoped<VkPipelineLayout, decltype(destroy_pipeline_layout)> pipeline_layout_scoped{
            pipeline_layout, destroy_pipeline_layout};

        // Match production's required warp size. The offline profile has no cooperative stages.
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required_subgroup_size_info{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        required_subgroup_size_info.requiredSubgroupSize = kWarpSize;
        validation_gate("vkCreateComputePipelines");
        VkComputePipelineCreateInfo pipeline_create_info{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_create_info.stage = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = plan.force_warp_size_subgroup ? &required_subgroup_size_info : nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *shader_module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        };
        pipeline_create_info.layout = pipeline_layout;
        pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
        pipeline_create_info.basePipelineIndex = 0;

        // The application cache, when requested, outlives every pipeline recreation.
        const auto destroy_pipeline = [device](VkPipeline owned) {
            vkDestroyPipeline(device, owned, nullptr);
        };
        Scoped<VkPipeline, std::function<void(VkPipeline)>> pipeline;
        std::vector<double> pipeline_build_ms;
        for (std::uint32_t repeat = 0; repeat < options.compile_repeats; ++repeat) {
            pipeline.Reset();
            VkPipeline created = VK_NULL_HANDLE;
            const auto build_start = std::chrono::steady_clock::now();
            CheckVk(vkCreateComputePipelines(device, *application_cache, 1, &pipeline_create_info,
                                             nullptr, &created),
                    "vkCreateComputePipelines");
            const auto build_end = std::chrono::steady_clock::now();
            pipeline_build_ms.push_back(
                std::chrono::duration<double, std::milli>(build_end - build_start).count());
            document["pipeline_build_ms"] = pipeline_build_ms;
            pipeline = Scoped<VkPipeline, std::function<void(VkPipeline)>>{created,
                                                                           destroy_pipeline};
        }
        LogPipelineBuildTimes(pipeline_build_ms);
        if (!options.pipeline_cache.empty()) {
            validation_gate("vkGetPipelineCacheData");
            document["application_cache"]["saved_bytes"] =
                SaveApplicationCache(device, *application_cache, options.pipeline_cache);
        }
    } catch (...) {
        // Scoped handles above already unwound; record the true validation counts before
        // main() prints the document.
        document["validation_errors"] =
            validation_active ? validation.errors.load(std::memory_order_relaxed) : 0;
        throw;
    }

    // Post-teardown validation sampling: the device is gone, so remaining errors are final.
    const std::uint32_t validation_errors = validation.errors.load(std::memory_order_relaxed);
    document["validation_errors"] = validation_active ? validation_errors : 0;
    if (validation_active) {
        LogLine(fmt::format("validation: {} error(s), {} warning(s)", validation_errors,
                            validation.warnings.load(std::memory_order_relaxed)));
    }
    const auto rss = PeakRssKib();
    document["process_peak_rss_kib"] = rss ? Json(*rss) : Json(nullptr);
    LogLine("compile-only replay finished: no dispatch, no numerical correctness claim");
    if (validation_active && validation_errors != 0) {
        document["status"] = "error";
        document["error"] = {{"message", fmt::format(
            "Vulkan validation reported {} error(s)", validation_errors)}};
        exit_code = ExitFailure;
    } else {
        document["status"] = "ok";
    }
    std::cout << document.dump(2) << '\n';
    return exit_code;
}

// -----------------------------------------------------------------------------
// Run paths
// -----------------------------------------------------------------------------

/// --emit-only: module + JSON metadata for offline validation. No Vulkan instance is created
/// and no device query happens; CLI conflicts are rejected in ParseOptions before emission.
int RunEmitOnly(const Options& options, const TB::ShaderModule& shader, Json& document) {
    WriteSpirv(options.emit_spirv, shader.words);
    document["emit_only"] = true;
    document["spirv_path"] = options.emit_spirv;
    document["device"] = nullptr;
    document["pipeline_build_ms"] = nullptr;
    document["dispatch_gpu_ms"] = nullptr;
    document["checked_words"] = 0;
    document["mismatched_words"] = 0;
    document["validation_enabled"] = false;
    document["validation_errors"] = 0;
    document["status"] = "ok";
    const auto rss = PeakRssKib();
    document["process_peak_rss_kib"] = rss ? Json(*rss) : Json(nullptr);
    std::cout << document.dump(2) << '\n';
    LogLine(fmt::format("emitted {} SPIR-V words ({} bytes, {} bytes shared memory) to {}",
                        shader.words.size(), shader.words.size() * sizeof(std::uint32_t),
                        shader.shared_memory_bytes, options.emit_spirv));
    return ExitPass;
}

int RunListDevices(const Options& options, Json& document) {
    bool validation_layer_enabled = false;
    bool debug_utils_enabled = false;
    const auto instance =
        CreateInstance(options.validation, validation_layer_enabled, debug_utils_enabled);
    const auto handles = EnumeratePhysicalDevices(*instance);
    Json devices = Json::array();
    for (std::uint32_t i = 0; i < handles.size(); ++i) {
        devices.push_back(DeviceJson(CollectPhysicalInfo(handles[i]), i));
    }
    document["devices"] = std::move(devices);
    document["status"] = "ok";
    std::cout << document.dump(2) << '\n';
    return ExitPass;
}

int RunVulkan(const Options& options, const TB::Parameters& parameters,
              const TB::ShaderModule& shader, Json& document) {
    document["pipeline_build_ms"] = Json::array();
    document["dispatch_gpu_ms"] = nullptr;
    document["checked_words"] = 0;
    document["mismatched_words"] = 0;
    document["validation_enabled"] = false;
    document["validation_errors"] = 0;
    document["cache_environment"] = "inherited";
    // The module is written before any Vulkan setup, so an unwritable path fails fast.
    if (!options.emit_spirv.empty()) {
        WriteSpirv(options.emit_spirv, shader.words);
        document["spirv_path"] = options.emit_spirv;
    }

    ValidationState validation;
    bool validation_layer_enabled = false;
    bool debug_utils_enabled = false;
    const auto instance =
        CreateInstance(options.validation, validation_layer_enabled, debug_utils_enabled);
    const bool validation_active = validation_layer_enabled;
    document["validation_enabled"] = validation_active;
    const Scoped<VkDebugUtilsMessengerEXT, MessengerDeleter> messenger =
        CreateMessenger(*instance, validation, debug_utils_enabled);

    const auto handles = EnumeratePhysicalDevices(*instance);
    if (handles.empty()) {
        throw std::runtime_error("no Vulkan physical devices found");
    }
    std::vector<PhysicalInfo> devices;
    devices.reserve(handles.size());
    for (const VkPhysicalDevice handle : handles) {
        devices.push_back(CollectPhysicalInfo(handle));
    }
    const std::uint32_t chosen = ChooseDeviceIndex(devices, options);
    const PhysicalInfo& info = devices[chosen];
    document["device"] = DeviceJson(info, chosen);

    const std::vector<std::string> reasons = SkipReasons(*instance, info, options, shader);
    if (!reasons.empty()) {
        document["status"] = "skipped";
        document["reason"] = Join(reasons, "; ");
        document["reasons"] = reasons;
        LogLine(fmt::format("skipped: {}", Join(reasons, "; ")));
        std::cout << document.dump(2) << '\n';
        return ExitSkipped;
    }

    DevicePlan plan = CreateDevice(info, options);
    const VkDevice device = *plan.device;
    document["case"] = CaseJson(options, parameters, plan.force_warp_size_subgroup
                                                    ? std::optional<std::uint32_t>(kWarpSize)
                                                    : std::nullopt);
    LogLine(fmt::format("device[{}]: {} (Vulkan {}, {})", chosen, info.properties.deviceName,
                        ApiVersionString(info.properties.apiVersion),
                        info.driver.driverName));
    LogLine(fmt::format(
        "case: {} mode, {} pattern, depth {}, operations {}, local size {}, {} workgroups{}",
        ModeName(options.mode), PatternName(options.pattern), parameters.depth,
        parameters.operations, parameters.local_size, options.workgroups,
        plan.force_warp_size_subgroup ? ", required 32-lane subgroups" : ""));

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, plan.queue_family, 0, &queue);

    // Fixture and storage: 10 words per invocation, 8 input words and 2 output words.
    const std::uint32_t invocations = options.workgroups * options.local_size;
    const std::uint64_t total_words =
        static_cast<std::uint64_t>(invocations) * TB::WordsPerInvocation;
    const TB::Fixture fixture = TB::MakeFixture(parameters, options.workgroups, options.seed);
    if (fixture.input.size() != total_words || fixture.expected.size() != total_words) {
        throw std::runtime_error(fmt::format(
            "fixture has {} input / {} expected words but the case needs {} per invocation",
            fixture.input.size(), fixture.expected.size(), total_words));
    }
    const std::uint64_t buffer_bytes = total_words * sizeof(std::uint32_t);
    HostBuffer host_buffer(device, info.handle, static_cast<VkDeviceSize>(buffer_bytes));
    std::memcpy(host_buffer.Mapped(), fixture.input.data(), static_cast<size_t>(buffer_bytes));
    host_buffer.Flush();

    Scoped<VkShaderModule, std::function<void(VkShaderModule)>> shader_module;
    {
        VkShaderModuleCreateInfo shader_create_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_create_info.codeSize = shader.words.size() * sizeof(std::uint32_t);
        shader_create_info.pCode = shader.words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        CheckVk(vkCreateShaderModule(device, &shader_create_info, nullptr, &module),
                "vkCreateShaderModule");
        shader_module = Scoped<VkShaderModule, std::function<void(VkShaderModule)>>{
            module, [device](VkShaderModule owned) { vkDestroyShaderModule(device, owned, nullptr); }};
    }

    const VkDescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    };
    VkDescriptorSetLayoutCreateInfo set_layout_create_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout_create_info.bindingCount = 1;
    set_layout_create_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    CheckVk(vkCreateDescriptorSetLayout(device, &set_layout_create_info, nullptr, &set_layout),
            "vkCreateDescriptorSetLayout");
    const auto destroy_set_layout = [device](VkDescriptorSetLayout owned) {
        vkDestroyDescriptorSetLayout(device, owned, nullptr);
    };
    Scoped<VkDescriptorSetLayout, decltype(destroy_set_layout)> set_layout_scoped{
        set_layout, destroy_set_layout};

    const VkDescriptorPoolSize pool_size{
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo pool_create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_create_info.maxSets = 1;
    pool_create_info.poolSizeCount = 1;
    pool_create_info.pPoolSizes = &pool_size;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    CheckVk(vkCreateDescriptorPool(device, &pool_create_info, nullptr, &descriptor_pool),
            "vkCreateDescriptorPool");
    const auto destroy_descriptor_pool = [device](VkDescriptorPool owned) {
        vkDestroyDescriptorPool(device, owned, nullptr);
    };
    Scoped<VkDescriptorPool, decltype(destroy_descriptor_pool)> descriptor_pool_scoped{
        descriptor_pool, destroy_descriptor_pool};

    VkDescriptorSetAllocateInfo set_allocate_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_allocate_info.descriptorPool = descriptor_pool;
    set_allocate_info.descriptorSetCount = 1;
    const VkDescriptorSetLayout set_layout_handle = *set_layout_scoped;
    set_allocate_info.pSetLayouts = &set_layout_handle;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    CheckVk(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set),
            "vkAllocateDescriptorSets");
    // Freed with the descriptor pool.

    const VkDescriptorBufferInfo buffer_info{
        .buffer = host_buffer.Get(),
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    const VkWriteDescriptorSet descriptor_write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pImageInfo = nullptr,
        .pBufferInfo = &buffer_info,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);

    VkPipelineLayoutCreateInfo layout_create_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_create_info.setLayoutCount = 1;
    layout_create_info.pSetLayouts = &set_layout_handle;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    CheckVk(vkCreatePipelineLayout(device, &layout_create_info, nullptr, &pipeline_layout),
            "vkCreatePipelineLayout");
    const auto destroy_pipeline_layout = [device](VkPipelineLayout owned) {
        vkDestroyPipelineLayout(device, owned, nullptr);
    };
    Scoped<VkPipelineLayout, decltype(destroy_pipeline_layout)> pipeline_layout_scoped{
        pipeline_layout, destroy_pipeline_layout};

    // Match production: cooperative matrices require full, explicitly sized guest warps.
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required_subgroup_size_info{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    required_subgroup_size_info.requiredSubgroupSize = kWarpSize;
    VkComputePipelineCreateInfo pipeline_create_info{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_create_info.stage = VkPipelineShaderStageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = plan.force_warp_size_subgroup ? &required_subgroup_size_info : nullptr,
        .flags = options.mode == TB::Mode::Cooperative
                     ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0U,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = *shader_module,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };
    pipeline_create_info.layout = pipeline_layout;
    pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
    pipeline_create_info.basePipelineIndex = 0;

    // Compile repeats: VkPipelineCache-less creations, each timed with steady_clock. The
    // driver's internal caches are inherited; samples are chronological first/warm times, not
    // a controlled cold cache, and no user cache is cleared.
    const auto destroy_pipeline = [device](VkPipeline owned) {
        vkDestroyPipeline(device, owned, nullptr);
    };
    Scoped<VkPipeline, std::function<void(VkPipeline)>> pipeline;
    std::vector<double> pipeline_build_ms;
    for (std::uint32_t repeat = 0; repeat < options.compile_repeats; ++repeat) {
        VkPipeline created = VK_NULL_HANDLE;
        const auto build_start = std::chrono::steady_clock::now();
        CheckVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info,
                                         nullptr, &created),
                "vkCreateComputePipelines");
        const auto build_end = std::chrono::steady_clock::now();
        pipeline_build_ms.push_back(
            std::chrono::duration<double, std::milli>(build_end - build_start).count());
        document["pipeline_build_ms"] = pipeline_build_ms;
        pipeline = Scoped<VkPipeline, std::function<void(VkPipeline)>>{created, destroy_pipeline};
    }
    LogPipelineBuildTimes(pipeline_build_ms);

    // Timestamps: only trusted when the compute queue family reports valid bits and a
    // positive period; otherwise dispatch_gpu_ms stays null with a reason.
    const float timestamp_period = info.properties.limits.timestampPeriod;
    const std::uint32_t timestamp_valid_bits = info.compute_timestamp_valid_bits;
    const bool timestamps_usable =
        timestamp_period > 0.0F && timestamp_valid_bits > 0 && queue != VK_NULL_HANDLE;
    Scoped<VkQueryPool, std::function<void(VkQueryPool)>> query_pool;
    std::string timestamps_reason;
    if (timestamps_usable) {
        VkQueryPoolCreateInfo query_pool_create_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_pool_create_info.queryCount = 2 * options.iterations;
        VkQueryPool pool = VK_NULL_HANDLE;
        CheckVk(vkCreateQueryPool(device, &query_pool_create_info, nullptr, &pool),
                "vkCreateQueryPool");
        const auto destroy_query_pool = [device](VkQueryPool owned) {
            vkDestroyQueryPool(device, owned, nullptr);
        };
        query_pool = Scoped<VkQueryPool, std::function<void(VkQueryPool)>>{pool, destroy_query_pool};
    } else {
        timestamps_reason = fmt::format(
            "compute queue family reports {} timestamp valid bits and {} ns timestamp period",
            timestamp_valid_bits, timestamp_period);
    }

    VkCommandPoolCreateInfo pool_create_info_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_create_info_ci.queueFamilyIndex = plan.queue_family;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    CheckVk(vkCreateCommandPool(device, &pool_create_info_ci, nullptr, &command_pool),
            "vkCreateCommandPool");
    const auto destroy_command_pool = [device](VkCommandPool owned) {
        vkDestroyCommandPool(device, owned, nullptr);
    };
    Scoped<VkCommandPool, decltype(destroy_command_pool)> command_pool_scoped{command_pool,
                                                                              destroy_command_pool};

    VkCommandBufferAllocateInfo command_allocate_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_allocate_info.commandPool = command_pool;
    command_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate_info.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    CheckVk(vkAllocateCommandBuffers(device, &command_allocate_info, &command_buffer),
            "vkAllocateCommandBuffers");
    // Freed with the command pool.

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(command_buffer, &begin_info), "vkBeginCommandBuffer");
    if (*query_pool) {
        vkCmdResetQueryPool(command_buffer, *query_pool, 0, 2 * options.iterations);
    }
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0,
                            1, &descriptor_set, 0, nullptr);
    const auto barrier = [&](VkPipelineStageFlags source_stage, VkPipelineStageFlags target_stage,
                             VkAccessFlags source_access, VkAccessFlags target_access) {
        VkMemoryBarrier memory_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memory_barrier.srcAccessMask = source_access;
        memory_barrier.dstAccessMask = target_access;
        vkCmdPipelineBarrier(command_buffer, source_stage, target_stage, 0, 1, &memory_barrier, 0,
                             nullptr, 0, nullptr);
    };
    for (std::uint32_t iteration = 0; iteration < options.iterations; ++iteration) {
        if (iteration == 0) {
            // Host writes become visible to the shader before the first dispatch.
            barrier(VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_HOST_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        } else {
            // Each dispatch is self-contained, but the barrier serializes iterations so the
            // timestamp brackets are exact and output writes never overlap.
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        if (*query_pool) {
            vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, *query_pool,
                                2 * iteration);
        }
        vkCmdDispatch(command_buffer, options.workgroups, 1, 1);
        if (*query_pool) {
            vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, *query_pool,
                                2 * iteration + 1);
        }
    }
    // Results become host-visible after the last dispatch.
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    CheckVk(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

    VkFenceCreateInfo fence_create_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CheckVk(vkCreateFence(device, &fence_create_info, nullptr, &fence), "vkCreateFence");
    const auto destroy_fence = [device](VkFence owned) { vkDestroyFence(device, owned, nullptr); };
    Scoped<VkFence, decltype(destroy_fence)> fence_scoped{fence, destroy_fence};

    const VkCommandBuffer submitted = command_buffer;
    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &submitted;
    CheckVk(vkQueueSubmit(queue, 1, &submit_info, fence), "vkQueueSubmit");
    const VkResult wait_result =
        vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs);
    if (wait_result == VK_TIMEOUT) {
        const auto message = fmt::format("GPU execution exceeded the {:.0f} s wait limit",
                                         kFenceTimeoutNs / 1'000'000'000.0);
        document["status"] = "error";
        document["error"] = {{"code", "gpu_timeout"}, {"message", message}};
        std::cout << document.dump(2) << '\n' << std::flush;
        LogLine(message);
        std::fflush(stderr);
        // Pending work still owns these resources; process exit avoids invalid Vulkan teardown.
        std::_Exit(ExitFailure);
    }
    CheckVk(wait_result, "vkWaitForFences");

    // Timestamp fetch happens only after the fence proves completion; the 64-bit fetch
    // without VK_QUERY_RESULT_WAIT_BIT therefore cannot block indefinitely.
    std::optional<std::vector<double>> dispatch_gpu_ms;
    if (*query_pool) {
        std::vector<std::uint64_t> ticks(2 * options.iterations);
        const VkResult query_result = vkGetQueryPoolResults(
            device, *query_pool, 0, 2 * options.iterations,
            ticks.size() * sizeof(std::uint64_t), ticks.data(), sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT);
        if (query_result == VK_NOT_READY) {
            timestamps_reason = "timestamp queries not ready after the fence completed";
        } else {
            CheckVk(query_result, "vkGetQueryPoolResults");
            const std::uint64_t valid_bits_mask = timestamp_valid_bits >= 64
                                                      ? ~std::uint64_t{0}
                                                      : ((std::uint64_t{1} << timestamp_valid_bits) - 1);
            std::vector<double> milliseconds;
            milliseconds.reserve(options.iterations);
            for (std::uint32_t iteration = 0; iteration < options.iterations; ++iteration) {
                const std::uint64_t start = ticks[2 * iteration] & valid_bits_mask;
                const std::uint64_t end = ticks[2 * iteration + 1] & valid_bits_mask;
                const std::uint64_t delta = (end - start) & valid_bits_mask;
                milliseconds.push_back(static_cast<double>(delta) *
                                       static_cast<double>(timestamp_period) / 1'000'000.0);
            }
            dispatch_gpu_ms = std::move(milliseconds);
        }
    }

    host_buffer.Invalidate();
    const auto* device_words = static_cast<const std::uint32_t*>(host_buffer.Mapped());
    // The eight input words per invocation are compared bit-exact so corrupted inputs can
    // never hide behind the FP signed-zero tolerance; EquivalentWord applies only to the two
    // output words (offsets 8 and 9 of each invocation).
    std::uint64_t mismatched_words = 0;
    std::optional<std::uint64_t> first_mismatch;
    bool first_mismatch_is_input = false;
    for (std::uint64_t word = 0; word < total_words; ++word) {
        const std::uint64_t offset_in_invocation = word % TB::WordsPerInvocation;
        const bool is_input = offset_in_invocation < kInputWordsPerInvocation;
        const bool matches =
            is_input ? device_words[word] == fixture.expected[word]
                     : TB::EquivalentWord(device_words[word], fixture.expected[word]);
        if (!matches) {
            ++mismatched_words;
            if (!first_mismatch) {
                first_mismatch = word;
                first_mismatch_is_input = is_input;
            }
        }
    }
    document["checked_words"] = total_words;
    document["mismatched_words"] = mismatched_words;
    if (dispatch_gpu_ms) {
        document["dispatch_gpu_ms"] = *dispatch_gpu_ms;
        document["timestamp_period_ns"] = timestamp_period;
        document["timestamp_valid_bits"] = timestamp_valid_bits;
    } else {
        document["dispatch_gpu_ms"] = nullptr;
        document["dispatch_gpu_ms_reason"] = timestamps_reason;
    }
    // Include resource teardown in the validation verdict, after the fence proved completion.
    fence_scoped.Reset();
    command_pool_scoped.Reset();
    query_pool.Reset();
    pipeline.Reset();
    pipeline_layout_scoped.Reset();
    descriptor_pool_scoped.Reset();
    set_layout_scoped.Reset();
    shader_module.Reset();
    host_buffer.Release();
    plan.device.Reset();
    const std::uint32_t validation_errors = validation.errors.load(std::memory_order_relaxed);
    document["validation_errors"] = validation_active ? validation_errors : 0;

    const std::string mismatch_detail = first_mismatch
                                            ? fmt::format(
                                                  " (first mismatch at word {}, {} word {} of "
                                                  "invocation {})",
                                                  *first_mismatch,
                                                  first_mismatch_is_input ? "input" : "output",
                                                  *first_mismatch % TB::WordsPerInvocation,
                                                  *first_mismatch / TB::WordsPerInvocation)
                                            : std::string{};
    LogLine(fmt::format("verify: {} of {} words equivalent{}", total_words - mismatched_words,
                        total_words, mismatch_detail));
    if (dispatch_gpu_ms) {
        LogLine(fmt::format("dispatch: {} iteration(s), median {:.4f} ms GPU time (queue timestamps)",
                            options.iterations, Median(*dispatch_gpu_ms)));
    } else {
        LogLine(fmt::format("dispatch GPU timing unavailable: {}", timestamps_reason));
    }
    if (validation_active) {
        LogLine(fmt::format("validation: {} error(s), {} warning(s)", validation_errors,
                            validation.warnings.load(std::memory_order_relaxed)));
    }

    const auto rss = PeakRssKib();
    document["process_peak_rss_kib"] = rss ? Json(*rss) : Json(nullptr);

    int exit_code = ExitPass;
    if (mismatched_words != 0) {
        document["status"] = "error";
        document["error"] = {{"message",
            fmt::format("{} of {} words mismatched{}", mismatched_words, total_words,
                        mismatch_detail)}};
        exit_code = ExitFailure;
    } else if (validation_active && validation_errors != 0) {
        document["status"] = "error";
        document["error"] = {{"message", fmt::format(
            "Vulkan validation reported {} error(s)", validation_errors)}};
        exit_code = ExitFailure;
    } else {
        document["status"] = "ok";
    }
    std::cout << document.dump(2) << '\n';
    return exit_code;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            PrintHelp();
            return ExitPass;
        }
    }

    Json document{{"schema_version", 1}};
    try {
        const Options options = ParseOptions(argc, argv);
        if (!options.compile_pipeline.empty()) {
            // Compile-only replay: validated input must fail before synthetic emission or any
            // Vulkan call, so an emission bug or broken ICD cannot mask the input error.
            return RunCompilePipeline(options, document);
        }
        const TB::Parameters parameters{
            .mode = options.mode,
            .pattern = options.pattern,
            .depth = options.depth,
            .operations = options.operations,
            .local_size = options.local_size,
        };
        document["case"] = CaseJson(options, parameters, std::nullopt);

        // Emission is timed with steady_clock, separately from driver compilation, and runs
        // after CLI validation but before any device query in every path.
        const auto emission_start = std::chrono::steady_clock::now();
        const TB::ShaderModule shader = TB::EmitShader(parameters);
        const double emission_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                       emission_start)
                .count();
        document["shader"] = ShaderJson(shader, emission_ms);

        if (options.emit_only) {
            return RunEmitOnly(options, shader, document);
        }
        if (options.list_devices) {
            return RunListDevices(options, document);
        }
        return RunVulkan(options, parameters, shader, document);
    } catch (const UsageError& error) {
        document["status"] = "error";
        document["error"] = {{"message", error.what()}};
        std::cout << document.dump(2) << '\n';
        fmt::print(stderr, "tensor-bench: {}\n", error.what());
        return ExitUsage;
    } catch (const std::exception& error) {
        document["status"] = "error";
        document["error"] = {{"message", error.what()}};
        std::cout << document.dump(2) << '\n';
        fmt::print(stderr, "tensor-bench: {}\n", error.what());
        return ExitFailure;
    }
}
