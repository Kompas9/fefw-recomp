# Third-party versions

Fetched into this folder (git-ignored by `tools/textures/.gitignore`). Nothing here is committed.

## astc-encoder (ARM Limited)

- URL: https://github.com/ARM-software/astc-encoder.git
- Folder: `third_party/astc-encoder`
- Tag: `5.7.1`, commit `0affb51aaf0e0de3a8b038fa32487d59754a1eed` (2026-11-03, "Add changelog entry for 5.7.1 release (#685)")
- Licence: `third_party/astc-encoder/LICENSE.txt`, SPDX `Apache-2.0`. Core sources carry `SPDX-License-Identifier: Apache-2.0`.
- Bundled third-party code under `Source/ThirdParty/`: `exr_reader.hh` and `streamreader.hh` are `BSD-3-Clause` (Syoyo Fujita, 2025). The rest (stb_image, tinyexr, wuffs) are in the same folder and were not checked file by file.

## bc7enc_rdo (Richard Geldreich, Jr. / Binomial LLC)

- URL: https://github.com/richgel999/bc7enc_rdo.git
- Folder: `third_party/bc7enc_rdo`
- Branch: `master` (default HEAD), commit `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` (2026-07-30, "Update README.md")
- Licence file: `third_party/bc7enc_rdo/LICENSE`
- `bc7e.ispc`: Apache-2.0 (per LICENSE: "bc7e.ispc uses the Apache 2.0 license and is Copyright (C) 2018-2021 Binomial LLC"). The file header says "Copyright (C) 2018-2020 Binomial LLC ... Apache 2.0 license - see LICENSE." The years differ between LICENSE and the header.
- Everything else in the repo: LICENSE says "All other source code files in this repo are available under 2 licenses -- choose whichever you prefer": Alternative A MIT (Copyright 2020-2021 Richard Geldreich, Jr.) or Alternative B public domain (Unlicense wording). `bc7enc.cpp` header: "MIT license or public domain".
- `lodepng.cpp` / `lodepng.h`: LodePNG, Copyright 2005-2016 Lode Vandevenne, zlib-style licence text included in LICENSE.
- The repo also ships a prebuilt `ispc.exe` (third_party/bc7enc_rdo/ispc.exe). It is not used; the official ISPC below is used instead.
- `src/bc7e_cpp.h` and `src/bc7e_cpp.cpp` (in the tree, generated) are `bc7e.ispc` of this commit translated to C++ by
  `scripts/port_bc7e.py`: a modified version of an Apache-2.0 file, so they carry its copyright line and say that they were changed. The
  default (`cpp`) build of the library uses them; neither ISPC compiler is run for it.
- LICENSE contains no SPDX identifiers, so the SPDX names above are read from the text.

## ISPC compiler (Intel Corporation)

- URL: https://github.com/ispc/ispc/releases/download/v1.32.0/ispc-v1.32.0-windows.zip
- Tag: `v1.32.0`, build commit 2b17b0ef24032610 (per `ispc --version`, LLVM 23.1.2)
- ZIP SHA-256: `148D5CD96ABE7501C730D5FBD00F1F0C893B96BFEADA38D1811C9B6251E86A5C`
- ZIP kept at: `build/downloads/ispc-v1.32.0/ispc-v1.32.0-windows.zip` (not in third_party)
- Extracted to: `third_party/ispc/ispc-v1.32.0-windows/` (executable: `third_party/ispc/ispc-v1.32.0-windows/bin/ispc.exe`)
- Licence: `third_party/ispc/ispc-v1.32.0-windows/LICENSE.txt`, SPDX `BSD-3-Clause` (Copyright Intel Corporation). Third-party notices: `third_party/ispc/ispc-v1.32.0-windows/third-party-programs.txt`.
- ispc.exe is not on PATH; always called by full path.

## Local toolchain used (not vendored)

- clang-cl 23.1.4 (`C:\Program Files\LLVM\bin\clang-cl.exe`)
- CMake 4.4.4 (`C:\Program Files\CMake\bin\cmake.exe`)
- Ninja 1.13.2 (`%LOCALAPPDATA%\Microsoft\WinGet\Links\ninja.exe`)
- VS Build Tools 2026 (18.11.1) for MSVC headers/libs, via `vcvars64.bat`
