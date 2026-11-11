// SPDX-License-Identifier: GPL-3.0-or-later
// FSR 3 frame generation through the FidelityFX API 1.1.4 (framegen_ffx.h).
#include "extras/framegen_ffx.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <string>

#include "extras/ffx_files.h"
#include "extras/framegen.h"
#include "nxrt/config.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

#include "../../../third_party/amd_ffx_api_fg/ffx_api.h"
#include "../../../third_party/amd_ffx_api_fg/ffx_api_types.h"
#include "../../../third_party/amd_ffx_api_fg/ffx_framegeneration.h"
#include "../../../third_party/amd_ffx_api_fg/vk/ffx_api_vk.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "framegen";

struct Ffx {
    HMODULE mod = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;

    gfx::Device* dev = nullptr;
    VkQueueInfoFFXAPI game = {}, present = {}, acquire = {}, compute = {};
    PFN_vkCreateSwapchainKHR orig_create = nullptr;
    PFN_vkDestroySwapchainKHR orig_destroy = nullptr;
    PFN_vkGetSwapchainImagesKHR orig_images = nullptr;
    PFN_vkAcquireNextImageKHR orig_acquire = nullptr;
    PFN_vkQueuePresentKHR orig_present = nullptr;
    PFN_vkSetHdrMetadataEXT orig_hdr = nullptr;

    bool failed = false;
    ffxContext sc_ctx = nullptr, fg_ctx = nullptr;
    ffxQueryDescSwapchainReplacementFunctionsVK fns = {};
    VkSwapchainKHR sc = VK_NULL_HANDLE, dead = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {}, fg_display = {}, fg_max = {};
    bool fg_inverted = true;
    uint64_t frame_id = 1;
    bool logged = false;
    std::atomic<bool> reset_pending{false};   // the frame's inputs had the game's reset (camera cut): the next interpolation resets
    // [dlss] frame_gen_dump_every: a real frame, the generated one after it and the next real one, read back from the present callback
    struct Slot {
        gfx::Buffer buf;
        uint32_t w = 0, h = 0;
        uint64_t frame = 0;
        bool generated = false;
    };
    Slot slot[3];
    std::atomic<int> next{-1};          // the slot the present callback fills next (-1: none armed, 3: all filled)
    uint64_t armed_at = 0;
    int dumps = 0;
    std::chrono::steady_clock::time_point installed = std::chrono::steady_clock::now();
};
Ffx& g() {
    static Ffx f;
    return f;
}

// once-only progress lines while the FFX path is new (where a fault happens)
void step(const char* what) {
    static std::string seen;
    if (seen.find(what) != std::string::npos) return;
    seen += what;
    seen += '|';
    NXLOG_I(kTag, "FSR 3 frame generation: %s", what);
}

bool load() {
    Ffx& f = g();
    if (f.mod) return true;
    const std::wstring path = ffx_find_framegen().dll;
    if (path.empty()) return false;
    f.mod = LoadLibraryW(path.c_str());
    if (!f.mod) {
        NXLOG_E(kTag, "FSR 3 frame generation: LoadLibrary of amd_fidelityfx_vk.dll failed (error %lu)", GetLastError());
        return false;
    }
    f.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(f.mod, "ffxCreateContext"));
    f.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(f.mod, "ffxDestroyContext"));
    f.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(f.mod, "ffxConfigure"));
    f.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(f.mod, "ffxQuery"));
    f.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(f.mod, "ffxDispatch"));
    if (!f.create || !f.destroy || !f.configure || !f.query || !f.dispatch) {
        NXLOG_E(kTag, "amd_fidelityfx_vk.dll lacks the FidelityFX API");
        FreeLibrary(f.mod);
        f.mod = nullptr;
        return false;
    }
    return true;
}

