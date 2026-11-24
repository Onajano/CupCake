// The procedural scene of the layer's upscaler tests (d3d12_ffx_upscale_test, d3d12_ffx_shim_test), on a D3D12 device
// of the Vulkan layer: a 1280x720 RGBA16F checkerboard of 32-pixel squares (1.0 / 0.0), R32_FLOAT depth 0 (far,
// inverted) and zero R16G16_FLOAT motion, uploaded by copies and moved to NON_PIXEL_SHADER_RESOURCE, and a 1920x1080
// RGBA16F UAV output (the console DLL's one configuration). check() reads the output back: every value finite, the
// centre of every 48-pixel square within 0.1 of its input. No game data.
#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ffx_scene {

constexpr UINT RW = 1280, RH = 720, OW = 1920, OH = 1080, SQUARE = 32;

template<class T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
};

inline float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = h >> 15, exp = (h >> 10) & 31, mant = h & 1023;
    const float v = exp == 0    ? std::ldexp(float(mant), -24)
                    : exp == 31 ? (mant ? NAN : INFINITY)
                                : std::ldexp(float(mant | 1024), int(exp) - 25);
    return sign ? -v : v;
}

inline float halton(int i, int base) {
    float f = 1, r = 0;
    for (; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

struct Scene {
    ID3D12Device* device = nullptr;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;   // open after create(), and after each reset()
    Ref<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    Ref<ID3D12Resource> color, depth, motion, output, upload, readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[3]{}, out_footprint{};
    UINT64 readback_size = 0;

    Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    ~Scene() {
        if (event) CloseHandle(event);
    }

    bool texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                 Ref<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
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
    }

    bool buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state, Ref<ID3D12Resource>& out) {
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
    }

    // The queue, list, images and buffers, the checkerboard in the upload buffer, and the list recording the input
    // uploads and their move to NON_PIXEL_SHADER_RESOURCE (left open).
    bool create(ID3D12Device* d) {
        device = d;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        if (!event || FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                             IID_PPV_ARGS(&list.p))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))))
            return false;
        if (!texture(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                     color) ||
            !texture(RW, RH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, depth) ||
            !texture(RW, RH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                     motion) ||
            !texture(OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output))
            return false;
        ID3D12Resource* inputs[] = {color.p, depth.p, motion.p};
        UINT64 upload_size = 0;
        for (int i = 0; i < 3; ++i) {
            const D3D12_RESOURCE_DESC desc = inputs[i]->GetDesc();
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&desc, 0, 1, upload_size, &footprints[i], nullptr, nullptr, &bytes);
            upload_size = (footprints[i].Offset + bytes + 511) & ~UINT64(511);
        }
        const D3D12_RESOURCE_DESC out_desc = output->GetDesc();
        device->GetCopyableFootprints(&out_desc, 0, 1, 0, &out_footprint, nullptr, nullptr, &readback_size);
        if (!buffer(D3D12_HEAP_TYPE_UPLOAD, upload_size, D3D12_RESOURCE_STATE_GENERIC_READ, upload) ||
            !buffer(D3D12_HEAP_TYPE_READBACK, readback_size, D3D12_RESOURCE_STATE_COPY_DEST, readback))
            return false;
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped)) || !mapped) return false;
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
        D3D12_RESOURCE_BARRIER to_read[3];
        for (int i = 0; i < 3; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = inputs[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[i];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            to_read[i] = transition(inputs[i], D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        list->ResourceBarrier(3, to_read);
        return true;
    }

    // Records the output's copy into the readback buffer (the output stays in UNORDERED_ACCESS around it).
    void record_readback() {
        const D3D12_RESOURCE_BARRIER to_copy =
            transition(output.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = out_footprint;
        src.pResource = output.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const D3D12_RESOURCE_BARRIER back =
            transition(output.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &back);
    }

    // Closes, executes and waits for the list.
    bool submit() {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.p, ++fence_value)) ||
            FAILED(fence->SetEventOnCompletion(fence_value, event)))
            return false;
        return WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    }

    // Reopens the list after a submit.
    bool reset() { return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr)); }

    // The readback buffer against the checkerboard; prints the counts.
    bool check() {
        void* mapped = nullptr;
        const D3D12_RANGE range{0, SIZE_T(readback_size)};
        if (FAILED(readback->Map(0, &range, &mapped)) || !mapped) return false;
        const UINT pitch = out_footprint.Footprint.RowPitch;
        auto texel = [&](UINT x, UINT y, int c) {
            return half_to_float(reinterpret_cast<const std::uint16_t*>(static_cast<const unsigned char*>(mapped) +
                                                                        std::size_t(y) * pitch)[x * 4 + c]);
        };
        std::size_t non_finite = 0;
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
                    close = close &&
                            std::fabs(texel(sx * square + square / 2, sy * square + square / 2, c) - want) < 0.1f;
                right += close;
            }
        readback->Unmap(0, nullptr);
        std::printf("non-finite output values: %zu; square centres within 0.1 of the input: %u of %u\n", non_finite,
                    right, squares);
        return non_finite == 0 && right == squares;
    }
};

}  // namespace ffx_scene
