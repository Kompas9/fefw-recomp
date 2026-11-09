# Bundled Lichii archive reader

`fefw-archive.exe` is an extraction-only command-line frontend linked against
[Lichii](https://github.com/zortos293/Lichii)'s `core` archive readers. It does not
create a Core::System, execute guest code, create a window, or start a renderer.
The launcher runs it in the background from `tools/xci`.

It reads XCI/NSP containers, imports NSP tickets, resolves a matching base/update
NCA pair, checks the supported NSO build ID and streams ExeFS and merged RomFS
files to the launcher's newly created scratch directory. It does not copy the
whole encrypted secure partition. SHA3/compressed/sparse storage support comes
from Lichii's readers rather than a second decryption implementation here.

## Building for releases

Use an idle, configured Windows MinGW Lichii checkout with its dependencies and
libraries built. The version tested for these archive APIs is upstream
`0ab7736df9dba2b396d2c7a26bd26fbb9b7b0dc0` plus the local Windows build
compatibility changes. The recipe records the exact source commit and packages
its corresponding tracked source, including any locally committed fixes.

```powershell
python tools/xci/build_helper.py --lichii-source C:/src/Lichii --lichii-build C:/src/Lichii/build/release
```

This temporarily adds one CMake target, builds it, restores the CMake hook,
and writes `tools/xci/bin`: the helper, its non-system DLL dependencies,
licences, corresponding source and build metadata. Existing source files are
not patched. Do not run concurrently with another build/configuration of that
Lichii tree. This packaging recipe currently supports MinGW on Windows.

`game-build/scripts/make-release.ps1` copies this payload into the release's
`tools/xci`. `-ArchiveToolsDir` selects a different prepared payload. Release
packaging fails when the helper/source/licences are missing instead of shipping
a nonfunctional XCI button. Source-checkout launchers find `tools/xci/bin`.

## Protocol and local files

The helper takes `--xci`, `--keys`, `--out`, optional `--update` and one or more
`--build-id` arguments. `--out` must be an existing empty scratch directory.
It reports JSON lines (`stage`, `progress`, `complete`, `error`); `--version`
reports protocol 1. Paths are passed as argument values, without a shell, and
Windows wide arguments are converted to UTF-8 to support non-ASCII filenames.

The original keys and archives are read only. A generated build-local copy of
Lichii's key manager adapts only two IO functions: `ReloadKeys` reads the explicitly
selected prod.keys and an optional adjacent title.keys; `WriteKeyToFile` becomes
a no-op. Ticket-derived keys stay in memory and disappear when the helper exits.
No key files are copied into the dump or written to an emulator profile. All
archive decryption, ticket parsing and filesystem implementation remain Lichii's.
`prepare_reader.py` checks the expected function signatures and fails on upstream
layout changes rather than silently using the original profile/key-writing paths.

The launcher controls cancellation by terminating this helper, then clearing
only its own scratch directory. No emulator process is launched or stopped.

## Distribution and attribution

The frontend is GPL-3.0-or-later. Lichii incorporates GPL-2.0-or-later/GPL-3.0-or-later
Eden/yuzu code and third-party components: preserve those original source notices.
`corresponding-source.zip` includes the exact Lichii source and helper build recipe;
`licenses/` includes the Lichii licence and the dynamically linked runtime notices
(OpenSSL, LZ4, GCC runtime exception and winpthreads). No game data, keys, firmware
or generated game code is included in the helper payload.
