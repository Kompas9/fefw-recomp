// SPDX-License-Identifier: GPL-3.0-or-later
// Where the AMD FidelityFX DLLs are (FSR 3 upscaling and FSR 3 frame generation), and which one is missing when they are not. None of the
// DLLs is part of this repository; the release package carries AMD's signed builds (MIT) in upscalers\fsr\ next to the launcher.
//
//   upscaling          amd_fidelityfx_upscaler_vk.dll (FSR SDK v2.4.0, signedbin; FSR 3.1, or FSR 4 where AMD offers it), loaded through
//                      amd_fidelityfx_loader_vk.dll when that sits beside it; else amd_fidelityfx_vk.dll (FidelityFX SDK v1.1.4, FSR 3.1)
//   frame generation   amd_fidelityfx_vk.dll (FidelityFX SDK v1.1.4: the Vulkan frame-generation swapchain)
//
// Search, for each: the setting ([upscale] fsr_dll; for frame generation [dlss] frame_gen_dll, else fsr_dll: a folder, or a file in it),
// then upscalers\fsr\ next to the executable, next to the launcher (NXRT_LAUNCHER_DIR, set by the launcher), in the run folder, the older
// upscalers\fsr3\, the executable's folder. A setting whose folder lacks the files is logged and the default folders are searched after it.
#pragma once

#include <string>

namespace nxrt::extras {

struct FfxFind {
    std::wstring dll;       // the DLL to load; empty: not found
    std::string version;    // "v2.4.0" or "v1.1.4" (what the files are), empty when not found
    std::string missing;    // not found: the files missing and where they were looked for (one log line)
};

FfxFind ffx_find_upscaler();   // read once
FfxFind ffx_find_framegen();   // read once
std::string ffx_narrow(const std::wstring& w);

}  // namespace nxrt::extras
