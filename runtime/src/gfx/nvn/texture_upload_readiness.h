// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace nxrt::nvn::be::gpu {
// Depth uploads are not implemented by tex_ensure_current. Layout dependencies
// remain necessary even when the CPU-dirty bit cannot result in an upload.
constexpr bool pool_preparation_required(bool cpu_dirty, bool depth_image, bool sampled_layout_matches) {
    return !sampled_layout_matches || (cpu_dirty && !depth_image);
}
}