ffxReturnCode_t fg_dispatch(ffxDispatchDescFrameGeneration* params, void* ctx);   // below
// Teardown order (AMD's samples): frame generation switched off on the swapchain first (it stops calling into the context and drops the
// HUD-less image it holds), then the FFX swapchain, then the frame generation context. A context destroyed while the swapchain still
// generates with it leaves the swapchain's present thread calling into freed state.
void disable_fg(Ffx& f) {
    if (!f.fg_ctx || !f.sc) return;
    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = reinterpret_cast<void*>(f.sc);
    c.frameGenerationCallback = &fg_dispatch;
    c.frameGenerationCallbackUserContext = &f.fg_ctx;
    c.presentCallback = nullptr;
    c.frameGenerationEnabled = false;
    c.allowAsyncWorkloads = false;
    c.HUDLessColor = FfxApiResource{};
    c.frameID = f.frame_id;
    const ffxReturnCode_t r = f.configure(&f.fg_ctx, &c.header);
    if (r != FFX_API_RETURN_OK) NXLOG_W(kTag, "FSR 3 frame generation: switching it off before the teardown failed (%u)", r);
}

void destroy_fg_context(Ffx& f) {
    if (f.fg_ctx) f.destroy(&f.fg_ctx, nullptr);
    f.fg_ctx = nullptr;
}

void destroy_contexts() {
    Ffx& f = g();
    disable_fg(f);
    if (f.sc_ctx) f.destroy(&f.sc_ctx, nullptr);   // destroys its swapchain
    f.sc_ctx = nullptr;
    f.fns = {};
    destroy_fg_context(f);
}

