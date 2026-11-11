// SPDX-License-Identifier: GPL-3.0-or-later
// fefw.exe of the release package: built on the player's PC (tools/build/fefw_build.py, bundled Zig) from this file and the six
// libraries lifted out of their own dump. All it does is hand the lifted module tables to the runtime in fefw_rt.dll (fefw_rt_api.h).
#include <stdint.h>
#include "fefw_rt_api.h"

// The processor features this build of the game code needs: fefw_build.py passes its variant's set (-DFEFW_CPU_NEEDS=...).
#ifndef FEFW_CPU_NEEDS
#define FEFW_CPU_NEEDS FEFW_CPU_NEEDS_FMA_BUILD
#endif

extern "C" {
extern const struct NxModule nxmod_main, nxmod_sdk, nxmod_subsdk0, nxmod_subsdk1, nxmod_subsdk2, nxmod_rtld;
extern const uint32_t nxmod_main_abi, nxmod_sdk_abi, nxmod_subsdk0_abi, nxmod_subsdk1_abi, nxmod_subsdk2_abi, nxmod_rtld_abi;
}

// The manifest (res/fefw.manifest) sets the UTF-8 code page, so argv is UTF-8 as the runtime expects.
int main(int argc, char** argv) {
    static const FefwRtModules mods = {
        sizeof(FefwRtModules), FEFW_RT_API,
        &nxmod_rtld, &nxmod_main, &nxmod_sdk, &nxmod_subsdk0, &nxmod_subsdk1, &nxmod_subsdk2,
        &nxmod_rtld_abi, &nxmod_main_abi, &nxmod_sdk_abi, &nxmod_subsdk0_abi, &nxmod_subsdk1_abi, &nxmod_subsdk2_abi,
        FEFW_CPU_NEEDS,
    };
    return fefw_rt_main(argc, argv, &mods);
}
