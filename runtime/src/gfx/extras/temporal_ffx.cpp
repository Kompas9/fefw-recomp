// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 through the FidelityFX API (temporal.h). The Vulkan DLLs are loaded at run time (ffx_files.h: the FSR SDK v2.4.0
// amd_fidelityfx_upscaler_vk.dll, through amd_fidelityfx_loader_vk.dll when it sits beside it, or the v1.1.4 amd_fidelityfx_vk.dll; MIT,
// signed by AMD). The upscaler version is the newest the DLL offers for this GPU (FSR 3.1.x; FSR 4 where AMD supports it).
#include <windows.h>

#include <cstdio>
#include <string>

#include "extras/ffx_files.h"
#include "extras/temporal.h"
#include "nxrt/extras.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

#include "../../../third_party/amd_ffx_api/api/include/ffx_api.h"
#include "../../../third_party/amd_ffx_api/api/include/ffx_api_types.h"
#include "../../../third_party/amd_ffx_api/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/amd_ffx_api/upscalers/include/ffx_upscale.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "fsr3";

void on_message(uint32_t type, const wchar_t* msg) {
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, msg ? msg : L"", -1, buf, sizeof buf, nullptr, nullptr);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) NXLOG_E(kTag, "%s", buf);
    else NXLOG_W(kTag, "%s", buf);
}

FfxApiResource resource(const TemporalImage& t, uint32_t state, uint32_t usage) {
    FfxApiResource r = {};
    r.resource = reinterpret_cast<void*>(t.image);
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = t.format == VK_FORMAT_R16G16B16A16_SFLOAT ? FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT
                         : t.format == VK_FORMAT_R16G16_SFLOAT     ? FFX_API_SURFACE_FORMAT_R16G16_FLOAT
                         : t.format == VK_FORMAT_R32_SFLOAT        ? FFX_API_SURFACE_FORMAT_R32_FLOAT
                                                                   : FFX_API_SURFACE_FORMAT_UNKNOWN;
    r.description.width = t.width;
    r.description.height = t.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    return r;
}

class Ffx final : public TemporalBackend {
public:
    const char* name() const override { return version_.empty() ? "FSR 3" : version_.c_str(); }