// The swapchain's frame pacing ([dlss] frame_gen_pacing). AMD's default paces to the recent average frame time less a 0.1 ms margin and,
// after a heavy stretch, can stay at the slow target (ffx_api_types.h: "gets stuck at targeting ~33ms"; the game then runs slower with
// frame generation than without it). `game` (the default) sets AMD's "tuning set B" for that case: 0.01 ms margin, variance factor 0.3.
// `amd` keeps AMD's defaults. The v1.1.4 Vulkan swapchain takes only these two values (not hybrid spin) and its configure function
// returns an error code even when it has applied them (its `return FFX_OK` is unreachable), so that code is not an error here.
void tune_pacing(Ffx& f) {
    static const std::string mode = config_get_str("dlss", "frame_gen_pacing", "game");
    if (mode == "amd") return;
    FfxApiSwapchainFramePacingTuning t = {};
    t.safetyMarginInMs = 0.01f;
    t.varianceFactor = 0.3f;
    t.allowHybridSpin = false;
    t.hybridSpinTime = 2;
    t.allowWaitForSingleObjectOnFence = false;
    ffxConfigureDescFrameGenerationSwapChainKeyValueVK kv = {};
    kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_VK;
    kv.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
    kv.ptr = &t;
    const ffxReturnCode_t r = f.configure(&f.sc_ctx, &kv.header);
    static bool told = false;
    if (!told) {
        told = true;
        if (r == FFX_API_RETURN_OK || r == FFX_API_RETURN_ERROR_RUNTIME_ERROR)
            NXLOG_I(kTag, "FSR 3 frame pacing: margin 0.01 ms, variance factor 0.3 ([dlss] frame_gen_pacing = amd keeps AMD's defaults)");
        else
            NXLOG_W(kTag, "FSR 3 frame pacing tuning not taken (%u): AMD's defaults", r);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_create(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    Ffx& f = g();
    if (f.failed) return f.orig_create(d, ci, a, out);
    if (f.sc_ctx && f.fns.pOutCreateSwapchainFFXAPI && f.fns.pOutDestroySwapchainFFXAPI) {
        // recreated (the renderer has idled the device): FFX's own destroy, then create; its context stays. The renderer's later destroy of
        // the old handle is skipped (the new object may even reuse its address).
        disable_fg(f);
        if (f.sc) f.fns.pOutDestroySwapchainFFXAPI(d, f.sc, a, f.sc_ctx);
        destroy_fg_context(f);      // after the swapchain that used it (ensure_fg_context makes a new one for the new size)
        f.dead = f.sc;
        f.sc = VK_NULL_HANDLE;
        VkSwapchainCreateInfoKHR c2 = *ci;
        c2.oldSwapchain = VK_NULL_HANDLE;
        VkSwapchainKHR sc = VK_NULL_HANDLE;
        const VkResult vr = f.fns.pOutCreateSwapchainFFXAPI(d, &c2, a, &sc, f.sc_ctx);
        if (vr != VK_SUCCESS || !sc) {
            NXLOG_E(kTag, "FSR 3 frame generation: the swapchain was not remade (%d)", static_cast<int>(vr));
            return vr != VK_SUCCESS ? vr : VK_ERROR_INITIALIZATION_FAILED;
        }
        f.sc = sc;
        f.format = ci->imageFormat;
        f.extent = ci->imageExtent;
        *out = sc;
        NXLOG_I(kTag, "FSR 3 frame generation swapchain remade %ux%u", ci->imageExtent.width, ci->imageExtent.height);
        tune_pacing(f);
        return VK_SUCCESS;
    }
    ffxCreateContextDescFrameGenerationSwapChainVK desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    desc.physicalDevice = f.dev->physical();
    desc.device = d;
    desc.swapchain = &sc;
    desc.allocator = nullptr;
    desc.createInfo = *ci;
    desc.createInfo.oldSwapchain = VK_NULL_HANDLE;
    desc.gameQueue = f.game;
    desc.asyncComputeQueue = f.compute;
    desc.presentQueue = f.present;
    desc.imageAcquireQueue = f.acquire;
    const ffxReturnCode_t r = f.create(&f.sc_ctx, &desc.header, nullptr);
    if (r != FFX_API_RETURN_OK || !sc) {
        NXLOG_E(kTag, "FSR 3 frame generation: the swapchain context was not made (%u): plain swapchain", r);
        f.sc_ctx = nullptr;
        f.failed = true;
        return f.orig_create(d, ci, a, out);
    }
    f.fns = {};
    f.fns.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (f.query(&f.sc_ctx, &f.fns.header) != FFX_API_RETURN_OK || !f.fns.pOutQueuePresentKHR || !f.fns.pOutAcquireNextImageKHR ||
        !f.fns.pOutGetSwapchainImagesKHR) {
        NXLOG_E(kTag, "FSR 3 frame generation: no replacement swapchain functions: plain swapchain");
        destroy_contexts();
        f.failed = true;
        return f.orig_create(d, ci, a, out);
    }
    f.sc = sc;
    f.format = ci->imageFormat;
    f.extent = ci->imageExtent;
    *out = sc;
    NXLOG_I(kTag, "FSR 3 frame generation swapchain %ux%u, format %d", ci->imageExtent.width, ci->imageExtent.height, static_cast<int>(ci->imageFormat));
    tune_pacing(f);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hook_destroy(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    Ffx& f = g();
    if (sc && sc == f.dead) {
        f.dead = VK_NULL_HANDLE;
        return;
    }
    if (sc && sc == f.sc && f.sc_ctx) {
        destroy_contexts();
        f.sc = VK_NULL_HANDLE;
        return;
    }
    f.orig_destroy(d, sc, a);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_images(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* imgs) {
    Ffx& f = g();
    return sc == f.sc && f.sc_ctx ? f.fns.pOutGetSwapchainImagesKHR(d, sc, n, imgs) : f.orig_images(d, sc, n, imgs);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore s, VkFence fe, uint32_t* i) {
    Ffx& f = g();
    return sc == f.sc && f.sc_ctx ? f.fns.pOutAcquireNextImageKHR(d, sc, t, s, fe, i) : f.orig_acquire(d, sc, t, s, fe, i);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_present(VkQueue q, const VkPresentInfoKHR* pi) {
    Ffx& f = g();
    if (f.sc_ctx && pi && pi->swapchainCount == 1 && pi->pSwapchains[0] == f.sc) {
        if (f.fg_ctx) step("first present with the frame generation context");
        const VkResult r = f.fns.pOutQueuePresentKHR(q, pi);
        if (f.fg_ctx) step("first present with the frame generation context done");
        return r;
    }
    return f.orig_present(q, pi);
}

VKAPI_ATTR void VKAPI_CALL hook_hdr(VkDevice d, uint32_t n, const VkSwapchainKHR* scs, const VkHdrMetadataEXT* md) {
    Ffx& f = g();
    if (f.sc_ctx && n == 1 && scs[0] == f.sc && f.fns.pOutSetHdrMetadataEXT) f.fns.pOutSetHdrMetadataEXT(d, n, scs, md);
    else if (f.orig_hdr) f.orig_hdr(d, n, scs, md);
}

void on_message(uint32_t type, const wchar_t* msg) {
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, msg ? msg : L"", -1, buf, sizeof buf, nullptr, nullptr);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) NXLOG_E(kTag, "FidelityFX: %s", buf);
    else NXLOG_W(kTag, "FidelityFX: %s", buf);
}

VkImageLayout layout_of(uint32_t state) {
    switch (state) {
        case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        case FFX_API_RESOURCE_STATE_PIXEL_READ:
        case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case FFX_API_RESOURCE_STATE_COPY_SRC: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case FFX_API_RESOURCE_STATE_COPY_DEST: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case FFX_API_RESOURCE_STATE_PRESENT: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case FFX_API_RESOURCE_STATE_RENDER_TARGET: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        default: return VK_IMAGE_LAYOUT_GENERAL;
    }
}

void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Only with frame_gen_dump_every / frame_gen_dump_at: the composition of each presented frame (real or generated) into the swapchain image
// (a copy, with the layout changes for the states FFX reports), plus the read-back of the armed frames.
ffxReturnCode_t compose(ffxCallbackDescFrameGenerationPresent* d, void*) {
    Ffx& f = g();
    auto cb = static_cast<VkCommandBuffer>(d->commandList);
    auto src = static_cast<VkImage>(d->currentBackBuffer.resource);
    auto dst = static_cast<VkImage>(d->outputSwapChainBuffer.resource);
    const VkImageLayout sl = layout_of(d->currentBackBuffer.state), dl = layout_of(d->outputSwapChainBuffer.state);
    const uint32_t w = d->currentBackBuffer.description.width, h = d->currentBackBuffer.description.height;
    img_barrier(cb, src, sl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    img_barrier(cb, dst, dl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy r = {};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {std::min(w, d->outputSwapChainBuffer.description.width), std::min(h, d->outputSwapChainBuffer.description.height), 1};
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    const int k = f.next.load();
    const bool want_generated = k == 1;
    if (k >= 0 && k < 3 && d->isGeneratedFrame == want_generated && f.slot[k].buf.buffer &&
        static_cast<VkDeviceSize>(w) * h * 4 <= f.slot[k].buf.size) {
        VkBufferImageCopy bc = {};
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.slot[k].buf.buffer, 1, &bc);
        f.slot[k].w = w;
        f.slot[k].h = h;
        f.slot[k].frame = d->frameID;
        f.slot[k].generated = d->isGeneratedFrame;
        f.next.store(k + 1);
    }
    img_barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sl);
    img_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dl);
    return FFX_API_RETURN_OK;
}

int64_t dump_every() {
    static const int64_t v = config_get_int("dlss", "frame_gen_dump_every", 0);
    return v;
}

// [dlss] frame_gen_dump_at: seconds after the FFX swapchain was installed, a comma list (a dump at each)
const std::vector<double>& dump_at() {
    static const std::vector<double> v = [] {
        std::vector<double> out;
        const std::string s = config_get_str("dlss", "frame_gen_dump_at", "");
        size_t i = 0;
        while (i < s.size()) {
            const size_t j = s.find(',', i);
            const std::string t = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (!t.empty()) out.push_back(std::atof(t.c_str()));
            if (j == std::string::npos) break;
            i = j + 1;
        }
        std::sort(out.begin(), out.end());
        return out;
    }();
    return v;
}
bool dumping() { return dump_every() > 0 || !dump_at().empty(); }

// Render thread: arm a dump every N frames (4 at most), write the PNGs once the frames are surely done.
void dump_tick(Ffx& f, VkExtent2D bb, VkFormat fmt) {
    if (!dumping()) return;
    const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - f.installed).count();
    const int k = f.next.load();
    if (k == 3 && f.frame_id - f.armed_at > 12) {
        for (int i = 0; i < 3; ++i) {
            Ffx::Slot& sl = f.slot[i];
            f.dev->invalidate_buffer(sl.buf);
            std::vector<uint8_t> px(static_cast<size_t>(sl.w) * sl.h * 4);
            std::memcpy(px.data(), sl.buf.mapped, px.size());
            const bool bgr = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
            for (size_t p = 0; p < px.size(); p += 4) {
                if (bgr) std::swap(px[p], px[p + 2]);
                px[p + 3] = 255;
            }
            char name[96];
            std::snprintf(name, sizeof name, "fg_%06llu_%d_%s.png", static_cast<unsigned long long>(f.armed_at), i, sl.generated ? "generated" : "real");
            if (i == 0) NXLOG_I(kTag, "FSR 3 frame generation: dump %d at %.1f s after the swapchain install", f.dumps + 1, since);
            gfx::save_png_rgba(name, px.data(), sl.w, sl.h);
            NXLOG_I(kTag, "FSR 3 frame generation: wrote %s (frame %llu)", name, static_cast<unsigned long long>(sl.frame));
        }
        f.next.store(-1);
        ++f.dumps;
    }
    const std::vector<double>& at = dump_at();
    const bool due = dump_every() > 0 ? f.dumps < 4 && f.frame_id % static_cast<uint64_t>(dump_every()) == 0
                                      : static_cast<size_t>(f.dumps) < at.size() && since >= at[static_cast<size_t>(f.dumps)];
    if (k == -1 && due) {
        const VkDeviceSize size = static_cast<VkDeviceSize>(bb.width) * bb.height * 4;
        for (Ffx::Slot& sl : f.slot) {
            if (sl.buf.buffer && sl.buf.size >= size) continue;
            if (sl.buf.buffer) f.dev->destroy_buffer(sl.buf);
            if (f.dev->create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &sl.buf, "framegen dump") != VK_SUCCESS) return;
        }
        f.armed_at = f.frame_id;
        f.next.store(0);
    }
}

