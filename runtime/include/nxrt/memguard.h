// SPDX-License-Identifier: GPL-3.0-or-later
// Memory guard: a background thread polls the process's private commit (PrivateUsage); past the limit it calls the
// on_exceed callback (default: write the report file, flush, TerminateProcess(3)). The footprint is logged every 30 s; one warning at 75% of the limit.
// The report goes to crash_report_path() (the crash report's file name): reason "memory limit", the footprint, every gauge, the last 30 s of frame times.
// Config: [memory] limit_gb (default 8) / env NXRT_MEMORY_LIMIT_GB; [memory] poll_ms (250).
#pragma once
#include <cstdint>

namespace nxrt {

struct MemGuardOptions {
    uint64_t limit_bytes = 0;   // 0: read [memory] limit_gb from the config (default 8 GB)
    uint32_t poll_ms = 0;       // 0: [memory] poll_ms (default 250)
    // Called once from the guard thread when the footprint exceeds the limit (it must not call memguard_stop: that joins this thread). Null = default (terminate the process).
    void (*on_exceed)(uint64_t footprint, uint64_t limit, void* user) = nullptr;
    void* user = nullptr;
};

uint64_t memguard_footprint();       // bytes of private commit of this process (0 if the query fails)
// Private commit includes the video memory a process has allocated (WDDM keeps a commit charge for it). The guard is about host memory, so it
// subtracts what this function returns (the GPU's dedicated usage of this process; the Vulkan device sets it). Null: no subtraction.
// Without it a game at 4K, with 5 GB of textures and render targets in video memory, ran into the 8 GB limit with only 3 GB of host memory.
void memguard_set_discount(uint64_t (*gpu_bytes)());
// Video memory of the device the game renders on, for the 30 s log line (the Vulkan device sets it; null: not logged). usage and budget
// describe the largest device-local heap as the driver reports them (VK_EXT_memory_budget; estimates without it), heap is that heap's
// size, driver_bytes every vkAllocateMemory of this process (host imports included).
struct MemGuardVideo { uint64_t usage = 0, budget = 0, heap = 0, driver_bytes = 0; bool exact = false; };
void memguard_set_video(void (*fn)(MemGuardVideo*));
uint64_t memguard_footprint_host();  // memguard_footprint() minus the discount (what the guard compares with the limit)
double memguard_limit_gb(const char* env_text);   // positive number or 8.0
void memguard_start(const MemGuardOptions& opt = {});
void memguard_stop();                // joins the guard thread
bool memguard_running();
uint64_t memguard_peak();            // highest footprint seen by the guard
bool memguard_tripped();             // on_exceed has fired

}  // namespace nxrt
