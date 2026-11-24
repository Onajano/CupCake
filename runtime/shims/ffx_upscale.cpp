#include "ffx_upscale.h"

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "d3d12_com.h"

namespace rt::ffx_upscale {

namespace {

// FfxApiSurfaceFormat for a VkFormat, as ffxApiGetSurfaceFormatVK maps them (FidelityFX SDK 2.4.0
// api/include/vk/ffx_api_vk.h; the enum is sequential from UNKNOWN = 0 in api/include/ffx_api_types.h).
std::uint32_t surface_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32G32B32A32_UINT: return 2;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 3;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 4;
        case VK_FORMAT_R32G32B32_SFLOAT: return 5;
        case VK_FORMAT_R32G32_SFLOAT: return 6;
        case VK_FORMAT_R8_UINT: case VK_FORMAT_S8_UINT: return 7;
        case VK_FORMAT_R32_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: return 8;
        case VK_FORMAT_R8G8B8A8_UNORM: return 10;
        case VK_FORMAT_R8G8B8A8_SNORM: return 11;
        case VK_FORMAT_R8G8B8A8_SRGB: return 12;
        case VK_FORMAT_B8G8R8A8_UNORM: return 14;
        case VK_FORMAT_B8G8R8A8_SRGB: return 15;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 16;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 17;
        case VK_FORMAT_R16G16_SFLOAT: return 18;
        case VK_FORMAT_R16G16_UINT: return 19;
        case VK_FORMAT_R16G16_SINT: return 20;
        case VK_FORMAT_R16_SFLOAT: return 21;
        case VK_FORMAT_R16_UINT: return 22;
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT: return 23;
        case VK_FORMAT_R16_SNORM: return 24;
        case VK_FORMAT_R8_UNORM: return 25;
        case VK_FORMAT_R8G8_UNORM: return 26;
        case VK_FORMAT_R8G8_UINT: return 27;
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 28;
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return 29;
        default: return 0;
    }
}

bool depth_format(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D16_UNORM_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
           format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

struct DeviceRecord {
    leonida::VulkanContext* context = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
};

struct Entry {
    VkDevice device = VK_NULL_HANDLE;
    std::mutex mutex;  // one replay at a time records through the context, and destroy waits for it
    std::unique_ptr<leonida::ffx_vk::Upscaler> upscaler;
};

std::mutex g_mutex;
std::map<VkDevice, DeviceRecord> g_devices;
// AMD's loader without a context, for context-less queries and configures; loaded on first use.
std::mutex g_loose_mutex;
leonida::ffx_vk::Upscaler g_loose;
bool g_loose_loaded = false;
std::map<std::uint64_t, std::shared_ptr<Entry>> g_contexts;
std::uint64_t g_next = 1;

std::shared_ptr<Entry> find(std::uint64_t context) {
    std::lock_guard lock(g_mutex);
    auto it = g_contexts.find(context);
    return it == g_contexts.end() ? nullptr : it->second;
}

// Waits until nothing submitted on `record`'s device can still use the upscalers, then destroys them. The submit
// lock comes first, then each entry's own lock: a replay holds the entry's lock only while recording.
void destroy_entries(VkDevice device, const DeviceRecord& record, std::vector<std::shared_ptr<Entry>> entries) {
    if (entries.empty()) return;
    {
        // A replay records an upscale and submits it under the tracker lock: taking it first means a batch recorded
        // with these upscalers has been submitted, so the wait covers it.
        std::lock_guard tracker(record.context->tracker_mutex());
        std::lock_guard submit(record.context->submit_mutex());
        record.context->vk.vkDeviceWaitIdle(device);
    }
    for (auto& entry : entries) {
        std::lock_guard lock(entry->mutex);
        entry->upscaler.reset();
    }
}

ffx::Resource resource(const leonida::UpscaleImage& image, bool output) {
    ffx::Resource r{};
    if (!image.image) return r;
    r.resource = reinterpret_cast<void*>(image.image);
    std::uint32_t usage = output ? ffx::RESOURCE_USAGE_UAV : ffx::RESOURCE_USAGE_READ_ONLY;
    if (depth_format(image.format)) usage |= ffx::RESOURCE_USAGE_DEPTHTARGET;
    r.description = {ffx::RESOURCE_TYPE_TEXTURE2D, surface_format(image.format), image.width, image.height, 1, 1, 0,
                     usage};
    // The state names the layout replay put the image in: GENERAL is the UAV state, the read-only layouts are reads.
    r.state = image.layout == VK_IMAGE_LAYOUT_GENERAL ? ffx::RESOURCE_STATE_UNORDERED_ACCESS
                                                      : ffx::RESOURCE_STATE_COMPUTE_READ;
    return r;
}

bool record(VkCommandBuffer command_buffer, const leonida::Upscale& u,
            const leonida::UpscaleImage (&images)[leonida::upscale_image_count]) {
    std::shared_ptr<Entry> entry = find(u.context);
    if (!entry) return false;
    for (std::size_t k = 0; k < 4; ++k)   // the required images need a format the upscaler knows
        if (!surface_format(images[k].format)) return false;
    ffx::DispatchDescUpscale d{};
    d.header.type = ffx::DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = command_buffer;
    d.color = resource(images[0], false);
    d.depth = resource(images[1], false);
    d.motionVectors = resource(images[2], false);
    d.output = resource(images[3], true);
    d.exposure = resource(images[4], false);
    d.reactive = resource(images[5], false);
    d.transparencyAndComposition = resource(images[6], false);
    d.jitterOffset = {u.jitter_x, u.jitter_y};
    d.motionVectorScale = {u.motion_vector_scale_x, u.motion_vector_scale_y};
    d.renderSize = {u.render_width, u.render_height};
    d.upscaleSize = {u.upscale_width, u.upscale_height};
    d.enableSharpening = u.enable_sharpening;
    d.sharpness = u.sharpness;
    d.frameTimeDelta = u.frame_time_delta;
    d.preExposure = u.pre_exposure;
    d.reset = u.reset;
    d.cameraNear = u.camera_near;
    d.cameraFar = u.camera_far;
    d.cameraFovAngleVertical = u.camera_fov_vertical;
    d.viewSpaceToMetersFactor = u.view_space_to_meters;
    d.flags = u.flags;
    std::lock_guard lock(entry->mutex);
    return entry->upscaler && entry->upscaler->dispatch(d) == ffx::RETURN_OK;
}

}  // namespace

