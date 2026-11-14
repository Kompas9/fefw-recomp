# tools/textures: ASTC to BC7 transcoder

C++20 library plus CLI (`astc2bc7`) that turns ASTC LDR / sRGB textures (all 14 footprints, 4x4 to 12x12) into BC7, once,
at setup time, so the renderer never keeps 4 to 9 times larger RGBA8 copies. BCn input is copied through untouched.

Pipeline: ASTC to RGBA8 with ARM astcenc 5.7.1, then BC7 with `bc7e` (from bc7enc_rdo). Both stages are
multithreaded (decode in bands of block rows, one astcenc context per thread; encode in 64-block chunks).

Two builds of the same library (CMake `A2B_BC7E`, `scripts/build.ps1 [-Ispc]`):

- `cpp` (default): bc7e translated to C++ by `scripts/port_bc7e.py` (`src/bc7e_cpp.cpp`, generated: the ISPC qualifiers dropped, the
  block loop and `cif`/`cfor` made plain C++, ISPC's 64-bit `uint16_t` and float constants kept). It is compiled twice, for baseline
  x86-64 and for AVX2 + FMA, and `src/bc7e_cpp_dispatch.cpp` picks one by CPUID (the AVX2 one is about 1.35 times as fast, the same
  quality; `A2B_BC7E_SCALAR=1` forces the other). astcenc's
  decoder is compiled into the library (SSE2, `A2B_ASTC_STATIC`): no DLLs. No ISPC compiler is involved. The runtime builds exactly
  this from source when no ispc build exists (`cmake/a2b_cpp.cmake`, included by `runtime/cmake/gfx.cmake`).
- `ispc`: bc7e.ispc through the ISPC compiler (sse4, avx2 and avx512 paths with its own dispatch); astcenc decode-only as three DLLs
  (`astcdec-avx2/sse4.1/sse2-shared.dll`), the one the CPU supports loaded at run time. The same presets and quality, an estimated
  3 to 4 times faster encode.

`a2b_bc7_encoder()` says which one a program has; the runtime starts the C++ build at a cheaper preset (`[nvn] bc7_quality` 1, no
background re-encode; `runtime/docs/CONFIG.md`).

## Behaviour that matters

- sRGB: ASTC sRGB data is decoded with astcenc's sRGB profile (RGB stays sRGB-encoded, alpha linear), BC7 is encoded on those 8-bit values, and the output is tagged `BC7_UNORM_SRGB` (DXGI 99). Linear ASTC gives `BC7_UNORM` (98). The `.astc` file format does not store the profile, so the CLI takes `--srgb`.
- Alpha: opaque source blocks keep alpha exactly 255. Left alone, bc7e stores opaque mode-6 alpha in the shared p-bit (254 half the time) and uses mode 4/5 channel rotation without pinning the swapped-in original alpha (244..253 on normal maps; `docs/debug/002.md`). The first version of this library switched mode 6 and rotation off for opaque blocks, which cost about 2 dB on albedo and 2 to 12 dB on two-channel (RG, B = 0) normal maps, where rotation is what gives the data channel in the alpha slot its own index set. Now (`allow_opaque_mode6 = 0`, the default) opaque blocks are encoded with mode 6 off (pinning it costs more RGB PSNR than the mode gains), with the preset's channel rotations (about +45% encode time at quality 3), and every opaque block is then fixed up exactly: the slot that holds the original alpha gets endpoints that decode to 255 for any index, which touches no data channel. `--allow-opaque-mode6` gives the raw bc7e output (alpha may drift); `--no-opaque-mode6` the first version.
- Partial blocks: image sizes that are not multiples of 4 (or of the ASTC footprint) are handled; edge pixels are replicated into partial BC7 blocks.
- Passthrough: DDS files with BC1 to BC7 (DX10 or legacy FourCC headers) are copied byte for byte, same format.
- Presets: `--quality 0..6` = bc7e ultrafast, veryfast, fast, basic, slow, veryslow, slowest; default 5 (veryslow). Chosen from the sweep below: 5 has the best mean PSNR per unit time; 6 is 2x slower for no mean gain.
- Not handled: ASTC HDR, 3D ASTC, texture arrays and mip chains inside one call (call once per level), non-Windows hosts (`LoadLibrary`).

## Build and use

```powershell
powershell -File tools\textures\scripts\fetch_third_party.ps1     # astc-encoder 5.7.1, bc7enc_rdo, ISPC 1.32.0 into third_party\
pwsh -File tools\textures\scripts\build.ps1                       # C++ build: CMake + Ninja + clang-cl (runtime\scripts\env.cmd) -> build\release\astc2bc7.exe
pwsh -File tools\textures\scripts\build.ps1 -Ispc                 # ISPC build -> build\release\astc2bc7.exe + 3 DLLs (+ bc7e_ispc.lib)
build\release\astc2bc7.exe in.astc out.dds --srgb [--quality 5] [--threads N] [--dump-rgba f.rgba] [--bench N]
build\release\astc2bc7.exe blocks.bin out.dds --raw 1024x1024:6x6 [--raw-out]     # bare ASTC blocks
python -I tools\textures\scripts\port_bc7e.py                     # regenerate src\bc7e_cpp.{h,cpp} after bc7e.ispc changes
```

### The C++ build, measured (Ryzen 5 7600X, 2026-11-03, other builds running alongside)

On 22 ASTC 8x8 textures of Fire Emblem: Fortune's Weave (level 0, 64 to 2048 texels, albedo, normal, mask, AO) and on the Khronos
images of astc-encoder's test set encoded at 4x4, 6x6, 8x8 and 12x12 (40 cases): the library's banded, threaded decode equals one
astcenc decode of the whole image byte for byte in every case, and no opaque texel loses alpha 255. PSNR RGB of the BC7 against the
ASTC decode, mean (min):

| preset | game 8x8 (22) | Khronos (40) | 8 largest game textures: MPix/s, 10 threads | MPix per CPU second |
|---|---|---|---|---|
| 0 ultrafast | 58.1 (46.2) | 45.9 (37.6) | 113 | 13.0 |
| 1 veryfast | 61.2 (46.5) | 50.9 (42.8) | 23.7 | 2.39 |
| 2 fast | 61.3 (46.5) | | 17.5 | 1.83 |
| 3 basic | 62.3 (47.0) | 52.4 (45.4) | 8.0 | 0.84 |
| 4 slow | 62.3 (46.7) | | 9.4 | 1.01 |
| 5 veryslow | 63.1 (47.4) | | 2.5 | 0.26 |
| 6 slowest | | | 1.7 | 0.18 |

bc7enc_rdo's scalar `bc7enc` (modes 1/6 opaque, 5/6/7 alpha) at uber 0 is faster (39 MPix/s) at preset-1 quality on the game set
(60.8 dB) but leaves alpha 254 on opaque textures and loses 2 dB on the worst Khronos case, so it is not used.

Library API: `include/astc2bc7.h` (`a2b_transcode`, `a2b_decode_astc_rgba8`, `a2b_encode_bc7`, `a2b_output_size`, C linkage).

## Stand-in corpus and oracle

Corpus (not committed, under `work\corpus\`): 16 CC0 1K images from ambientCG (albedo, normal, gray, alpha, high-frequency noise;
`textures_src\MANIFEST.tsv`), encoded with astcenc 5.7.1 `-medium` by `scripts/make_astc_corpus.py` at all 14 block sizes, plus
odd-size crops (1000x750, 333x257, 31x17) at 4x4, 6x6, 8x8 and 12x12: 260 `.astc` files, with astcenc's own decode as ground truth (`ref_png`).

```powershell
pwsh -File tools\textures\oracle\build.ps1                          # bc7dec.exe, the independent BC7 decoder (bc7enc_rdo's reference decoder)
tools\.venv\Scripts\python.exe tools\textures\oracle\run_oracle.py --out work\corpus\textures_oracle_run [--sweep]
```

Checks per file: (1) our ASTC decode equals astcenc's CLI output byte for byte; (2) PSNR and SSIM of the BC7 result (decoded by `bc7dec`, not by our code) against the ASTC decode;
(3) DDS format tag, size and payload length; (4) alpha stays 255 on opaque sources. Plus BCn passthrough (BC1, BC3, BC4, BC5, BC7, odd sizes) bit identical, and the corpus mean.

**Thresholds and why**: PSNR RGB and RGBA at least 33 dB per file, SSIM (luma) at least 0.97 per file, mean PSNR RGB at least 48 dB.
They sit 1.6 dB, 0.004 and 3.9 dB under the measured worst case or mean, so a real regression trips them and noise does not. 33 dB is far above what a broken encoder gives (under 25 dB)
and below the roughly 40 dB usually called visually lossless, because the worst files are the noisiest 1024x1024 normal map at 4x4 to 5x5 (34.65 dB) and a 31x17 alpha crop.
The reference is already lossy: BC7 is measured against what the ASTC decoder produces, not against the original.

**Results** (i9-12900K, 16 threads, preset 5, 260 files, 113 s): ORACLE PASS. Decode identity 260 of 260 (0 differing bytes). Alpha 260 of 260. Passthrough 11 of 11.
PSNR RGB min 34.96, mean 53.81 dB (by kind albedo 50.8, normal 62.1, gray 60.2, noise 48.0, alpha 46.7; the first-version converter gave min 34.65, mean 51.95, normal 55.1). SSIM luma min 0.9854, mean 0.997.

**Throughput** (preset 5, 1024x1024 sRGB 6x6, in memory, 16 threads): decode 117 MPix/s, BC7 encode 6.5 MPix/s, total 5.9 MPix/s (0.8 MPix/s on one thread, 7 to 9x scaling on 16 cores).
Cold CLI process including file IO: 3.8 to 4.7 MPix/s. A 600 MPix texture set would take about two minutes.

Preset sweep (56 files, 16 threads; encode MPix/s measured on one 10x10 sRGB file):

| preset | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| mean PSNR RGB (dB) | 46.82 | 49.77 | 49.91 | 50.72 | 50.59 | 51.68 | 51.65 |
| min PSNR RGB (dB) | 28.88 | 33.47 | 32.65 | 33.70 | 34.08 | 34.65 | 34.94 |
| encode MPix/s | 395 | 67 | 46 | 31 | 26 | 5.8 | 3.0 |

SSIM is computed on luma of the RGB channels, also for normal maps, where it is only a rough guide.

## Provenance and licences

| Component | Source | Licence |
|---|---|---|
| astcenc 5.7.1 (decoder DLLs, header) | github.com/ARM-software/astc-encoder, tag 5.7.1 | Apache-2.0 (`licenses/LICENSE.astc-encoder.Apache-2.0`) |
| `bc7e.ispc` | github.com/richgel999/bc7enc_rdo `b9438627`, Binomial LLC | Apache-2.0 (`licenses/LICENSE.bc7enc_rdo`) |
| `src/bc7e_cpp.h`, `src/bc7e_cpp.cpp` | `bc7e.ispc` above, translated to C++ by `scripts/port_bc7e.py` (a modified version; the files say so) | Apache-2.0 (`licenses/LICENSE.bc7enc_rdo`) |
| `bc7decomp_ref.cpp` (oracle's `bc7dec` only) | same repo | MIT or public domain, same LICENSE |
| ISPC 1.32.0 (build tool, not shipped) | github.com/ispc/ispc | BSD-3-Clause |
| everything else here | written for this project | GPL-3.0-or-later |

All are GPL-3.0 compatible (Apache-2.0 is one-way compatible with GPLv3). Pinned versions and hashes: `THIRD_PARTY.md`.
