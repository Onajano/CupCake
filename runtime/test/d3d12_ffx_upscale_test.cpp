// RT-344: the layer's Upscale command on the Vulkan-backed D3D12 device, end to end through its own objects. With
// LEONIDA_FFX_PC=1 (set here before the device exists), rt::ffx_upscale creates an upscale context on the device,
// rt::d3d12::record_upscale records the upscale into a D3D12 command list, and ExecuteCommandLists replays it: replay
// moves the images to the upscaler's layouts and back, and AMD's PC upscaler (LEONIDA_FFX_DIR, as for
// ffx_vk_dispatch_test) records into the replay's command buffer. d3d12_ffx_shim_test drives the same through the
// title's FFX calls.
//
// The scene (d3d12_ffx_scene.h) is upscaled 30 times and checked. Then the context is destroyed and a list recording
// an Upscale that names it must still execute (the command is skipped), and record_upscale must refuse an Upscale
// without a context and a copy list. Skips (77) without a Vulkan GPU or AMD's loader DLL. No game data.
#include "../shims/d3d12_com.h"
#include "../shims/ffx_upscale.h"
#include "d3d12_ffx_scene.h"

#include <cstdio>
#include <cstdlib>
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

void on_message(uint32_t type, const wchar_t* message) { std::printf("  ffx message (%u): %ls\n", type, message); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _putenv_s("LEONIDA_FFX_PC", "1");   // read once, when the device is created
    Ref<ID3D12Device> device;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p))) || !device.p) {
        std::printf("SKIP no Vulkan device\n");
        return 77;
    }
    ffx::CreateContextDescUpscale create{{ffx::CREATE_CONTEXT_DESC_TYPE_UPSCALE, nullptr},
                                         ffx::UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | ffx::UPSCALE_ENABLE_DEPTH_INVERTED |
                                             ffx::UPSCALE_ENABLE_AUTO_EXPOSURE,
                                         {RW, RH}, {OW, OH}, on_message};
    std::string error;
    const std::uint64_t context = rt::ffx_upscale::create(device.p, create, error);
    if (!context) {
        if (error.find("not found") != std::string::npos) {
            std::printf("SKIP %s\n", error.c_str());
            return 77;
        }
        std::printf("FAIL rt::ffx_upscale::create: %s\n", error.c_str());
        return 1;
    }
    std::printf("provider: %s\n", rt::ffx_upscale::provider(context).c_str());

    Scene scene;
    CHECK(scene.create(device.p));
    leonida::Upscale u{};
    u.context = context;
    u.color = rt::d3d12::resource_id(scene.color.p);
    u.depth = rt::d3d12::resource_id(scene.depth.p);
    u.motion_vectors = rt::d3d12::resource_id(scene.motion.p);
    u.output = rt::d3d12::resource_id(scene.output.p);
    u.motion_vector_scale_x = float(RW);
    u.motion_vector_scale_y = float(RH);
    u.render_width = RW;
    u.render_height = RH;
    u.upscale_width = OW;
    u.upscale_height = OH;
    u.frame_time_delta = 16.6f;
    u.camera_near = 1e30f;   // inverted depth: near and far swap
    u.camera_far = 0.1f;
    u.camera_fov_vertical = 1.0f;

    LARGE_INTEGER frequency, t0, t1;
    QueryPerformanceFrequency(&frequency);
    double timed_ms = 0;
    for (UINT frame = 0; frame < FRAMES; ++frame) {
        if (frame) CHECK(scene.reset());
        u.jitter_x = halton(int(frame % 8) + 1, 2) - 0.5f;
        u.jitter_y = halton(int(frame % 8) + 1, 3) - 0.5f;
        u.reset = frame == 0;
        CHECK(rt::d3d12::record_upscale(scene.list.p, u));
        if (frame + 1 == FRAMES) scene.record_readback();
        QueryPerformanceCounter(&t0);
        CHECK(scene.submit());
        QueryPerformanceCounter(&t1);
        if (frame >= 5) timed_ms += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(frequency.QuadPart);
    }
    std::printf("%u upscales %ux%u -> %ux%u through the layer, execute and wait %.2f ms each after 5 warm-up frames\n",
                FRAMES, RW, RH, OW, OH, timed_ms / (FRAMES - 5));
    CHECK(scene.check());

    // A context destroyed after an Upscale names it: the list still executes, the command skipped.
    rt::ffx_upscale::destroy(context);
    CHECK(rt::ffx_upscale::provider(context).empty());
    CHECK(scene.reset());
    CHECK(rt::d3d12::record_upscale(scene.list.p, u));
    CHECK(scene.submit());
    std::printf("an Upscale naming a destroyed context: list executed, command skipped\n");

    // Refused when recorded: no context, and a copy list.
    CHECK(scene.reset());
    leonida::Upscale none = u;
    none.context = 0;
    CHECK(!rt::d3d12::record_upscale(scene.list.p, none));
    CHECK(SUCCEEDED(scene.list->Close()));
    Ref<ID3D12CommandAllocator> copy_allocator;
    Ref<ID3D12GraphicsCommandList> copy_list;
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copy_allocator.p))));
    CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_allocator.p, nullptr,
                                             IID_PPV_ARGS(&copy_list.p))));
    CHECK(!rt::d3d12::record_upscale(copy_list.p, u));
    std::printf("PASS\n");
    return 0;
}