bool enabled() { return leonida::ffx_vk::pc_upscaler_enabled(); }

void install(leonida::VulkanContext& context, VkDevice device, VkPhysicalDevice physical,
             PFN_vkGetDeviceProcAddr get_device_proc) {
    {
        std::lock_guard lock(g_mutex);
        g_devices[device] = {&context, physical, get_device_proc};
    }
    context.set_upscaler([](std::uint64_t id) { return find(id) != nullptr; }, &record);
}

void uninstall(VkDevice device) {
    DeviceRecord record;
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(device);
        if (it == g_devices.end()) return;
        record = it->second;
        g_devices.erase(it);
        for (auto c = g_contexts.begin(); c != g_contexts.end();) {
            if (c->second->device == device) {
                entries.push_back(std::move(c->second));
                c = g_contexts.erase(c);
            } else {
                ++c;
            }
        }
    }
    destroy_entries(device, record, std::move(entries));
}

std::uint64_t create(ID3D12Device* device, const ffx::CreateContextDescUpscale& desc, std::string& error) {
    if (!enabled()) {
        error = "the PC upscaler is off (LEONIDA_FFX_PC)";
        return 0;
    }
    const rt::d3d12::VulkanDeviceInfo* info = rt::d3d12::vulkan_device_info(device);
    DeviceRecord record;
    {
        std::lock_guard lock(g_mutex);
        auto it = info ? g_devices.find(info->device) : g_devices.end();
        if (it == g_devices.end()) {
            error = "not a device of the Vulkan layer created with the PC upscaler on";
            return 0;
        }
        record = it->second;
    }
    auto entry = std::make_shared<Entry>();
    entry->device = info->device;
    entry->upscaler = std::make_unique<leonida::ffx_vk::Upscaler>();
    if (!entry->upscaler->load(error)) return 0;
    const ffx::ReturnCode rc = entry->upscaler->create({info->device, record.physical, record.get_device_proc}, desc);
    if (rc != ffx::RETURN_OK) {
        error = std::string("ffxCreateContext on the Vulkan backend: ") + ffx::return_code_name(rc);
        return 0;
    }
    std::fprintf(stderr, "[rt] ffx: upscale context %ux%u -> %ux%u, provider %s\n", desc.maxRenderSize.width,
                 desc.maxRenderSize.height, desc.maxUpscaleSize.width, desc.maxUpscaleSize.height,
                 entry->upscaler->provider().c_str());
    std::lock_guard lock(g_mutex);
    const std::uint64_t id = g_next++;
    g_contexts[id] = std::move(entry);
    return id;
}

