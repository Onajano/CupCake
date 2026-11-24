// The FFX upscale shim (RT-344, part 2): the five FidelityFX API functions the title calls, answered by AMD's PC
// upscaler on the Vulkan layer instead of the package's amd_fidelityfx_upscaler_xbox.dll.
//
// Both FFX DLLs of the package, amd_fidelityfx_loader_xbox.dll and amd_fidelityfx_upscaler_xbox.dll, export exactly
// ffxConfigure, ffxCreateContext, ffxDestroyContext, ffxDispatch and ffxQuery (dumpbin, docs/d3d12_command_objects.md
// §3.5); which one the title calls is not known without its executable, so both are served. The table is registered
// only with LEONIDA_FFX_PC=1, before main (the process environment decides): then the shims own the two modules
// (static imports and LoadLibrary/GetProcAddress resolve here, and the package's copies are never mapped). Without
// it nothing is registered and the title gets the package's DLLs exactly as before.
//
// The calls, with the FFX API's PC struct type ids, which the console DLL accepts (docs/d3d12_vulkan.md A.3):
// - ffxCreateContext: an upscale context (FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) on the device of the console
//   backend descriptor in its chain (type 0x01000002, {header, device} with the device at +0x10, measured from the
//   DLL), through rt::ffx_upscale::create. The title's handle is that context's number. The rest of the chain (the
//   version descriptor, a provider override naming the console provider) is for the console DLL and not passed on.
// - ffxDispatch: an upscale (FFX_API_DISPATCH_DESC_TYPE_UPSCALE) becomes one leonida::Upscale recorded into the
//   title's command list (rt::d3d12::record_upscale); replay runs it. The title's resources are the layer's own
//   objects (the device proxy forwards resource creation), so their pointers are the layer's resource ids.
// - ffxQuery, ffxConfigure: passed to AMD's loader (rt::ffx_upscale::query, configure); a global debug
//   configure for effect 0 is sent for the upscaler, the only effect served.
// - ffxDestroyContext: rt::ffx_upscale::destroy.
// Any other description type is FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE, reported once per type.
#include "ffx_upscale.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>

#include "d3d12_com.h"
#include "shims.h"

namespace {

// The console backend descriptor, {header, device}. Its type is measured from the console DLL (docs/d3d12_vulkan.md
// A.3): 0x01000002, inside BACKEND_ID_VK's range; the DLL refuses BACKEND_ID_XBOX ids.
constexpr ffx::StructType kXboxBackendDesc = 0x01000002u;
struct XboxBackendDesc {
    ffx::Header header;
    void* device;   // the title's ID3D12Device (the Xbox-ABI proxy over the layer's device)
};
static_assert(offsetof(XboxBackendDesc, device) == 0x10, "measured: the device at +0x10");

void unsupported(const char* call, ffx::StructType type) {
    static std::mutex mutex;
    static std::set<ffx::StructType> seen;
    std::lock_guard lock(mutex);
    if (seen.insert(type).second)
        std::fprintf(stderr, "[rt] ffx: %s with description type %#llx is not supported\n", call,
                     static_cast<unsigned long long>(type));
}

std::uint64_t id_of(const ffx::Context* context) {
    return context ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(*context)) : 0;
}

leonida::ResourceId id_of(const ffx::Resource& resource) {
    return rt::d3d12::resource_id(static_cast<const ID3D12Resource*>(resource.resource));
}

ffx::ReturnCode ffxCreateContext(ffx::Context* context, ffx::Header* desc, const void* /*memory callbacks*/) {
    if (!context || !desc) return ffx::RETURN_ERROR_PARAMETER;
    *context = nullptr;
    const ffx::CreateContextDescUpscale* upscale = nullptr;
    const XboxBackendDesc* backend = nullptr;
    for (const ffx::Header* h = desc; h; h = h->pNext) {
        if (h->type == ffx::CREATE_CONTEXT_DESC_TYPE_UPSCALE)
            upscale = reinterpret_cast<const ffx::CreateContextDescUpscale*>(h);
        else if (h->type == kXboxBackendDesc)
            backend = reinterpret_cast<const XboxBackendDesc*>(h);
    }
    if (!upscale) {
        unsupported("ffxCreateContext", desc->type);
        return ffx::RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
    if (!backend || !backend->device) {   // as the console DLL: no device, no context
        // Said once: a title whose backend description doesn't match would otherwise fail without a trace.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] ffx: ffxCreateContext refused: %s\n",
                         backend ? "the Xbox backend description has no device"
                                 : "no Xbox backend description (type 0x01000002) in the chain");
        return ffx::RETURN_ERROR_PARAMETER;
    }
    std::string error;
    const std::uint64_t id = rt::ffx_upscale::create(static_cast<ID3D12Device*>(backend->device), *upscale, error);
    if (!id) {
        std::fprintf(stderr, "[rt] ffx: ffxCreateContext failed: %s\n", error.c_str());
        return ffx::RETURN_ERROR;
    }
    *context = reinterpret_cast<ffx::Context>(static_cast<std::uintptr_t>(id));
    return ffx::RETURN_OK;
}

