// LEONIDA_STUB_PIPELINES (diagnostic): a pipeline whose translated shader is missing is created as a no-op stub
// instead of failing. Without the switch, a graphics PSO with no shader library is E_FAIL, as before; with it, the
// call succeeds and the PSO is never registered. A list that binds it drops the draws and dispatches made meanwhile
// and stays valid (D3D-482); with no pipeline state at all the same calls make Close fail, which is the control here.
// Synthetic only: no shader library, no game data, nothing reaches the GPU.
#include "../shims/d3d12_com.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failed = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
            ++g_failed;                                                             \
        }                                                                           \
    } while (0)

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
};

D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc() {
    static const unsigned char kBytes[16] = {'D', 'X', 'B', 'C'};  // never parsed: the library lookup comes first
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.VS = {kBytes, sizeof kBytes};
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    return desc;
}

}  // namespace

int main() {
    // No shader library, whatever the environment says.
    SetEnvironmentVariableW(L"LEONIDA_SHADERS", nullptr);
    _putenv_s("LEONIDA_STUB_PIPELINES", "");

    Ref<ID3D12Device> device;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p))) || !device.p) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = graphics_desc();

    {  // Off: refused as before, and no object.
        Ref<ID3D12PipelineState> pso;
        const HRESULT hr = device.p->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p));
        if (hr == E_NOTIMPL) {  // a device without the GPU address arena or graphics entry points refuses earlier
            std::printf("SKIP: the device cannot create graphics pipelines (E_NOTIMPL)\n");
            return 77;
        }
        CHECK(hr == E_FAIL);
        CHECK(!pso.p);
    }
    {  // "0" is off too.
        _putenv_s("LEONIDA_STUB_PIPELINES", "0");
        Ref<ID3D12PipelineState> pso;
        CHECK(device.p->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p)) == E_FAIL && !pso.p);
    }
    {  // On: a stub that is no registered pipeline, so SetPipelineState binds none.
        _putenv_s("LEONIDA_STUB_PIPELINES", "1");
        Ref<ID3D12PipelineState> pso;
        CHECK(device.p->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso.p)) == S_OK);
        CHECK(pso.p != nullptr);
        CHECK(rt::d3d12::graphics_pipeline_id(pso.p) == 0);
        CHECK(rt::d3d12::compute_pipeline_id(pso.p) == 0);
        CHECK(rt::d3d12::pipeline_is_stub(pso.p));

        // What is drawn or dispatched while the stub is bound is dropped, and the list stays valid: Close is S_OK and
        // the list holds no command. Without a pipeline at all the same calls make Close fail (the control below).
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        CHECK(device.p->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p)) == S_OK);
        CHECK(allocator.p &&
              device.p->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                          IID_PPV_ARGS(&list.p)) == S_OK);
        if (list.p) {
            list.p->SetPipelineState(pso.p);
            list.p->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list.p->DrawInstanced(3, 1, 0, 0);
            list.p->DrawIndexedInstanced(3, 1, 0, 0, 0);
            list.p->Dispatch(1, 1, 1);
            CHECK(list.p->Close() == S_OK);
            std::vector<leonida::Command> commands;
            CHECK(rt::d3d12::recorded_commands(list.p, commands) && commands.empty());

            // Control: the same draw with no pipeline state bound is an invalid list, as before.
            CHECK(list.p->Reset(allocator.p, nullptr) == S_OK);
            list.p->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list.p->DrawInstanced(3, 1, 0, 0);
            CHECK(list.p->Close() == E_INVALIDARG);

            // Reset with the stub as the initial state binds it too; Reset with none forgets it.
            CHECK(list.p->Reset(allocator.p, pso.p) == S_OK);
            list.p->Dispatch(1, 1, 1);
            CHECK(list.p->Close() == S_OK);
            CHECK(list.p->Reset(allocator.p, nullptr) == S_OK);
            list.p->Dispatch(1, 1, 1);
            CHECK(list.p->Close() == E_INVALIDARG);
        }
    }
    {  // Real refusals are not stubbed: a bad description is still E_INVALIDARG.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC bad = desc;
        bad.VS = {};
        Ref<ID3D12PipelineState> pso;
        CHECK(device.p->CreateGraphicsPipelineState(&bad, IID_PPV_ARGS(&pso.p)) == E_INVALIDARG && !pso.p);
    }
    _putenv_s("LEONIDA_STUB_PIPELINES", "");

    std::printf("%s\n", g_failed ? "FAILED" : "PASS");
    return g_failed ? 1 : 0;
}
