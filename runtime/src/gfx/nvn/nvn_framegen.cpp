// SPDX-License-Identifier: GPL-3.0-or-later
// HUD-less colour for frame generation (nvn_framegen.h).
#include "nvn_framegen.h"

#include <algorithm>
#include <string>
#include <vector>

#include "extras/framegen.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "framegen";

struct Img {
    gfx::Image img;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct State {
    bool after_eval = false;
    Texture* captured = nullptr;      // this frame's copy is of this texture (before its first blended draw after the evaluation)
    VkExtent2D cap_size = {};
    Texture* copied_to = nullptr;     // the window texture drawn after the capture (the game's copy of the captured picture into it)
    std::vector<Texture*> written;    // drawn without blending after the evaluation (the scene's passes)
    std::vector<Texture*> window;     // the textures of the window last presented
    VkFormat window_format = VK_FORMAT_UNDEFINED;
    Img copy[2], bb[2];
    uint32_t cur = 0;
    uint64_t frames = 0, evals = 0, used = 0, no_capture = 0, not_shown = 0;
    bool told_first = false, told_miss = false;
};
State& st() {
    static State s;
    return s;
}

VkFormat unorm_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return f;
    }
}

bool ensure(Img& i, VkExtent2D e, VkFormat fmt, VkImageUsageFlags usage, const char* name) {
    if (i.img.image && i.img.extent.width == e.width && i.img.extent.height == e.height && i.img.format == fmt) return true;
    if (i.img.image) {
        gfx::Image old = i.img;
        defer_destroy([old]() mutable { device().destroy_image(old); });
    }
    i = Img();
    gfx::ImageDesc d;
    d.format = fmt;
    d.width = e.width;
    d.height = e.height;
    d.usage = usage;
    d.name = name;
    std::string why;
    if (device().create_image(d, &i.img, &why) != VK_SUCCESS) {
        NXLOG_E(kTag, "%s %ux%u: not made (%s)", name, e.width, e.height, why.c_str());
        i = Img();
        return false;
    }
    return true;
}

