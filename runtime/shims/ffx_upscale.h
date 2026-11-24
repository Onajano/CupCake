// RT-344: AMD's PC FidelityFX upscaler (src/ffx_vk_backend) behind the Vulkan layer's Upscale commands.
//
// Off unless LEONIDA_FFX_PC=1. When on, a device of the layer enables the upscaler's extensions and install()s the
// replay hooks before it is handed out; create() then makes an upscale context on that device, the FFX shim records
// leonida::Upscale commands naming it (rt::d3d12::record_upscale), and replay records each through the context into
// its command buffer. The contexts live here, keyed by a number that is never reused.
//
// Thread safety: every function may be called from any thread. A context is used by one replay at a time.
// destroy() and uninstall() wait for the device to go idle under the submit lock (a VkQueue needs external
// synchronization), since replays already submitted may still use the context; that happens when a title changes
// resolution or ends, not per frame. An Upscale whose context is gone by replay is skipped.
#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <string>

#include "../src/ffx_vk_backend.h"

namespace leonida {
class VulkanContext;
}

namespace rt::ffx_upscale {

bool enabled();

// Called by the layer's device: install() once, before the device is handed out; uninstall() before it is destroyed
// (it destroys the device's contexts).
void install(leonida::VulkanContext& context, VkDevice device, VkPhysicalDevice physical,
             PFN_vkGetDeviceProcAddr get_device_proc);
void uninstall(VkDevice device);

// An upscale context on `device` (a device of the Vulkan layer, or the Xbox-ABI proxy over one) for `desc`'s flags
// and sizes, or 0 with `error` set: the upscaler is off, the device was created without it, or AMD's loader DLL is
// missing or refuses the description.
std::uint64_t create(ID3D12Device* device, const ffx::CreateContextDescUpscale& desc, std::string& error);
void destroy(std::uint64_t context);
// The provider the loader chose for `context`, e.g. "4.1.2"; empty when there is no such context.
std::string provider(std::uint64_t context);

// ffxQuery and ffxConfigure for the FFX shim; the descriptions are data and pass through unchanged, except:
// - with no context (0), they go to AMD's loader on its own. A GET_VERSIONS query names a D3D12 device, which is
//   replaced by that device's Vulkan handles; any other query gets the Vulkan backend of a device with the upscaler
//   chained in front of its own chain for the call, as the FFX API wants a backend for a context-less query.
// - an unknown context is FFX_API_RETURN_ERROR.
ffx::ReturnCode query(std::uint64_t context, ffx::Header* desc);
ffx::ReturnCode configure(std::uint64_t context, const ffx::Header* desc);

}  // namespace rt::ffx_upscale
