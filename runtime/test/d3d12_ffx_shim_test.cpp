// RT-344 part 2: the FFX upscale shim (shims/ffx_shim.cpp) driven as the title drives the package's FFX DLL. The five
// functions are looked up the way the loader binds the title's imports (rt::shims::find on the console DLLs' names)
// and called with the console chain: the upscale create description, a version descriptor naming the console's
// provider (4.1.1, which the shim does not pass on), and the console backend descriptor holding the device.
//
//   d3d12_ffx_shim_test               CTest sets LEONIDA_FFX_PC=1 (the table is registered before main)
//   d3d12_ffx_shim_test --expect-off  CTest sets LEONIDA_FFX_PC=0: nothing is registered, the modules aren't owned
//
// With it on: a context-less ffxConfigure (global debug) and ffxQuery (GET_VERSIONS on the title's device, the
// Quality upscale ratio), ffxCreateContext through amd_fidelityfx_loader_xbox.dll, the provider query on the
// context, 30 ffxDispatch calls through amd_fidelityfx_upscaler_xbox.dll on the scene of d3d12_ffx_scene.h (checked
// as in d3d12_ffx_upscale_test), descriptions the shim does not handle (FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE, or
// _PARAMETER without a device), and ffxDestroyContext. Skips (77) without a Vulkan GPU or AMD's loader DLL
// (LEONIDA_FFX_DIR). No game data.
#include "../shims/d3d12_com.h"
#include "../shims/ffx_upscale.h"
#include "../shims/shims.h"
#include "d3d12_ffx_scene.h"

#include <cstdio>
#include <cstring>
#include <string>

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);     \
            return 1;                                                             \
        }                                                                         \
    } while (0)

using namespace ffx_scene;

