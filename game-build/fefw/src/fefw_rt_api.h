// SPDX-License-Identifier: GPL-3.0-or-later
// The one call between fefw.exe (linked on the player's PC: the lifted game libraries + fefw_stub.cpp) and fefw_rt.dll (the runtime,
// shipped prebuilt with no game code). Plain C, so the two sides may come from different compilers (zig/clang mingw for the exe,
// clang-cl for the DLL). Everything else the lifted code calls (recomp ctx.h: call_indirect, host_import, guest_svc, ...) is exported by
// fefw_rt.dll as well (src/fefw_rt.def).
#pragma once
#include <stdint.h>

#define FEFW_RT_API 1u     // bump when this struct or the exported set changes (a field added at the end is told apart by size)

// The processor features a build of the game code needs (tools/build/fefw_build.py CPU_VARIANTS): the stub passes its build's set
// in cpu_needs, fefw_rt.dll checks it before the first game instruction (fefw_cpu.h).
#define FEFW_CPU_CX16   0x01u   // CMPXCHG16B: the guest's 128-bit exclusive pairs (every build)
#define FEFW_CPU_SSE42  0x02u   // SSE3, SSSE3, SSE4.1 and SSE4.2
#define FEFW_CPU_POPCNT 0x04u
#define FEFW_CPU_AVX    0x08u   // with the YMM state enabled by the operating system
#define FEFW_CPU_FMA    0x10u   // FMA3
#define FEFW_CPU_NEEDS_FMA_BUILD    (FEFW_CPU_CX16 | FEFW_CPU_SSE42 | FEFW_CPU_AVX | FEFW_CPU_FMA)   // -mcpu=x86_64+cx16+fma
#define FEFW_CPU_NEEDS_COMPAT_BUILD (FEFW_CPU_CX16 | FEFW_CPU_SSE42 | FEFW_CPU_POPCNT)               // -mcpu=x86_64+cx16+sse4_2+popcnt

#ifdef __cplusplus
extern "C" {
#endif
struct NxModule;
typedef struct FefwRtModules {
    uint32_t size;         // sizeof(FefwRtModules)
    uint32_t api;          // FEFW_RT_API
    const struct NxModule *rtld, *main, *sdk, *subsdk0, *subsdk1, *subsdk2;
    const uint32_t *rtld_abi, *main_abi, *sdk_abi, *subsdk0_abi, *subsdk1_abi, *subsdk2_abi;
    uint32_t cpu_needs;    // FEFW_CPU_* of the build (FEFW_CPU_NEEDS_FMA_BUILD when an older stub left it out)
} FefwRtModules;

int fefw_rt_main(int argc, char** argv, const FefwRtModules* mods);
#ifdef __cplusplus
}
#endif