void to(VkCommandBuffer cb, Img& i, VkImageLayout l) {
    image_barrier(cb, i.img.image, i.layout, l, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    i.layout = l;
}

bool is_window_texture(const State& s, Texture* t) {
    for (Texture* w : s.window)
        if (w == t) return true;
    return false;
}

// The texture's level 0 into copy[cur] (TRANSFER_DST layout afterwards). No pass open.
bool copy_texture(State& s, Frame& f, Texture* t) {
    if (!tex_gpu(t) || tex_is_depth(t)) return false;
    const VkExtent3D e = tex_image_extent(t, 0);
    Img& c = s.copy[s.cur];
    if (!ensure(c, VkExtent2D{e.width, e.height}, unorm_of(tex_vk_format(t)),
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "framegen hudless copy"))
        return false;
    tex_transition(t, f, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to(f.cb, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy r = {};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = e;
    vkCmdCopyImage(f.cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    memory_barrier(f.cb);
    return true;
}

}  // namespace

bool fg_hudless_on() {
    static const bool v = extras::framegen_requested() && config_get_bool("dlss", "frame_gen_hudless", true);
    return v;
}

void fg_note_eval() {
    if (fg_hudless_on()) st().after_eval = true;
}

bool fg_hudless_candidate(Texture* t, bool blended) {
    State& s = st();
    if (!s.after_eval || !t) return false;
    if (s.captured) {   // the game's copy of the captured picture into the window texture
        if (!s.copied_to && t != s.captured && is_window_texture(s, t)) s.copied_to = t;
        return false;
    }
    if (!blended) {
        if (std::find(s.written.begin(), s.written.end(), t) == s.written.end()) s.written.push_back(t);
        return false;
    }
    // the UI layer blended over the finished scene: a target of the picture's format that the scene's passes wrote (the UI layer itself
    // is only cleared before its blended draws; the HDR targets of the post effects have another format)
    if (is_window_texture(s, t)) return true;
    return s.window_format != VK_FORMAT_UNDEFINED && tex_vk_format(t) == s.window_format &&
           std::find(s.written.begin(), s.written.end(), t) != s.written.end();
}

void fg_hudless_capture(Frame& f, Texture* t) {
    State& s = st();
    if (copy_texture(s, f, t)) {
        s.captured = t;
        const VkExtent3D e = tex_image_extent(t, 0);
        s.cap_size = VkExtent2D{e.width, e.height};
    } else {
        s.after_eval = false;
    }
}

void fg_hudless_present(Window* w, Texture* presented, Frame& f, VkExtent2D bb, VkFormat bb_format, bool again, const int32_t crop[4],
                        const FgPictureDraw& draw) {
    if (!fg_hudless_on()) return;
    State& s = st();
    if (w) {
        s.window.clear();
        for (const auto& t : w->textures) s.window.push_back(t.get());
    }
    if (presented && tex_gpu(presented)) s.window_format = tex_vk_format(presented);
    const bool eval = s.after_eval;
    Texture* cap = s.captured;
    Texture* copied = s.copied_to;
    const VkExtent2D cap_size = s.cap_size;
    s.after_eval = false;
    s.captured = s.copied_to = nullptr;
    s.written.clear();
    if (again || !eval || !presented || !extras::framegen_running()) return;
    ++s.evals;
    const auto report = [&] {
        if (s.evals % 600 == 0)
            NXLOG_I(kTag, "HUD-less colour: %llu of the last frames with a scene had it, %llu had no UI composite after the scene, %llu "
                          "composited one that was not shown", static_cast<unsigned long long>(s.used), static_cast<unsigned long long>(s.no_capture),
                    static_cast<unsigned long long>(s.not_shown));
        if (s.evals % 600 == 0) s.used = s.no_capture = s.not_shown = 0;
    };
    // the captured picture is the presented one, or was drawn into it whole (the size of the window's crop)
    const VkExtent3D pe = tex_image_extent(presented, 0);
    const bool cropped = crop[2] > 0 && crop[3] > 0;
    const uint32_t shown_w = cropped ? static_cast<uint32_t>(crop[2]) : pe.width, shown_h = cropped ? static_cast<uint32_t>(crop[3]) : pe.height;
    const bool direct = cap && cap == presented;
    const bool via_copy = cap && copied == presented && cap_size.width == shown_w && cap_size.height == shown_h;
    if (!direct && !via_copy) {
        ++(cap ? s.not_shown : s.no_capture);
        if (!s.told_miss) {
            s.told_miss = true;
            NXLOG_I(kTag, "HUD-less colour: a frame with a scene but no usable capture (captured t%u %ux%u, then drawn into t%u; presented t%u, "
                          "shown %ux%u): frame generation sees its UI",
                    cap ? cap->id : 0u, cap_size.width, cap_size.height, copied ? copied->id : 0u, presented->id, shown_w, shown_h);
        }
        report();
        return;
    }
    Img& c = s.copy[s.cur];
    Img& b = s.bb[s.cur];
    s.cur ^= 1u;
    if (!ensure(b, bb, bb_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                "framegen hudless"))
        return;
    to(f.cb, c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    b.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    to(f.cb, b, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const int32_t whole[4] = {0, 0, static_cast<int32_t>(cap_size.width), static_cast<int32_t>(cap_size.height)};
    draw(c.img.view, cap_size, direct ? crop : whole, b.img.view);
    to(f.cb, b, VK_IMAGE_LAYOUT_GENERAL);
    extras::framegen_hudless(f.cb, b.img.image, b.img.view, bb_format, bb);
    ++s.used;
    if (!s.told_first) {
        s.told_first = true;
        NXLOG_I(kTag, "HUD-less colour: the scene before the UI is t%u %ux%u (%s), passed to frame generation at %ux%u", cap->id, cap_size.width,
                cap_size.height, direct ? "the presented window texture" : "then drawn into the presented window texture", bb.width, bb.height);
    }
    report();
}

}  // namespace nxrt::nvn::be::gpu