namespace {

constexpr UINT FRAMES = 30;
constexpr const char* kDlls[] = {"amd_fidelityfx_loader_xbox.dll", "amd_fidelityfx_upscaler_xbox.dll"};
constexpr const char* kNames[] = {"ffxCreateContext", "ffxDestroyContext", "ffxConfigure", "ffxQuery", "ffxDispatch"};

// What the title chains: type 0x01000002, measured from the console DLL (docs/d3d12_vulkan.md A.3), the device at +0x10.
// Written out here rather than shared with the shim, so a wrong id in the shim fails this test.
constexpr ffx::StructType kXboxBackendDesc = 0x01000002u;
struct XboxBackendDesc {
    ffx::Header header;
    void* device;
};

void on_message(uint32_t type, const wchar_t* message) { std::printf("  ffx message (%u): %ls\n", type, message); }

ffx::Resource resource(ID3D12Resource* r, UINT w, UINT h, uint32_t format, uint32_t usage, uint32_t state) {
    ffx::Resource out{};
    out.resource = r;
    out.description = {ffx::RESOURCE_TYPE_TEXTURE2D, format, w, h, 1, 1, 0, usage};
    out.state = state;
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool expect_off = argc > 1 && std::strcmp(argv[1], "--expect-off") == 0;
    for (const char* dll : kDlls) {
        CHECK(rt::shims::owns_dll(dll) == !expect_off);
        for (const char* name : kNames) CHECK((rt::shims::find(dll, name) != nullptr) == !expect_off);
    }
    if (expect_off) {
        std::printf("PASS: with LEONIDA_FFX_PC off nothing is registered and the package's FFX DLLs are not owned\n");
        return 0;
    }
    auto fn = [](const char* dll, const char* name) { return rt::shims::find(dll, name); };
    const auto create = reinterpret_cast<ffx::PfnCreateContext>(fn(kDlls[0], "ffxCreateContext"));
    const auto destroy = reinterpret_cast<ffx::PfnDestroyContext>(fn(kDlls[0], "ffxDestroyContext"));
    const auto configure = reinterpret_cast<ffx::PfnConfigure>(fn(kDlls[0], "ffxConfigure"));
    const auto query = reinterpret_cast<ffx::PfnQuery>(fn(kDlls[0], "ffxQuery"));
    const auto dispatch = reinterpret_cast<ffx::PfnDispatch>(fn(kDlls[1], "ffxDispatch"));
    for (const char* name : kNames) CHECK(fn(kDlls[0], name) == fn(kDlls[1], name));   // one implementation

    Ref<ID3D12Device> device;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p))) || !device.p) {
        std::printf("SKIP no Vulkan device\n");
        return 77;
    }
    ffx::ConfigureDescGlobalDebug debug{{ffx::CONFIGURE_DESC_TYPE_GLOBALDEBUG, nullptr}, 0, on_message,
                                        ffx::GLOBALDEBUG_LEVEL_VERBOSE};
    const ffx::ReturnCode configured = configure(nullptr, &debug.header);
    if (configured == ffx::RETURN_NO_PROVIDER) {
        std::printf("SKIP AMD's loader DLL not found (set LEONIDA_FFX_DIR)\n");
        return 77;
    }
    std::printf("ffxConfigure(global debug, no context): %s\n", ffx::return_code_name(configured));
    CHECK(configured == ffx::RETURN_OK);

    uint64_t count = 8, ids[8] = {};
    const char* names[8] = {};
    ffx::QueryDescGetVersions versions{{ffx::QUERY_DESC_TYPE_GET_VERSIONS, nullptr},
                                       ffx::CREATE_CONTEXT_DESC_TYPE_UPSCALE, device.p, &count, ids, names};
    CHECK(query(nullptr, &versions.header) == ffx::RETURN_OK && count > 0);
    std::printf("providers for the title's device:");
    for (uint64_t i = 0; i < count && i < 8; ++i) std::printf(" %s", names[i] ? names[i] : "?");
    std::printf("\n");
    float ratio = 0;
    ffx::QueryDescUpscaleGetUpscaleRatioFromQualityMode quality{
        {ffx::QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE, nullptr}, ffx::QUALITY_MODE_QUALITY, &ratio};
    CHECK(query(nullptr, &quality.header) == ffx::RETURN_OK && ratio == 1.5f);

    // The console chain: upscale, then the version the console DLL wants, then its backend with the device.
    XboxBackendDesc backend{{kXboxBackendDesc, nullptr}, device.p};
    ffx::CreateContextDescUpscaleVersion version{{ffx::CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &backend.header},
                                                 ffx::upscaler_version(4, 1, 1)};
    ffx::CreateContextDescUpscale upscale{{ffx::CREATE_CONTEXT_DESC_TYPE_UPSCALE, &version.header},
                                          ffx::UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | ffx::UPSCALE_ENABLE_DEPTH_INVERTED |
                                              ffx::UPSCALE_ENABLE_AUTO_EXPOSURE,
                                          {RW, RH}, {OW, OH}, on_message};
    ffx::Context context = nullptr;
    CHECK(create(&context, &upscale.header, nullptr) == ffx::RETURN_OK && context);
    ffx::QueryGetProviderVersion provider{{ffx::QUERY_DESC_TYPE_GET_PROVIDER_VERSION, nullptr}, 0, nullptr};
    CHECK(query(&context, &provider.header) == ffx::RETURN_OK && provider.versionName);
    std::printf("context provider: %s\n", provider.versionName);

    // Queries that carry the title's device, with and without a context: the shim hands AMD's code the layer's
    // Vulkan device instead and puts the title's back.
    for (ffx::Context* on : {static_cast<ffx::Context*>(nullptr), &context}) {
        ffx::EffectMemoryUsage usage{};
        ffx::QueryDescUpscaleGetGPUMemoryUsageV2 memory{{ffx::QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2, nullptr},
                                                        device.p, {RW, RH}, {OW, OH}, upscale.flags, &usage};
        const ffx::ReturnCode rc = query(on, &memory.header);
        std::printf("GPU memory usage (%s): %s, %llu bytes\n", on ? "context" : "no context", ffx::return_code_name(rc),
                    static_cast<unsigned long long>(usage.totalUsageInBytes));
        CHECK(rc == ffx::RETURN_OK && usage.totalUsageInBytes > 0 && memory.device == device.p);
        uint64_t n = 8;
        ffx::QueryDescGetVersions listed{{ffx::QUERY_DESC_TYPE_GET_VERSIONS, nullptr},
                                         ffx::CREATE_CONTEXT_DESC_TYPE_UPSCALE, device.p, &n, ids, names};
        const ffx::ReturnCode listed_rc = query(on, &listed.header);
        std::printf("GET_VERSIONS (%s): %s\n", on ? "context" : "no context", ffx::return_code_name(listed_rc));
        // AMD's loader lists versions only without a context (measured: ERROR_UNKNOWN_DESCTYPE with one, 4.1.2/3.1.6).
        CHECK((on ? listed_rc == ffx::RETURN_ERROR_UNKNOWN_DESCTYPE : listed_rc == ffx::RETURN_OK && n > 0) &&
              listed.device == device.p);
    }

    // Refused: no upscale description, no backend descriptor.
    ffx::Context refused = nullptr;
    ffx::Header other{kXboxBackendDesc, nullptr};
    CHECK(create(&refused, &other, nullptr) == ffx::RETURN_ERROR_UNKNOWN_DESCTYPE && !refused);
    ffx::CreateContextDescUpscale lonely = upscale;
    lonely.header.pNext = nullptr;
    CHECK(create(&refused, &lonely.header, nullptr) == ffx::RETURN_ERROR_PARAMETER && !refused);
    // A backend under a BACKEND_ID_XBOX id is not the console descriptor (the console DLL refuses those too).
    XboxBackendDesc wrong_backend{{ffx::backend_sub_id(ffx::BACKEND_ID_XBOX, 0x02), nullptr}, device.p};
    ffx::CreateContextDescUpscale wrong = upscale;
    wrong.header.pNext = &wrong_backend.header;
    CHECK(create(&refused, &wrong.header, nullptr) == ffx::RETURN_ERROR_PARAMETER && !refused);

    Scene scene;
    CHECK(scene.create(device.p));
    ffx::DispatchDescUpscale d{};
    d.header.type = ffx::DISPATCH_DESC_TYPE_UPSCALE;
    d.color = resource(scene.color.p, RW, RH, ffx::SURFACE_FORMAT_R16G16B16A16_FLOAT, ffx::RESOURCE_USAGE_READ_ONLY,
                       ffx::RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(scene.depth.p, RW, RH, ffx::SURFACE_FORMAT_R32_FLOAT, ffx::RESOURCE_USAGE_READ_ONLY,
                       ffx::RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(scene.motion.p, RW, RH, ffx::SURFACE_FORMAT_R16G16_FLOAT,
                               ffx::RESOURCE_USAGE_READ_ONLY, ffx::RESOURCE_STATE_COMPUTE_READ);
    d.output = resource(scene.output.p, OW, OH, ffx::SURFACE_FORMAT_R16G16B16A16_FLOAT, ffx::RESOURCE_USAGE_UAV,
                        ffx::RESOURCE_STATE_UNORDERED_ACCESS);
    d.motionVectorScale = {float(RW), float(RH)};
    d.renderSize = {RW, RH};
    d.upscaleSize = {OW, OH};
    d.frameTimeDelta = 16.6f;
    d.preExposure = 1.0f;
    d.cameraNear = 1e30f;   // inverted depth: near and far swap
    d.cameraFar = 0.1f;
    d.cameraFovAngleVertical = 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    for (UINT frame = 0; frame < FRAMES; ++frame) {
        if (frame) CHECK(scene.reset());
        d.commandList = scene.list.p;
        d.jitterOffset = {halton(int(frame % 8) + 1, 2) - 0.5f, halton(int(frame % 8) + 1, 3) - 0.5f};
        d.reset = frame == 0;
        CHECK(dispatch(&context, &d.header) == ffx::RETURN_OK);
        if (frame + 1 == FRAMES) scene.record_readback();
        CHECK(scene.submit());
    }
    std::printf("%u ffxDispatch calls through the shim, %ux%u -> %ux%u\n", FRAMES, RW, RH, OW, OH);
    CHECK(scene.check());

    // A dispatch description the shim does not handle.
    ffx::Header mask{ffx::effect_sub_id(ffx::EFFECT_ID_UPSCALE, 0x06), nullptr};
    CHECK(dispatch(&context, &mask) == ffx::RETURN_ERROR_UNKNOWN_DESCTYPE);
    CHECK(destroy(&context, nullptr) == ffx::RETURN_OK && !context);
    std::printf("PASS\n");
    return 0;
}