    bool load(gfx::Device& dev) {
        dev_ = &dev;
        const FfxFind found = ffx_find_upscaler();
        const std::wstring& path = found.dll;
        if (path.empty()) {
            NXLOG_W(kTag, "FSR 3 upscaling is not available: %s. Put AMD's DLLs in upscalers\\fsr next to the launcher (the release package has "
                          "them there) or name their folder in [upscale] fsr_dll; the built-in upscaler is used", found.missing.c_str());
            return false;
        }
        mod_ = LoadLibraryW(path.c_str());
        if (!mod_) {
            NXLOG_E(kTag, "LoadLibrary of %s failed (error %lu): the built-in upscaler is used", ffx_narrow(path).c_str(), GetLastError());
            return false;
        }
        create_ = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(mod_, "ffxCreateContext"));
        destroy_ = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(mod_, "ffxDestroyContext"));
        query_ = reinterpret_cast<PfnFfxQuery>(GetProcAddress(mod_, "ffxQuery"));
        dispatch_ = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(mod_, "ffxDispatch"));
        if (!create_ || !destroy_ || !query_ || !dispatch_) {
            NXLOG_E(kTag, "the DLL does not export the FidelityFX API");
            return false;
        }
        NXLOG_I(kTag, "FidelityFX API loaded from %s (FidelityFX SDK %s files)", ffx_narrow(path).c_str(), found.version.c_str());
        return true;
    }

    bool evaluate(VkCommandBuffer cb, const TemporalFrame& f) override {
        if (failed_) return false;
        if (!ctx_ || f.render.width > max_render_.width || f.render.height > max_render_.height || f.out.width != max_out_.width || f.out.height != max_out_.height) {
            if (!make(f.render, f.out)) {
                failed_ = true;
                return false;
            }
        }
        ffxDispatchDescUpscale d = {};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = reinterpret_cast<void*>(cb);
        d.color = resource(f.color, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        d.depth = resource(f.depth, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        d.motionVectors = resource(f.motion, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        d.output = resource(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        d.jitterOffset = {f.jitter_px[0], f.jitter_px[1]};
        d.motionVectorScale = {f.mv_scale[0], f.mv_scale[1]};
        d.renderSize = {f.render.width, f.render.height};
        d.upscaleSize = {f.out.width, f.out.height};
        d.enableSharpening = f.sharpness > 0.0f;
        d.sharpness = f.sharpness;
        d.frameTimeDelta = f.frame_ms;
        d.preExposure = 1.0f;
        d.reset = f.reset;
        d.cameraNear = f.near_z;   // FSR orders near / far itself from the inverted-depth flag
        d.cameraFar = f.far_z;
        d.cameraFovAngleVertical = f.fov_y;
        d.viewSpaceToMetersFactor = 1.0f;
        const ffxReturnCode_t r = dispatch_(&ctx_, &d.header);
        if (r != FFX_API_RETURN_OK) {
            NXLOG_E(kTag, "ffxDispatch failed (%u): FSR 3 off", r);
            failed_ = true;
            return false;
        }
        return true;
    }

private:
    bool make(VkExtent2D render, VkExtent2D out) {
        if (ctx_) {
            dev_->wait_idle();
            destroy_(&ctx_, nullptr);
            ctx_ = nullptr;
        }
        ffxCreateBackendVKDesc be = {};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.deviceContext.vkDevice = dev_->device();
        be.deviceContext.vkPhysicalDevice = dev_->physical();
        be.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
        ffxCreateContextDescUpscale cd = {};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.header.pNext = &be.header;
        cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        cd.maxRenderSize = {render.width, render.height};
        cd.maxUpscaleSize = {out.width, out.height};
        cd.fpMessage = &on_message;
        ffxReturnCode_t r = create_(&ctx_, &cd.header, nullptr);
        if (r != FFX_API_RETURN_OK) {   // the v1.1.4 DLL knows only the older backend descriptor
            ffxCreateBackendVKDesc_DEPRECATED old = {};
            old.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
            old.vkDevice = dev_->device();
            old.vkPhysicalDevice = dev_->physical();
            old.vkDeviceProcAddr = vkGetDeviceProcAddr;
            cd.header.pNext = &old.header;
            ctx_ = nullptr;
            r = create_(&ctx_, &cd.header, nullptr);
        }
        if (r != FFX_API_RETURN_OK) {
            NXLOG_E(kTag, "ffxCreateContext (upscale %ux%u -> %ux%u) failed (%u): FSR 3 off", render.width, render.height, out.width, out.height, r);
            ctx_ = nullptr;
            return false;
        }
        max_render_ = render;
        max_out_ = out;
        ffxQueryGetProviderVersion pv = {};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (query_(&ctx_, &pv.header) == FFX_API_RETURN_OK && pv.versionName) version_ = std::string("FSR ") + pv.versionName;
        NXLOG_I(kTag, "context made: %s, %ux%u -> %ux%u, HDR, inverted depth, auto exposure", name(), render.width, render.height, out.width, out.height);
        return true;
    }

    gfx::Device* dev_ = nullptr;
    HMODULE mod_ = nullptr;
    PfnFfxCreateContext create_ = nullptr;
    PfnFfxDestroyContext destroy_ = nullptr;
    PfnFfxQuery query_ = nullptr;
    PfnFfxDispatch dispatch_ = nullptr;
    ffxContext ctx_ = nullptr;
    VkExtent2D max_render_ = {}, max_out_ = {};
    std::string version_;
    bool failed_ = false;
};

}  // namespace

bool temporal_ffx_available() { return !ffx_find_upscaler().dll.empty(); }

TemporalBackend* temporal_ffx(void* gfx_device) {
    static Ffx* inst = nullptr;
    static bool tried = false;
    if (!tried && gfx_device) {
        tried = true;
        auto* f = new Ffx();
        if (f->load(*static_cast<gfx::Device*>(gfx_device))) inst = f;
        else delete f;
    }
    return inst;
}

}  // namespace nxrt::extras