ffxReturnCode_t fg_dispatch(ffxDispatchDescFrameGeneration* params, void* ctx) {
    step("first interpolation dispatch (present thread)");
    // the game's camera cut (its DLSS reset): the frame between the old view and the new one is not interpolated from the old one
    if (g().reset_pending.exchange(false)) params->reset = true;
    const ffxReturnCode_t r = g().dispatch(static_cast<ffxContext*>(ctx), &params->header);
    step("first interpolation dispatch done");
    return r;
}

// FFX's backend asks for KHR aliases of core functions (vkGetBufferMemoryRequirements2KHR) and calls them unchecked; on a 1.3 device
// without the old extensions those names give null: fall back to the core name.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char* name) {
    if (PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name)) return f;
    const size_t n = name ? std::strlen(name) : 0;
    if (n > 3 && std::strcmp(name + n - 3, "KHR") == 0) return vkGetDeviceProcAddr(d, std::string(name, n - 3).c_str());
    return nullptr;
}

FfxApiResource res(VkImage img, uint32_t fmt, VkExtent2D e, uint32_t usage) {
    FfxApiResource r = {};
    r.resource = reinterpret_cast<void*>(img);
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;   // GENERAL
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = fmt;
    r.description.width = e.width;
    r.description.height = e.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    return r;
}

