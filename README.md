# fefw-recomp

A native Windows PC port of **Fire Emblem: Fortune's Weave** (Nintendo Switch 2), made by static recompilation. The game's code is
translated to C++ and compiled into a regular Windows program, which runs on a small runtime that stands in for the console's system
software and draws with Vulkan. It is not an emulator.

> [!IMPORTANT]
> **No game files are included or downloadable here.** This project contains only its own tools and runtime. To play, you need your
> own legally dumped copy of the game; everything that reads the game happens on your PC. See [LEGAL.md](LEGAL.md).
>
> Not affiliated with or endorsed by Nintendo or Intelligent Systems.

## Features

- Runs natively on Windows, with no emulator in between
- 60 or 120 FPS modes, any output resolution, render scale and a temporal upscaler
- Ultrawide (21:9, 32:9 or any wider shape): a wider view with the menus and HUD kept at 16:9 in the centre
- Windowed, borderless or fullscreen
- Keyboard or any controller, with matching button prompts
- An in-game settings overlay (**F1**)
- A launcher that sets everything up from your dump, with nothing else to install

## Requirements

- Windows 10 or 11, 64-bit
- A GPU with Vulkan 1.3 and an up-to-date driver
- 16 GB of RAM and about 30 GB of free disk space
- Your own dump of the game at update **131072**, in one of these forms:
  - a folder with `exefs\` and `romfs\`
  - a dump folder with `base\` and `updates\<version>\`
  - a [`.nup`](https://github.com/a55uka/nup) package
  - an uncompressed `.xci`, imported with the bundled Lichii archive reader and your own keys (see below)

## Getting started

1. Download `fefw-recomp-<version>-win64.zip` from the [Releases](../../releases) page and extract it to its own folder.
2. Run `fefw-launcher.exe`.
3. On **Setup**, choose your dump folder, **.nup** file, or use **.xci** to import a cartridge dump and press **Set up**. The launcher checks your files, builds the game on
   your PC and prepares the shaders. The first setup takes a while; you can **Pause** it and continue later.
4. Press **Play**.

Your saves are in the `save\` folder next to the launcher.

### Optional XCI import

On Setup, press **.xci**, select your cartridge dump and your own `prod.keys`.
The last picker accepts a matching update **NSP**; Cancel skips that optional file.
An XCI without the supported executable needs the supported update supplied separately.
The bundled Lichii archive reader handles extraction automatically; no emulator or
separately installed extractor is required. Keys are never downloaded or bundled.
For rights-managed content, tickets in the NSP are read; an optional `title.keys`
next to the selected `prod.keys` is also used.

The importer checks the executable against the same supported-build list as
folder/NUP setup and produces the full merged RomFS for patch updates.
After validation, press **Use these game files**, then **Set up** as usual.

Each import creates a new `xci_dump/import-...` folder under the game folder
(or `game/xci_dump` in a checkout). Allow space for the full decrypted dump.
The source archives and existing imports are not overwritten. Cancellation/failure
cleans only the current attempt. Keys and ticket-derived keys stay in memory;
no key files are copied into the extracted dump.

DLC, compressed XCZ/NSZ and split XCI are not imported by this action; the existing
folder/NUP flow remains available. For source/release builders, see
[tools/xci/README.md](tools/xci/README.md) for the helper build and corresponding source.

### Settings

The launcher's **Settings** page covers resolution, frame rate, upscaling, window mode, sound and controls. Press **F1** in game for
the overlay. Every option is described in [runtime/docs/CONFIG.md](runtime/docs/CONFIG.md).

### Game language

**Settings > Language** (or **F1 > Game**) picks the language of the game's text and voices: Japanese, English (US or UK), French,
German, Italian, Spanish, Korean and Chinese (Simplified or Traditional). *Automatic*, the default, follows your Windows display
language and uses English when the game has no translation for it. The change applies the next time the game starts.

### DLSS (NVIDIA RTX only)

NVIDIA's files cannot be shipped here, so DLSS stays off until you add them. Take them from NVIDIA's
[Streamline SDK](https://github.com/NVIDIAGameWorks/Streamline) (v2.x, the `bin/x64` folder of the release, not the development one) and
put these four files in a folder named `upscalers\streamline` next to `fefw-launcher.exe`:

```
sl.interposer.dll   sl.common.dll   sl.dlss.dll   nvngx_dlss.dll
```

Then choose **DLSS** (or **Automatic**, which picks DLSS when the files and an RTX GPU are there) as the Upscaler in **Settings**, and
restart the game. `fefw.log` says `Streamline ... started` and `DLSS (Streamline)` when it works. The folder can be somewhere else:
set it under **Settings > Picture > DLSS (Streamline) folder**.

### Keyboard and controller

Any controller works, and so does the keyboard, with no setup. The button prompts in the game follow the one you used last: press a
key and they show keys, touch the controller and they show its buttons. The keyboard drives the Switch buttons the game knows:

| Keys | Switch | |
|---|---|---|
| W A S D | Left stick | Move |
| Arrow keys, I J K L | Right stick | Camera |
| Enter, Space | A | Confirm |
| Esc, Backspace | B | Back |
| R | X | |
| F | Y | |
| Q / E | L / R | E: dash in town |
| Z / C | ZL / ZR | C: map in town |
| Tab | + | Menu |
| M | - | |
| 1 2 3 4, or numpad 8 2 4 6 | D-pad up, down, left, right | |
| V / B | Left / right stick click | |

The movement keys push the stick all the way. **F1** (settings), **F2** (frame rate counter) and **Alt+Enter** (full screen) are
the runtime's own keys. **F1 > Controls** lists the keys as they are now.

To change a key, close the game and edit `[bindings.all]` in `controls.ini` in the run folder (the game creates it on its first
start). Each line is `key = action`:

```
[bindings.all]
W = move_forward
Return = btn_a
G = btn_x
X = none
```

Left side: a key name (`Return`, `Space`, `Left Shift`, `Up`, `Keypad 8` ...). Right side: `move_*`, `look_*`, `dpad_*`, `btn_a`,
`btn_b`, `btn_x`, `btn_y`, `btn_l`, `btn_r`, `btn_zl`, `btn_zr`, `btn_plus`, `btn_minus`, `btn_lstick`, `btn_rstick`, or `none` to
switch a key off. An older `controls.ini` gets the keys above added at the next start (only keys you have not bound yourself; the
old file is kept as `controls.ini.bak`). To go back to the default keys, delete the `[bindings.*]` sections. Controller options (stick dead zones,
button mapping, prompts) are under **Settings** and **F1 > Controls**.

### DLC

DLC is used when you dump it from your own copy, like the game. Put each add-on in `dlc\<title id>\romfs` of your dump folder (next
to `exefs\` and `romfs\`, or next to `base\` and `updates\`). A `.nup` with add-ons is unpacked that way on its own. The **Play**
page lists the DLC it found.

### Mods

RomFS mods go in the `mods\` folder next to the launcher (**Play** > **Open mods folder**), in the layout emulators use:
`mods\<mod name>\romfs\...`. Files in a mod replace the game's files at the same path. If two mods change the same file, the mod
whose name sorts last wins, so you can name them `01-...` and `02-...`. Rename a mod to start with `-` or `_` to turn it off. Code
mods (exefs `.ips` / `.pchtxt` patches) do not work, because the game code is compiled ahead of time.

### Updating to a new release

Extract the new zip to a new folder, copy your `save\` folder (and `mods\`, if you use mods) into it, and run **Set up** again.

## Troubleshooting

- **"These game files are a different version"**: only update 131072 is supported. Check that you dumped the update, not only the
  base game.
- **Setup stopped or failed**: press **Set up** again to continue where it stopped. **Open full log** on the Setup page shows what
  went wrong.
- **The game closes or freezes**: look at `fefw.log` in the launcher's folder, and update your GPU driver.

## Reporting problems

Open an [issue](../../issues) with `fefw.log`, your GPU, driver and Windows version, and the release you used. Please do not attach
game files or anything made from them.

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md). Contributions are welcome.

## Credits

XCI import uses the archive readers from [Lichii](https://github.com/zortos293/Lichii)
(GPL-3.0-or-later, incorporating Eden/yuzu code). The bundled helper includes its
corresponding source, build recipe and third-party notices in `tools/xci`.

This project stands on the work of others. Thank you.

- [nxrecomp](https://github.com/drakolordx7/nxrecomp) by drakolordx7: the recompiler this is built on.
- [pokopia-recomp](https://github.com/drakolordx7/pokopia-recomp) (drakolordx7) and [featureJosh/pokopia-recomp](https://github.com/featureJosh/pokopia-recomp)
  (featureJosh): the runtime, tools and launcher this project started from (GPL-3.0).
- [drakolordx7/switch2-recomp](https://github.com/drakolordx7/switch2-recomp): the launcher design this launcher follows.
- [nxnative-penis6](https://git.eden-emu.dev/penis6/nxnative-penis6) by seedds (MIT): the movie decoder front end, its dav1d copy and
  several hardware facts (notices in `runtime/THIRD_PARTY_NOTICES.md`).
- [Kompas9's Switch-Game-Patches](https://github.com/Kompas9/Switch-Game-Patches) (MIT): the Fortune's Weave ultrawide patch, whose sites and
  findings `[fefw] ultrawide` re-implements as runtime hooks.
- [Ryujinx / Ryubing](https://git.ryujinx.app/ryubing/ryujinx) (MIT): the shader translator and tiling code; [Mesa](https://gitlab.freedesktop.org/mesa/mesa) NAK (MIT).
- SuperMarioWonderRecomp (MIT); smo-recomp, ScarletNX and ssbu-recomp (GPL-3.0); [yuzu and Eden](https://git.eden-emu.dev/eden-emu/eden)
  (GPL); libnx (ISC); LLVM libc++abi (Apache-2.0 with LLVM exceptions).
- [Hookshot](https://github.com/NotRexed/Hookshot) (GPL-3.0) and SkyboundTotk: the RomFS mod folder layout and rules followed here, so
  mods work the same in both.
- [zbic](https://github.com/kinnay/zbic) by Yannik Marchand (GPLv2; installed by pip at setup, not included here): ZBIC decompression.
- [dav1d](https://code.videolan.org/videolan/dav1d) (BSD-2-Clause), [libvpx](https://chromium.googlesource.com/webm/libvpx) (BSD-3-Clause),
  [Zstandard](https://github.com/facebook/zstd) (BSD), [SDL3](https://github.com/libsdl-org/SDL) (zlib), volk, VulkanMemoryAllocator, Dear ImGui (MIT),
  Vulkan-Headers (Apache-2.0 or MIT).
- [astc-encoder](https://github.com/ARM-software/astc-encoder) (Apache-2.0), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo)
  (MIT or public domain), ISPC (BSD-3-Clause).
- AMD FidelityFX SDK headers and FSR (MIT), NVIDIA Image Scaling (MIT), NVIDIA Streamline headers (MIT). No NVIDIA or AMD binaries are
  included; DLSS is not used or distributed.
- [Sascha Willems' Vulkan samples](https://github.com/SaschaWillems/Vulkan) (MIT): the stand-in shader test corpus.
- [Kenney input prompts](https://kenney.nl/assets/input-prompts) (CC0), DejaVu fonts, [Tauri](https://tauri.app), [Lucide](https://lucide.dev) icons (ISC).

Licence texts: `runtime/THIRD_PARTY_NOTICES.md`, `recomp/THIRD_PARTY.md`, `tools/*/licenses/`, `tools/textures/THIRD_PARTY.md` and the
licence files inside `runtime/third_party/`. All of these licences are compatible with the GPL-3.0 this project is released under.

## License

GPL-3.0 ([LICENSE](LICENSE)). Third-party code keeps its own licence.
