// RT-344: the layer's Upscale command on the Vulkan-backed D3D12 device, end to end through its own objects, as the
// FFX shim will drive it. With LEONIDA_FFX_PC=1 (set here before the device exists), rt::ffx_upscale creates an
// upscale context on the device, rt::d3d12::record_upscale records the upscale into a D3D12 command list, and
// ExecuteCommandLists replays it: replay moves the images to the upscaler's layouts and back, and AMD's PC upscaler
// (LEONIDA_FFX_DIR, as for ffx_vk_dispatch_test) records into the replay's command buffer.
//
// A 1280x720 RGBA16F checkerboard of 32-pixel squares, R32_FLOAT depth 0 and zero R16G16_FLOAT motion are uploaded by
// copies, moved to NON_PIXEL_SHADER_RESOURCE, and upscaled 30 times to a 1920x1080 UAV texture (the console DLL's one
// configuration). The output is copied back: every value finite, the centre of every 48-pixel square within 0.1.
// Then the context is destroyed and a list recording an Upscale that names it must still execute (the command is
// skipped), and record_upscale must refuse an Upscale without a context and a copy list. Skips (77) without a
// Vulkan GPU or AMD's loader DLL. No game data.
#include "../shims/d3d12_com.h"
#include "../shims/ffx_upscale.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);     \
            return 1;                                                             \
        }                                                                         \
    } while (0)

namespace {

constexpr UINT RW = 1280, RH = 720, OW = 1920, OH = 1080, SQUARE = 32, FRAMES = 30;

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
};

struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() {
        if (handle) CloseHandle(handle);
    }
};

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = h >> 15, exp = (h >> 10) & 31, mant = h & 1023;
    const float v = exp == 0    ? std::ldexp(float(mant), -24)
                    : exp == 31 ? (mant ? NAN : INFINITY)
                                : std::ldexp(float(mant | 1024), int(exp) - 25);
    return sign ? -v : v;
}