bool ensure_fg_context(const FfxFgFrame& fr) {
    Ffx& f = g();
    const bool fits = f.fg_ctx && f.fg_display.width == fr.bb.width && f.fg_display.height == fr.bb.height && fr.render.width <= f.fg_max.width &&
                      fr.render.height <= f.fg_max.height && f.fg_inverted == fr.depth_inverted;
    if (fits) return true;
    disable_fg(f);              // the swapchain stays: it must stop generating with the context before it goes
    destroy_fg_context(f);
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.vkDevice = f.dev->device();
    be.vkPhysicalDevice = f.dev->physical();
    be.vkDeviceProcAddr = &device_proc;
    ffxCreateContextDescFrameGenerationHudless hd = {};
    hd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hd.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(f.format);
    ffxCreateContextDescFrameGeneration cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.header.pNext = &be.header;
    be.header.pNext = &hd.header;
    cd.flags = (fr.depth_inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0u) | FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    cd.displaySize = {fr.bb.width, fr.bb.height};
    const VkExtent2D mx{std::max(fr.render.width, fr.bb.width), std::max(fr.render.height, fr.bb.height)};
    cd.maxRenderSize = {mx.width, mx.height};
    cd.backBufferFormat = ffxApiGetSurfaceFormatVK(f.format);
    step("creating the frame generation context");
    const ffxReturnCode_t r = f.create(&f.fg_ctx, &cd.header, nullptr);
    if (r == FFX_API_RETURN_OK) {
        ffxConfigureDescGlobalDebug1 dbg = {};
        dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        dbg.fpMessage = &on_message;
        dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
        f.configure(&f.fg_ctx, &dbg.header);
    }
    if (r != FFX_API_RETURN_OK) {
        NXLOG_E(kTag, "FSR 3 frame generation context %ux%u was not made (%u)", fr.bb.width, fr.bb.height, r);
        f.fg_ctx = nullptr;
        return false;
    }
    f.fg_display = fr.bb;
    f.fg_max = mx;
    f.fg_inverted = fr.depth_inverted;
    NXLOG_I(kTag, "FSR 3 frame generation context: display %ux%u, render up to %ux%u", fr.bb.width, fr.bb.height, mx.width, mx.height);
    return true;
}

}  // namespace

