// SPDX-License-Identifier: GPL-3.0-or-later
// FSR 3 frame generation ([dlss] frame_gen = fsr3, notes/framegen.md): the FidelityFX API 1.1.4 (amd_fidelityfx_vk.dll; found by
// ffx_files.h). Its swapchain replaces the renderer's: the swapchain functions of volk are pointed at
// wrappers when the device is made. It needs no hardware GPU scheduling (unlike DLSS-G).
#pragma once
#include <volk.h>

#include <cstdint>

namespace nxrt::gfx {
class Device;
}

namespace nxrt::extras {

bool ffx_fg_wanted();   // [dlss] frame_gen = fsr3 and amd_fidelityfx_vk.dll found (read once)
bool ffx_fg_files();    // amd_fidelityfx_vk.dll found (ffx_files.h), whatever [dlss] frame_gen says
// Device creation (vk_device.cpp): the queues to add to the graphics family (present, image acquire, async compute; 0 = none), then the
// install once the device and its first queue exist (before any swapchain).
uint32_t ffx_fg_extra_queues(uint32_t family_queue_count);
void ffx_fg_install(gfx::Device& dev, uint32_t extra_queues);

struct FfxFgFrame {
    VkImage depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE;   // render size, GENERAL, oriented as the backbuffer (framegen.cpp)
    VkExtent2D render = {};
    float jitter[2] = {}, mv_scale[2] = {};
    bool reset = false, depth_inverted = true, inputs = false, enable = false;
    VkImage hudless = VK_NULL_HANDLE;                           // backbuffer size and format, GENERAL; may be null
    VkExtent2D bb = {};
    int32_t rect[4] = {};
    float frame_ms = 16.7f;
};
// Render thread, before the present (cb: the present's command buffer, recording): configure and, with inputs, the prepare dispatch.
// False: FSR 3 frame generation is not running (no FFX swapchain).
bool ffx_fg_frame(VkCommandBuffer cb, const FfxFgFrame& f);
uint64_t ffx_fg_present_count();   // presents of the FFX swapchain so far (generated ones included)
// The window's swapchain is FFX's: the renderer hands its images to the present in SHADER_READ_ONLY_OPTIMAL, the layout FFX reads them in
// (VUID-vkCmdDraw-None-09600 with PRESENT_SRC_KHR).
bool ffx_fg_owns_swapchain();

}  // namespace nxrt::extras