ffx::ReturnCode ffxDestroyContext(ffx::Context* context, const void* /*memory callbacks*/) {
    if (!context) return ffx::RETURN_ERROR_PARAMETER;
    rt::ffx_upscale::destroy(id_of(context));
    *context = nullptr;
    return ffx::RETURN_OK;
}

ffx::ReturnCode ffxConfigure(ffx::Context* context, const ffx::Header* desc) {
    // A context-less global debug description with effect 0: AMD's PC loader routes it by effect (0 is
    // FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE there, measured), and the only effect served here is the upscaler.
    if (desc && desc->type == ffx::CONFIGURE_DESC_TYPE_GLOBALDEBUG &&
        !reinterpret_cast<const ffx::ConfigureDescGlobalDebug*>(desc)->effectId) {
        ffx::ConfigureDescGlobalDebug debug = *reinterpret_cast<const ffx::ConfigureDescGlobalDebug*>(desc);
        debug.effectId = ffx::EFFECT_ID_UPSCALE;
        return rt::ffx_upscale::configure(id_of(context), &debug.header);
    }
    return rt::ffx_upscale::configure(id_of(context), desc);
}

ffx::ReturnCode ffxQuery(ffx::Context* context, ffx::Header* desc) {
    return rt::ffx_upscale::query(id_of(context), desc);
}

ffx::ReturnCode ffxDispatch(ffx::Context* context, const ffx::Header* desc) {
    if (!context || !desc) return ffx::RETURN_ERROR_PARAMETER;
    if (desc->type != ffx::DISPATCH_DESC_TYPE_UPSCALE) {
        unsupported("ffxDispatch", desc->type);
        return ffx::RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
    const auto& d = *reinterpret_cast<const ffx::DispatchDescUpscale*>(desc);
    leonida::Upscale u{};
    u.context = id_of(context);
    u.color = id_of(d.color);
    u.depth = id_of(d.depth);
    u.motion_vectors = id_of(d.motionVectors);
    u.output = id_of(d.output);
    u.exposure = id_of(d.exposure);
    u.reactive = id_of(d.reactive);
    u.transparency_and_composition = id_of(d.transparencyAndComposition);
    u.jitter_x = d.jitterOffset.x;
    u.jitter_y = d.jitterOffset.y;
    u.motion_vector_scale_x = d.motionVectorScale.x;
    u.motion_vector_scale_y = d.motionVectorScale.y;
    u.render_width = d.renderSize.width;
    u.render_height = d.renderSize.height;
    u.upscale_width = d.upscaleSize.width;
    u.upscale_height = d.upscaleSize.height;
    u.enable_sharpening = d.enableSharpening;
    u.sharpness = d.sharpness;
    u.frame_time_delta = d.frameTimeDelta;
    u.pre_exposure = d.preExposure;
    u.reset = d.reset;
    u.camera_near = d.cameraNear;
    u.camera_far = d.cameraFar;
    u.camera_fov_vertical = d.cameraFovAngleVertical;
    u.view_space_to_meters = d.viewSpaceToMetersFactor;
    u.flags = d.flags;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(d.commandList);
    return rt::d3d12::record_upscale(list, u) ? ffx::RETURN_OK : ffx::RETURN_ERROR_PARAMETER;
}

#define FFX_ENTRIES(dll)                                                              \
    {dll, "ffxCreateContext", reinterpret_cast<void*>(&ffxCreateContext)},            \
    {dll, "ffxDestroyContext", reinterpret_cast<void*>(&ffxDestroyContext)},          \
    {dll, "ffxConfigure", reinterpret_cast<void*>(&ffxConfigure)},                    \
    {dll, "ffxQuery", reinterpret_cast<void*>(&ffxQuery)},                            \
    {dll, "ffxDispatch", reinterpret_cast<void*>(&ffxDispatch)}

const rt::shims::Entry kEntries[] = {
    FFX_ENTRIES("amd_fidelityfx_loader_xbox.dll"),
    FFX_ENTRIES("amd_fidelityfx_upscaler_xbox.dll"),
};

// Registered before main, and only with the upscaler on: unregistered, the shims don't own the two modules.
const struct Registration {
    Registration() {
        if (rt::ffx_upscale::enabled()) static rt::shims::Table table(kEntries, std::size(kEntries));
    }
} registration;

}  // namespace