bool ffx_fg_files() { return !ffx_find_framegen().dll.empty(); }

bool ffx_fg_wanted() {
    static const bool v = [] {
        if (framegen_mode() != 3) return false;
        const FfxFind f = ffx_find_framegen();
        if (f.dll.empty()) {
            NXLOG_W(kTag, "[dlss] frame_gen = fsr3, but %s: put it in upscalers\\fsr next to the launcher (the release package has it there) or "
                          "name its folder in [dlss] frame_gen_dll; frame generation stays off", f.missing.c_str());
            return false;
        }
        NXLOG_I(kTag, "FSR 3 frame generation: %s", ffx_narrow(f.dll).c_str());
        return true;
    }();
    return v;
}

uint32_t ffx_fg_extra_queues(uint32_t family_queue_count) {
    if (!ffx_fg_wanted()) return 0;
    if (family_queue_count < 4) {
        NXLOG_W(kTag, "FSR 3 frame generation needs 4 queues in the graphics family, it has %u: off", family_queue_count);
        return 0;
    }
    return 3;
}

void ffx_fg_install(gfx::Device& dev, uint32_t extra_queues) {
    if (extra_queues < 3 || !load()) return;
    Ffx& f = g();
    f.dev = &dev;
    f.installed = std::chrono::steady_clock::now();
    const uint32_t fam = dev.queue_family();
    f.game = {dev.queue(), fam, nullptr};   // FFX uses it only inside vkQueuePresentKHR, which the renderer calls under its queue lock
    VkQueue q[3] = {};
    for (uint32_t i = 0; i < 3; ++i) vkGetDeviceQueue(dev.device(), fam, i + 1, &q[i]);
    f.present = {q[0], fam, nullptr};
    f.acquire = {q[1], fam, nullptr};
    f.compute = {q[2], fam, nullptr};
    f.orig_create = vkCreateSwapchainKHR;
    f.orig_destroy = vkDestroySwapchainKHR;
    f.orig_images = vkGetSwapchainImagesKHR;
    f.orig_acquire = vkAcquireNextImageKHR;
    f.orig_present = vkQueuePresentKHR;
    f.orig_hdr = vkSetHdrMetadataEXT;
    vkCreateSwapchainKHR = &hook_create;
    vkDestroySwapchainKHR = &hook_destroy;
    vkGetSwapchainImagesKHR = &hook_images;
    vkAcquireNextImageKHR = &hook_acquire;
    vkQueuePresentKHR = &hook_present;
    if (f.orig_hdr) vkSetHdrMetadataEXT = &hook_hdr;
    NXLOG_I(kTag, "FSR 3 frame generation: amd_fidelityfx_vk.dll loaded, swapchain calls go through it (3 extra queues in family %u)", fam);
}