float halton(int i, int base) {
    float f = 1, r = 0;
    for (; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

void on_message(uint32_t type, const wchar_t* message) { std::printf("  ffx message (%u): %ls\n", type, message); }

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

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

    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    CHECK(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                             IID_PPV_ARGS(&list.p))));
    CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    Event event;
    CHECK(event.handle);
    UINT64 fence_value = 0;
    auto submit = [&]() -> bool {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.p, ++fence_value))) return false;
        if (FAILED(fence->SetEventOnCompletion(fence_value, event.handle))) return false;
        return WaitForSingleObject(event.handle, 10000) == WAIT_OBJECT_0;
    };

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto texture = [&](UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                       Ref<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = flags;
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                         IID_PPV_ARGS(&out.p)));
    };
    Ref<ID3D12Resource> color, depth, motion, output, upload, readback;
    CHECK(texture(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, color));
    CHECK(texture(RW, RH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, depth));
    CHECK(texture(RW, RH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, motion));
    CHECK(texture(OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output));

    // One upload buffer for the three inputs, one readback buffer for the output, laid out by GetCopyableFootprints.
    ID3D12Resource* inputs[] = {color.p, depth.p, motion.p};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[3]{}, out_footprint{};
    UINT64 upload_size = 0, readback_size = 0;
    for (int i = 0; i < 3; ++i) {
        const D3D12_RESOURCE_DESC d = inputs[i]->GetDesc();
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&d, 0, 1, upload_size, &footprints[i], nullptr, nullptr, &bytes);
        upload_size = (footprints[i].Offset + bytes + 511) & ~UINT64(511);
    }
    const D3D12_RESOURCE_DESC out_desc = output->GetDesc();
    device->GetCopyableFootprints(&out_desc, 0, 1, 0, &out_footprint, nullptr, nullptr, &readback_size);
    auto buffer = [&](D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state, Ref<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = type;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                         IID_PPV_ARGS(&out.p)));
    };
    CHECK(buffer(D3D12_HEAP_TYPE_UPLOAD, upload_size, D3D12_RESOURCE_STATE_GENERIC_READ, upload));
    CHECK(buffer(D3D12_HEAP_TYPE_READBACK, readback_size, D3D12_RESOURCE_STATE_COPY_DEST, readback));
    void* mapped = nullptr;
    CHECK(SUCCEEDED(upload->Map(0, nullptr, &mapped)) && mapped);
    std::memset(mapped, 0, std::size_t(upload_size));   // depth 0 (far, inverted) and zero motion
    for (UINT y = 0; y < RH; ++y) {
        auto* row = reinterpret_cast<std::uint16_t*>(static_cast<unsigned char*>(mapped) + footprints[0].Offset +
                                                     std::size_t(y) * footprints[0].Footprint.RowPitch);
        for (UINT x = 0; x < RW; ++x) {
            row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = ((x / SQUARE + y / SQUARE) & 1) ? 0x3C00 : 0;
            row[x * 4 + 3] = 0x3C00;
        }
    }
    upload->Unmap(0, nullptr);

    leonida::Upscale u{};
    u.context = context;
    u.color = rt::d3d12::resource_id(color.p);
    u.depth = rt::d3d12::resource_id(depth.p);
    u.motion_vectors = rt::d3d12::resource_id(motion.p);
    u.output = rt::d3d12::resource_id(output.p);
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
        if (frame) {
            CHECK(SUCCEEDED(allocator->Reset()));
            CHECK(SUCCEEDED(list->Reset(allocator.p, nullptr)));
        } else {
            for (int i = 0; i < 3; ++i) {
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = inputs[i];
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.pResource = upload.p;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = footprints[i];
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            D3D12_RESOURCE_BARRIER to_read[3];
            for (int i = 0; i < 3; ++i)
                to_read[i] = transition(inputs[i], D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(3, to_read);
        }
        u.jitter_x = halton(int(frame % 8) + 1, 2) - 0.5f;
        u.jitter_y = halton(int(frame % 8) + 1, 3) - 0.5f;
        u.reset = frame == 0;
        CHECK(rt::d3d12::record_upscale(list.p, u));
        if (frame + 1 == FRAMES) {
            const D3D12_RESOURCE_BARRIER to_copy = transition(output.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                              D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = out_footprint;
            src.pResource = output.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            const D3D12_RESOURCE_BARRIER back = transition(output.p, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &back);
        }
        QueryPerformanceCounter(&t0);
        CHECK(submit());
        QueryPerformanceCounter(&t1);
        if (frame >= 5) timed_ms += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(frequency.QuadPart);
    }
    std::printf("%u upscales %ux%u -> %ux%u through the layer, execute and wait %.2f ms each after 5 warm-up frames\n",
                FRAMES, RW, RH, OW, OH, timed_ms / (FRAMES - 5));

    const D3D12_RANGE range{0, SIZE_T(readback_size)};
    CHECK(SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped);
    std::size_t non_finite = 0;
    const UINT pitch = out_footprint.Footprint.RowPitch;
    auto texel = [&](UINT x, UINT y, int c) {
        return half_to_float(reinterpret_cast<const std::uint16_t*>(static_cast<const unsigned char*>(mapped) +
                                                                    std::size_t(y) * pitch)[x * 4 + c]);
    };
    for (UINT y = 0; y < OH; ++y)
        for (UINT x = 0; x < OW; ++x)
            for (int c = 0; c < 4; ++c) non_finite += !std::isfinite(texel(x, y, c));
    const UINT square = SQUARE * OW / RW;
    UINT right = 0, squares = 0;
    for (UINT sy = 0; (sy + 1) * square <= OH; ++sy)
        for (UINT sx = 0; (sx + 1) * square <= OW; ++sx, ++squares) {
            const float want = ((sx + sy) & 1) ? 1.0f : 0.0f;
            bool close = true;
            for (int c = 0; c < 3; ++c)
                close = close && std::fabs(texel(sx * square + square / 2, sy * square + square / 2, c) - want) < 0.1f;
            right += close;
        }
    readback->Unmap(0, nullptr);
    std::printf("non-finite output values: %zu; square centres within 0.1 of the input: %u of %u\n", non_finite, right,
                squares);
    CHECK(non_finite == 0 && right == squares);

    // A context destroyed after an Upscale names it: the list still executes, the command skipped.
    rt::ffx_upscale::destroy(context);
    CHECK(rt::ffx_upscale::provider(context).empty());
    CHECK(SUCCEEDED(allocator->Reset()));
    CHECK(SUCCEEDED(list->Reset(allocator.p, nullptr)));
    CHECK(rt::d3d12::record_upscale(list.p, u));
    CHECK(submit());
    std::printf("an Upscale naming a destroyed context: list executed, command skipped\n");

    // Refused when recorded: no context, and a copy list.
    CHECK(SUCCEEDED(allocator->Reset()));
    CHECK(SUCCEEDED(list->Reset(allocator.p, nullptr)));
    leonida::Upscale none = u;
    none.context = 0;
    CHECK(!rt::d3d12::record_upscale(list.p, none));
    CHECK(SUCCEEDED(list->Close()));
    Ref<ID3D12CommandAllocator> copy_allocator;
    Ref<ID3D12GraphicsCommandList> copy_list;
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copy_allocator.p))));
    CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_allocator.p, nullptr,
                                             IID_PPV_ARGS(&copy_list.p))));
    CHECK(!rt::d3d12::record_upscale(copy_list.p, u));
    std::printf("PASS\n");
    return 0;
}
