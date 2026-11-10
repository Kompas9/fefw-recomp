# Third-party notices

The nxrt files below name SuperMarioWonderRecomp as a source in their header comments, and the provenance table in README.md lists them: include/nxrt/diag.h, include/nxrt/dispatch.h, include/nxrt/ipc.h, include/nxrt/kernel.h, include/nxrt/memory.h, include/nxrt/module.h, include/nxrt/svc.h, src/core/dispatch.cpp, src/diag/memguard.cpp, src/diag/watchdog.cpp, src/ipc/ipc.cpp, src/ipc/sm.cpp, src/kernel/handles.cpp, src/kernel/kernel_sync.cpp, src/kernel/svc.cpp, src/kernel/svc_info.cpp, src/kernel/svc_memory.cpp, src/kernel/svc_thread.cpp, src/kernel/thread.cpp, src/kernel/time.cpp, src/loader/module.cpp.

## SuperMarioWonderRecomp (MIT)

Source: `refs/SuperMarioWonderRecomp/LICENSE`, reproduced verbatim:

```text
MIT License

Copyright (c) 2026 BelmanteGu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## smo-recomp, ScarletNX and ssbu-recomp (GPL-3.0)

`src/media/movie_mf.cpp` (the `movie::` library on Media Foundation) follows the design of smo-recomp `runtime/services/nnmovie_mf.c` (GPL-3.0): the object layouts of the library, the decoder's two buffer pools, and the token that carries a decoded sample from the extractor to a decoder. The code is rewritten for nxrt (C++, a decode-ahead worker, edit lists, the plain exported wrappers instead of virtual calls).

The nxrt parts that come from smo-recomp and ScarletNX are under GPL-3.0, like nxrt itself (GPL-3.0-or-later). The same holds for the ssbu-recomp references in `include/nxrt/diag.h`, `src/eh/*` (facts read from `runtime/eh_runtime.h` and `runtime/nncxxrt.c`: header layout of the SDK's libc++abi, the non-unique-name bit) and `src/core/hooks.cpp` (idea only). Upstream licence files: smo-recomp `LICENSE` (GPL-3.0), ScarletNX `LICENSE` (GPL-3.0; its source headers say GPL-3.0-or-later), ssbu-recomp `LICENSE` (GPL-3.0). The GPL-3.0 text for nxrt is the `LICENSE` file at the repository root.

## LLVM libc++abi (Apache-2.0 with LLVM exceptions)

`src/eh/typeinfo.cpp`, `src/eh/unwind.cpp`, `include/nxrt/eh.h` and `tests/lift/guest/rt2.h` follow the behaviour and data layouts of LLVM libc++abi (`libcxxabi/src/private_typeinfo.cpp`, `cxa_exception.h`, `cxa_exception.cpp`, `cxa_personality.cpp`), read from the llvm-project repository on 2026-10-21. The code was written from that behaviour and from the Itanium C++ ABI; no libc++abi source text was copied. Licence of the upstream files: Apache License v2.0 with LLVM Exceptions (https://llvm.org/LICENSE.txt), which is compatible with GPL-3.0-or-later for this use. The licence text in the release is `licenses/LICENSE.libcxxabi.Apache-2.0-LLVM-exception.txt` (source: `tools/release/licenses/`).

## recomp `oracle/e2e/rt/eh_rt.cpp` (GPL-3.0-or-later, S1)

`src/eh/unwind.cpp` and `src/eh/catch_call.cpp` rework the table walk and landing protocol of this test-only runtime (S1, same project, same licence). `tests/tools/guest_build.py` and `tests/tools/unicorn_expect.py` import S1's `build_prog.py` and `unicorn_side.py` from `../recomp/oracle/e2e` at build time without copying them.

## libnx (ISC)

`tests/tools/gen_hipc_vectors.py` re-implements the message packing of libnx `sf/hipc.h` and `sf/cmif.h` to produce independent test vectors. libnx is distributed under this licence:

```
Copyright 2017-2018 libnx Authors

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

## Graphics shell (S4b)

The graphics shell (`src/gfx`, `cmake/gfx*.cmake`) downloads the components below at configure time, each pinned by URL and SHA256 (see README.md, "Third-party code fetched at configure time"). The generated NVN tables take names, argument classes and object sizes from the headers named at the end of this section.

### SDL3 (zlib)

Source: SDL 3.4.18, `LICENSE.txt`:

```text
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### volk (MIT)

Copyright (c) 2018-2026 Arseny Kapoulkine. MIT licence, same text as the Wonder notice above.

### VulkanMemoryAllocator (MIT)

Copyright (c) 2017-2026 Advanced Micro Devices, Inc. All rights reserved. MIT licence, same text as the Wonder notice above.

### Zstandard 1.5.7 (BSD-3-Clause; dual-licensed BSD or GPL-2.0, used under BSD)

The library sources (lib/common, lib/compress, lib/decompress), fetched pinned by cmake/gfx_deps.cmake, compress and read the
shader preload pack (src/gfx/nvn/nvn_shaders.cpp).

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Dear ImGui (MIT)

Copyright (c) 2014-2026 Omar Cornut. MIT licence, same text as the Wonder notice above.

### Vulkan-Headers (Apache-2.0 OR MIT)

Copyright 2015-2026 The Khronos Group Inc. Files fall under Apache-2.0 or MIT (SPDX `Apache-2.0 OR MIT`); nxrt uses them under MIT.

### NVN interface facts

`src/gfx/nvn/nvn_table.inc` and `nvn_sizes.inc` list NVN function names, argument classes and object sizes. They were read from `nvn.h` of the mp1r-practice-mod repository (MIT notice in the file: "Copyright (c) 2020 h1k421"; the repository as a whole is GPL-2.0) and checked against smo-recomp, ssbu-recomp (both GPL-3.0), SuperMarioWonderRecomp (MIT, Copyright (c) 2026 BelmanteGu) and LASRecomp (MIT, Copyright (c) 2026 Peiikai). No source text is copied into the tables.

### NVN header (MIT) and shader cache module

`src/gfx/nvn/third_party/nvn/nvn.h` is the NVN header of the mp1r-practice-mod repository, copied with its MIT notice ("Copyright (c) 2020 h1k421") and a few corrected typedefs (marked in the file). `src/gfx/nvn/nvn_shaders.h` and `nvn_shaders.cpp` are adapted from SuperMarioWonderRecomp `runtime/src/nvn/nvn_shaders.{h,cpp}` (MIT, Copyright (c) 2026 BelmanteGu; MIT text as above), which in turn follows Ryujinx's `SupportBuffer.cs` (MIT, Copyright Ryujinx Team and Contributors). The translator patches in `tools/shaders` are Ryujinx source in context lines (MIT).

### Tools linked at build time

`nxrt_gfx` links `tools/textures` (astc2bc7, GPL-3.0-or-later; it bundles ARM astcenc 5.7.0, Apache-2.0, loaded as decoder DLLs next to the executable, and bc7e from bc7enc_rdo, Apache-2.0) and `tools/tiling` (GPL-3.0-or-later). Their notices are in those folders.
## libvpx 1.16.0 (BSD-3-Clause; x86inc assembly macros ISC)

Linked statically into Switch 2 builds for the host nn::codec VP9/VP8 decoders (src/media/video_codec.cpp, cmake/media_deps.cmake), built
by vcpkg (`vcpkg install libvpx:x64-windows-static`, port libvpx 1.16.0#3). Upstream: https://chromium.googlesource.com/webm/libvpx.
Licence text as installed by vcpkg (`share/libvpx/copyright`), reproduced verbatim:

```text
The first LICENSE below is the libvpx license (BSD-3-Clause).
The second LICENSE below is the license of the bundled third_party/x86inc assembly macros (ISC), which are assembled into libvpx on x86 and x64 targets.


LICENSE:

Copyright (c) 2010, The WebM Project authors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google, nor the WebM Project, nor the names
    of its contributors may be used to endorse or promote products
    derived from this software without specific prior written
    permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.



LICENSE:

Copyright (C) 2005-2012 x264 project

Authors: Loren Merritt <lorenm@u.washington.edu>
         Anton Mitrofanov <BugMaster@narod.ru>
         Jason Garrett-Glaser <darkshikari@gmail.com>
         Henrik Gramner <hengar-6@student.ltu.se>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

## Ryujinx and ryubing (MIT)

The service files below adapt code, tables or layouts from Ryujinx and its fork ryubing, both under the MIT licence (their headers name the source files): `src/services/acc.cpp`, `am.cpp`, `audio_out.cpp`, `hid.cpp`, `logfault.cpp`, `misc.cpp`, `network.cpp`, `pl.cpp`, `power.cpp`, `settings.cpp`, `time.cpp`, `timezone.cpp` and `vi_nv.cpp`. Licence text, from `refs/ryubing/LICENSE.txt`:

```text
MIT License

Copyright (c) Ryujinx Team and Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## yuzu and Eden (GPL-2.0-or-later)

`src/services/fs_romfs.cpp` adapts yuzu's `src/core/file_sys/fsmitm_romfsbuild.cpp`. `src/services/audio_out.cpp` and `src/services/vi_nv.cpp` take layouts from Eden and yuzu, which ScarletNX also ports (ScarletNX is covered above). `src/services/hid_shm.h` follows the Eden HID layout as cross-checked through ScarletNX; its offsets are Ryujinx's (see the Ryujinx section). The upstream licence is GPL-2.0-or-later; Eden's copies of the same files are marked GPL-3.0-or-later. nxrt is GPL-3.0-or-later, and GPL-2.0-or-later code may be used under GPL-3.0. The GPL-2.0 text, with a header naming these files, is `licenses/LICENSE.yuzu-Eden.GPL-2.0.txt` in the release (source: `tools/release/licenses/LICENSE.yuzu-Eden.GPL-2.0.txt`).

## DejaVu fonts (Bitstream Vera licence, reference only)

`src/services/pl.cpp` uses DejaVu Sans (`DejaVuSans.ttf`) as the fallback shared font. The font file is not in this worktree. Its licence text is `assets/fonts/LICENSE-DejaVu.txt` (in the `svc-pl` worktree, with the font at `assets/fonts/DejaVuSans.ttf`), the Bitstream Vera licence as the DejaVu fonts name it. Not copied here.

## AMD FidelityFX SDK v1.1.4, FSR 1 shader headers (MIT)

Files: `third_party/ffx_fsr1/` (`fsr1/ffx_fsr1.h`, `ffx_core*.h`, `ffx_common_types.h` and the other headers listed in `third_party/ffx_fsr1/README.txt`). Copyright (C) 2024 Advanced Micro Devices, Inc. Licence text: `third_party/ffx_fsr1/LICENSE.txt`. Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v1.1.4. Used by `src/gfx/extras/`: the EASU and RCAS compute shaders are generated from these headers by `scripts/gen_extras_shaders.py`, and the constant setup is a port in `src/gfx/extras/fsr1_consts.cpp`.

## NVIDIA Image Scaling SDK v1.0.3 (MIT)

Files: `third_party/nvidia_nis/` (`NIS_Scaler.h`, `NIS_Config.h`, unmodified). Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. Licence text: `third_party/nvidia_nis/LICENSE.txt`. Source: https://github.com/NVIDIAGameWorks/NVIDIAImageScaling, tag v1.0.3. Used by `src/gfx/extras/nis_spatial.cpp` (the constant setup, `NVScalerUpdateConfig`) and by the compute shader in `src/gfx/extras/nis_shaders.inc`, generated from these headers by `scripts/gen_nis_shaders.py`.

## AMD FidelityFX SDK v2.4.0, FidelityFX API headers (MIT)

Files: `third_party/amd_ffx_api/` (`api/include/ffx_api.h`, `ffx_api_types.h`, `ffx_api_loader.h`, `vk/ffx_api_vk.h`, `upscalers/include/ffx_upscale.h`, unmodified). Copyright (c) Advanced Micro Devices, Inc. Licence text: `third_party/amd_ffx_api/LICENSE.txt`. Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, release v2.4.0. Used by `src/gfx/extras/temporal_ffx.cpp`.

## AMD FidelityFX / FSR runtime DLLs (MIT)

`amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` (AMD-signed, `Kits/FidelityFX/signedbin/` of the FSR SDK v2.4.0,
whose `Kits/FidelityFX/docs/license.md` lists them under MIT) and `amd_fidelityfx_vk.dll` (`PrebuiltSignedDLL/` of the FidelityFX SDK
v1.1.4, MIT) are not in this repository or the release package. The game loads them at run time only (`LoadLibrary`,
`src/gfx/extras/ffx_files.cpp`) if the player puts them in `upscalers/fsr/` or names the folder with `[upscale] fsr_dll`.
Copyright (C) Advanced Micro Devices, Inc.

## NVIDIA Streamline SDK v2.14.16 headers (MIT)

Files: `third_party/nvidia_streamline/include/` (unmodified). Copyright (c) NVIDIA CORPORATION. Licence text: `third_party/nvidia_streamline/license.txt`. Source: https://github.com/NVIDIA-RTX/Streamline, release v2.14.16. Used by `src/gfx/extras/temporal_dlss.cpp`; nothing from Streamline is linked.

## NVIDIA Streamline runtime DLLs (MIT)

`sl.interposer.dll`, `sl.common.dll` and `sl.dlss.dll` (NVIDIA-signed, from `bin/x64` of the Streamline SDK v2.14.16 release, https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16; MIT, `licenses/LICENSE.Streamline.MIT.txt`, third-party notices in `licenses/NOTICE.Streamline-3rd-party.md`) are not in this repository and this project does not distribute them. The game loads them at run time only, if you install them yourself. Copyright (c) NVIDIA CORPORATION.

## NVIDIA DLSS (nvngx_dlss.dll, not shipped) and Intel XeSS (not shipped)

`nvngx_dlss.dll` (NVIDIA DLSS Super Resolution 310.9.16) is under the NVIDIA RTX SDKs LICENSE (v. March 14, 2024; https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt, a copy is `licenses/LICENSE.NVIDIA-RTX-SDKs.txt` in the release). That licence requires redistributors to pass on terms at least as protective as its own and forbids any use that subjects the SDK to a licence requiring it to be redistributable at no charge (section 4e), which does not fit a GPL-3.0 package, so this repository does not contain the file and this project neither distributes nor downloads it. `libxess.dll` (Intel XeSS) is never shipped either; `xess_dll` is read but not used (see `docs/CONFIG.md`). The DLSS SDK itself (NGX static library) is not used.

## dav1d 1.5.1 (BSD-2-Clause)

`third_party/dav1d`: the AV1 decoder of the Switch 2 movie player (`src/media/movie_vdec.cpp`), 8-bit, built from source. The C
sources come from nxnative-penis6's vendored copy (`dm/third_party/dav1d`, MIT repository, dav1d itself BSD-2-Clause) without its arm64
assembly; the x86-64 assembly (`src/x86`, `src/ext/x86`) is upstream 1.5.1's and is built when NASM is found (`src/ext/x86/x86inc.asm`
is under the ISC licence stated in its header); `config/config.h` is written for Windows x64 and `src/win32/thread.c` is upstream's. Upstream: https://code.videolan.org/videolan/dav1d.
Licence: `third_party/dav1d/COPYING` (Copyright © 2018-2019, VideoLAN and dav1d authors; BSD-2-Clause).

## nxnative-penis6 (MIT)

`src/media/movie_vdec.cpp` (the Switch 2 movie decoder front end) is ported from nxnative-penis6 `dm/host/hle_nx2_movie.c`, and the dav1d
copy in `third_party/dav1d` comes from its `dm/third_party/dav1d`. Several comments cite its commits for hardware facts (NVN2 sync scope,
display modes, shader forms); those facts are re-implemented here, not copied. Nothing is taken from its `nx2_lower` (GPL-3.0-or-later),
which would in any case be compatible with this project's GPL-3.0 licence. Source: https://git.eden-emu.dev/penis6/nxnative-penis6.

```
MIT License

Copyright (c) 2026 seedds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Kompas9's Switch-Game-Patches, Fortune's Weave ultrawide patch (MIT)

`games/fefw/fefw_wide.cpp` (`[fefw] ultrawide`) and its sites in `recomp/games/fefw/main.sites` follow Kompas9's "Ultrawide 3440x1440 v1.1"
patch for this game build (`games/Fire-Emblem-Fortunes-Weave`: `ULTRAWIDE.md`, `ultrawide-patch-records.json`): which code computes the
perspective camera's aspect, the 1920x1080 UI projections, the scissor pane, the world-to-screen markers, the 2D effects' projection and
scissors, and the movie viewport, and what each change does there. The runtime re-implements those findings as host hooks that compute their
values from the output's aspect; the IPS patch and its code caves are neither used nor distributed. Source:
https://github.com/Kompas9/Switch-Game-Patches.

```
MIT License

Copyright (c) 2026 Kompas9

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