bool ffx_fg_frame(VkCommandBuffer cb, const FfxFgFrame& fr) {
    Ffx& f = g();
    if (!f.sc_ctx) return false;
    const bool gen = fr.enable && fr.inputs && fr.depth && fr.motion && ensure_fg_context(fr);
    if (!f.fg_ctx) {
        ++f.frame_id;
        return true;
    }
    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = reinterpret_cast<void*>(f.sc);
    c.frameGenerationCallback = &fg_dispatch;
    if (dumping()) c.presentCallback = &compose;
    c.frameGenerationCallbackUserContext = &f.fg_ctx;
    c.frameGenerationEnabled = gen;
    c.allowAsyncWorkloads = false;
    if (gen && fr.hudless)
        c.HUDLessColor = res(fr.hudless, ffxApiGetSurfaceFormatVK(f.format), fr.bb, FFX_API_RESOURCE_USAGE_READ_ONLY | FFX_API_RESOURCE_USAGE_RENDERTARGET);
    c.flags = 0;
    c.onlyPresentGenerated = false;
    c.generationRect = {fr.rect[0], fr.rect[1], fr.rect[2], fr.rect[3]};
    c.frameID = f.frame_id;
    step(gen ? "configure (generation on)" : "configure (generation off)");
    ffxReturnCode_t r = f.configure(&f.fg_ctx, &c.header);
    if (r != FFX_API_RETURN_OK && !f.logged) {
        f.logged = true;
        NXLOG_E(kTag, "FSR 3 frame generation: configure failed (%u)", r);
    }
    if (gen) {
        ffxDispatchDescFrameGenerationPrepareCameraInfo cam = {};
        cam.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        cam.cameraUp[1] = 1.0f;
        cam.cameraRight[0] = 1.0f;
        cam.cameraForward[2] = 1.0f;
        ffxDispatchDescFrameGenerationPrepare p = {};
        p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        p.header.pNext = &cam.header;
        p.frameID = f.frame_id;
        p.flags = 0;
        p.commandList = cb;
        p.renderSize = {fr.render.width, fr.render.height};
        p.jitterOffset = {fr.jitter[0], fr.jitter[1]};
        p.motionVectorScale = {fr.mv_scale[0], fr.mv_scale[1]};
        p.frameTimeDelta = fr.frame_ms;
        p.unused_reset = fr.reset;
        if (fr.reset) f.reset_pending.store(true);
        p.cameraNear = 0.1f;
        p.cameraFar = 1000.0f;
        p.cameraFovAngleVertical = 1.0f;
        p.viewSpaceToMetersFactor = 1.0f;
        p.depth = res(fr.depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, fr.render, FFX_API_RESOURCE_USAGE_READ_ONLY | FFX_API_RESOURCE_USAGE_UAV);
        p.motionVectors = res(fr.motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, fr.render, FFX_API_RESOURCE_USAGE_READ_ONLY | FFX_API_RESOURCE_USAGE_UAV);
        step("prepare dispatch");
        r = f.dispatch(&f.fg_ctx, &p.header);
        step("prepare dispatch done");
        if (r != FFX_API_RETURN_OK && !f.logged) {
            f.logged = true;
            NXLOG_E(kTag, "FSR 3 frame generation: prepare failed (%u)", r);
        }
    }
    dump_tick(f, fr.bb, f.format);
    ++f.frame_id;
    return true;
}

bool ffx_fg_owns_swapchain() {
    Ffx& f = g();
    return f.sc_ctx && f.sc && !f.failed;
}

uint64_t ffx_fg_present_count() {
    Ffx& f = g();
    return f.sc_ctx && f.fns.pOutGetLastPresentCountFFXAPI ? f.fns.pOutGetLastPresentCountFFXAPI(f.sc) : 0;
}

}  // namespace nxrt::extras
