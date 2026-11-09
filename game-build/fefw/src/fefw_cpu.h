// SPDX-License-Identifier: GPL-3.0-or-later
// The CPU features the game needs, checked before any game code runs (fefw_main.cpp calls cpu_check() first thing).
//
// What the build requires, and where it comes from:
//   FMA3  the lifted game code is compiled with -mfma (game-build/release/fefw_build.py CXXFLAGS, recomp/games/fefw/CMakeLists.txt): the
//         guest's FMADD / FMLA family is a single vfmadd instruction. Without FMA3 the first one raises an illegal instruction at once
//         (a blank window, then nothing: issues 33 and 5, Xeon E5 v2 = Ivy Bridge).
//   AVX   -mfma implies AVX in LLVM (the compiler may use VEX encodings anywhere in the lifted code), and the operating system must have
//         enabled the YMM state (OSXSAVE and XGETBV).
//   CX16  -mcx16: the guest's 128-bit exclusive load/store pairs are cmpxchg16b.
// Not required: AVX2 and BMI (the runtime and the lifted code are not compiled for them; the texture transcoder's AVX2 build and dav1d's
// assembly are picked by CPUID at run time). fefw_rt.dll itself is baseline x86-64 (SSE2).
//
// Intel: Haswell (2013) and newer. AMD: Piledriver (2012) and newer.
//
// FEFW_CPU_FORCE_MISSING=fma,avx,cx16 (comma list; a test hook) makes the check behave as if those features were absent.
#pragma once

#include <intrin.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace fefw_cpu {

struct Report {
    bool fma = true, avx = true, cx16 = true;       // present (and, for AVX, enabled by the OS)
    bool forced = false;                            // a feature was hidden by FEFW_CPU_FORCE_MISSING
    char brand[64] = {};
    bool ok() const { return fma && avx && cx16; }
};

inline Report probe() {
    Report r;
    int b[4] = {};
    __cpuid(b, 0x80000000);
    if (static_cast<unsigned>(b[0]) >= 0x80000004u) {
        int* p = reinterpret_cast<int*>(r.brand);
        for (unsigned i = 0; i < 3; i++) __cpuid(p + 4 * i, static_cast<int>(0x80000002u + i));
        r.brand[48] = 0;
    }
    __cpuid(b, 0);
    const int max_leaf = b[0];
    if (max_leaf < 1) {
        r.fma = r.avx = r.cx16 = false;
    } else {
        __cpuid(b, 1);
        const unsigned ecx = static_cast<unsigned>(b[2]);
        r.cx16 = (ecx >> 13) & 1;
        const bool osxsave = (ecx >> 27) & 1;
        bool ymm = false;
        if (osxsave) {
            unsigned lo = 0, hi = 0;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            ymm = (lo & 6) == 6;                     // XMM and YMM state enabled by the OS
        }
        r.avx = ((ecx >> 28) & 1) && ymm;
        r.fma = ((ecx >> 12) & 1) && r.avx;          // VEX-encoded FMA needs the YMM state as well
    }
    if (const char* f = std::getenv("FEFW_CPU_FORCE_MISSING"); f && *f) {
        const std::string s = std::string(",") + f + ",";
        if (s.find(",fma,") != std::string::npos) r.fma = false, r.forced = true;
        if (s.find(",avx,") != std::string::npos) r.avx = false, r.forced = true;
        if (s.find(",cx16,") != std::string::npos) r.cx16 = false, r.forced = true;
    }
    return r;
}

// The plain-language message for a failing report ("" when the CPU is fine).
inline std::string message(const Report& r) {
    if (r.ok()) return {};
    std::string missing;
    auto add = [&](const char* s) { missing += missing.empty() ? "" : ", "; missing += s; };
    if (!r.fma) add("FMA3 (fused multiply-add)");
    if (!r.avx) add("AVX");
    if (!r.cx16) add("CMPXCHG16B");
    std::string m = "This PC's processor";
    if (r.brand[0]) {
        std::string b = r.brand;
        b.erase(0, b.find_first_not_of(' '));
        b.erase(b.find_last_not_of(' ') + 1);
        m += " (" + b + ")";
    }
    m += " does not support " + missing + ", which the game needs.\n\n"
         "Fortune's Weave is recompiled for processors with FMA3 and AVX: an Intel Haswell (2013) or newer, or an AMD Piledriver (2012) / Ryzen or newer. "
         "Older processors (Ivy Bridge and Xeon E5 v2, Sandy Bridge, Core 2, AMD FX before Piledriver) cannot run it.";
    if (r.forced) m += "\n\n(FEFW_CPU_FORCE_MISSING is set: the missing feature is simulated.)";
    return m;
}

}  // namespace fefw_cpu