void destroy(std::uint64_t context) {
    DeviceRecord record;
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_contexts.find(context);
        if (it == g_contexts.end()) return;
        entry = std::move(it->second);
        g_contexts.erase(it);
        auto d = g_devices.find(entry->device);
        if (d == g_devices.end()) return;   // uninstall() already destroyed it
        record = d->second;
    }
    const VkDevice device = entry->device;
    destroy_entries(device, record, {std::move(entry)});
}

std::string provider(std::uint64_t context) {
    std::shared_ptr<Entry> entry = find(context);
    if (!entry) return {};
    std::lock_guard lock(entry->mutex);
    return entry->upscaler ? entry->upscaler->provider() : std::string{};
}

namespace {

// Caller holds g_loose_mutex.
bool load_loose() {
    std::string error;
    if (!g_loose_loaded) g_loose_loaded = g_loose.load(error);
    return g_loose_loaded;
}

// Where a query carries the title's ID3D12Device (docs/d3d12_vulkan.md A.3), or null.
void** device_slot(ffx::Header* desc) {
    if (desc->type == ffx::QUERY_DESC_TYPE_GET_VERSIONS)
        return &reinterpret_cast<ffx::QueryDescGetVersions*>(desc)->device;
    if (desc->type == ffx::QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2)
        return &reinterpret_cast<ffx::QueryDescUpscaleGetGPUMemoryUsageV2*>(desc)->device;
    return nullptr;
}

}  // namespace

ffx::ReturnCode query(std::uint64_t context, ffx::Header* desc) {
    if (!desc) return ffx::RETURN_ERROR_PARAMETER;
    void** const slot = device_slot(desc);
    void* const named = slot ? *slot : nullptr;
    DeviceRecord record;
    VkDevice device = VK_NULL_HANDLE;
    {
        std::lock_guard lock(g_mutex);
        if (named) {
            const rt::d3d12::VulkanDeviceInfo* info = rt::d3d12::vulkan_device_info(static_cast<ID3D12Device*>(named));
            auto it = info ? g_devices.find(info->device) : g_devices.end();
            if (it == g_devices.end()) return ffx::RETURN_ERROR_PARAMETER;
            device = it->first;
            record = it->second;
        } else if (!context && !g_devices.empty()) {
            device = g_devices.begin()->first;
            record = g_devices.begin()->second;
        }
    }
    // AMD's code reads the device as a Vulkan one: for this call only it is the layer's, the title's is put back.
    ffx::VkDeviceContext vulkan{device, record.physical, reinterpret_cast<void*>(record.get_device_proc)};
    if (named) *slot = &vulkan;
    const auto run = [&]() -> ffx::ReturnCode {
        if (context) {
            std::shared_ptr<Entry> entry = find(context);
            if (!entry) return ffx::RETURN_ERROR;
            std::lock_guard lock(entry->mutex);
            return entry->upscaler ? entry->upscaler->query(desc) : ffx::RETURN_ERROR;
        }
        if (!device) return ffx::RETURN_NO_PROVIDER;
        std::lock_guard lock(g_loose_mutex);
        if (!load_loose()) return ffx::RETURN_NO_PROVIDER;
        if (named) return g_loose.query(desc);
        ffx::CreateBackendVKDesc backend{{ffx::CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, desc->pNext}, vulkan};
        ffx::Header* const chain = desc->pNext;
        desc->pNext = &backend.header;   // likewise the title's chain is restored
        const ffx::ReturnCode rc = g_loose.query(desc);
        desc->pNext = chain;
        return rc;
    };
    const ffx::ReturnCode rc = run();
    if (named) *slot = named;
    return rc;
}

ffx::ReturnCode configure(std::uint64_t context, const ffx::Header* desc) {
    if (!desc) return ffx::RETURN_ERROR_PARAMETER;
    if (context) {
        std::shared_ptr<Entry> entry = find(context);
        if (!entry) return ffx::RETURN_ERROR;
        std::lock_guard lock(entry->mutex);
        return entry->upscaler ? entry->upscaler->configure(desc) : ffx::RETURN_ERROR;
    }
    std::lock_guard lock(g_loose_mutex);
    return load_loose() ? g_loose.configure(desc) : ffx::RETURN_NO_PROVIDER;
}

}  // namespace rt::ffx_upscale
