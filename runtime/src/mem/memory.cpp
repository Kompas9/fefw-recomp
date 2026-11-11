// SPDX-License-Identifier: GPL-3.0-or-later
// Guest address space on the host (layout and contract: include/nxrt/memory.h).
//  - fixed reservations at exact addresses (images, stacks, the kernel region), committed on demand;
//  - the heap: one pagefile-backed section (SEC_RESERVE) with a primary view at kHeapBase and alias views of the same
//    section pages, so one physical page can be seen at two guest addresses;
//  - a Horizon-style vm map kept beside the real mappings for svcQueryMemory.
#include "nxrt/game.h"
#include "nxrt/memory.h"

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/write_watch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace nxrt::mem {
namespace {

constexpr const char* kTag = "mem";
constexpr uint64_t kAddrLimit = 0x800000000000ull;  // user-mode address limit of x64 Windows (47 bits)
constexpr uint64_t kVmEnd = 0x8000000000ull;         // end of the address space reported by vm_query
constexpr int kAliasTries = 16;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// Whole pages covering [addr, addr+size). False when the range leaves the user address space.
bool page_span(uint64_t addr, uint64_t size, uint64_t* start, uint64_t* end) {
    if (size > kAddrLimit || addr > kAddrLimit - size) return false;
    *start = addr & ~(kPage - 1);
    *end = align_up(addr + size, kPage);
    return true;
}

DWORD protect_of(Prot p) {
    switch (p) {
        case Prot::R: return PAGE_READONLY;
        case Prot::RW: return PAGE_READWRITE;
        case Prot::RX: return PAGE_EXECUTE_READ;
        case Prot::RWX: return PAGE_EXECUTE_READWRITE;
        case Prot::None: break;
    }
    return PAGE_NOACCESS;
}

// ---- fixed reservations (guarded by g_fixed_mu) ----
struct Fixed {
    uint64_t size;
    std::string tag;
};
std::mutex g_fixed_mu;
std::map<uint64_t, Fixed> g_fixed;

// ---- module state ----
std::mutex g_state_mu;                    // init / shutdown
std::atomic<bool> g_inited{false};
std::atomic<uint64_t> g_default_heap{0};  // bytes, from Config or [memory] heap_gb

std::mutex g_kernel_mu;
uint64_t g_kernel_next = kKernelBase;     // guarded by g_kernel_mu

std::mutex g_vm_mu;
std::map<uint64_t, VmInfo> g_vm;          // start -> block, guarded by g_vm_mu

// ---- heap (guarded by g_heap_mu) ----
std::mutex g_heap_mu;
HANDLE g_section = nullptr;
uint64_t g_heap_size = 0;                 // 0 = no heap; otherwise the section size = size of the primary view
std::vector<uint64_t> g_commit_bits;      // one bit per 4 KiB page of the section, set by heap_commit
uint64_t g_committed = 0;                 // bytes of pages whose bit is set
// Per 4 KiB page of the section, what the host reports for it: 0 never committed, 1 committed but no-access (a decommitted page or one
// the guest made inaccessible), 2 committed and accessible but not writable, 3 committed read-write. VirtualQuery over the section view
// walks every page table entry of the run of identical pages (about 75 ms for a 3 GiB committed heap), so is_mapped, protect and the
// write tracker's arm (heap_rw_pages) answer heap ranges from this table.
std::vector<uint8_t> g_heap_cls;
std::atomic<uint64_t> g_heap_epoch{0};    // bumped by every change of the table, inside the change's OsWriteScope
struct Alias {
    uint64_t size;
    uint64_t offset;                      // section offset of the first byte of the view
};
std::map<uint64_t, Alias> g_aliases;      // view address -> record
// Alias region in use: start -> size for every live view and for every candidate address the kernel refused.
std::map<uint64_t, uint64_t> g_alias_used;

// ---- gauges (include/nxrt/gauge.h); each callback takes the lock of the state it reads ----
// Removes its gauge when this file's statics are torn down (declared after them, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_heap_reserved("mem.heap_reserved", [] { return static_cast<int64_t>(heap_reserved()); });   // section size
GaugeHook gauge_heap_committed("mem.heap_committed", [] { return static_cast<int64_t>(heap_committed()); });   // committed through heap_commit
GaugeHook gauge_aliases("mem.aliases", [] {   // heap alias views live (g_heap_mu)
    std::lock_guard<std::mutex> lk(g_heap_mu);
    return static_cast<int64_t>(g_aliases.size());
});
GaugeHook gauge_alias_used("mem.alias_used", [] {   // alias-region ranges in use, plus addresses the kernel refused (never freed: only grows)
    std::lock_guard<std::mutex> lk(g_heap_mu);
    return static_cast<int64_t>(g_alias_used.size());
});
GaugeHook gauge_fixed("mem.fixed_regions", [] {   // fixed reservations of this module (images, stacks, kernel region)
    std::lock_guard<std::mutex> lk(g_fixed_mu);
    return static_cast<int64_t>(g_fixed.size());
});
GaugeHook gauge_fixed_bytes("mem.fixed_bytes", [] {
    std::lock_guard<std::mutex> lk(g_fixed_mu);
    uint64_t n = 0;
    for (const auto& kv : g_fixed) n += kv.second.size;
    return static_cast<int64_t>(n);
});
GaugeHook gauge_vm_blocks("mem.vm_blocks", [] {   // blocks of the Horizon-style vm map kept for QueryMemory
    std::lock_guard<std::mutex> lk(g_vm_mu);
    return static_cast<int64_t>(g_vm.size());
});

// [memory] fast_query = false: ask the host (VirtualQuery) instead of the page table above; for before/after measurements.
bool fast_query() {
    static const bool v = config_get_bool("memory", "fast_query", true);
    return v;
}

// True when [start, end) lies inside the heap primary view (g_heap_mu held).
bool in_heap_view(uint64_t start, uint64_t end) {
    return fast_query() && g_heap_size && !g_heap_cls.empty() && start >= kHeapBase && end <= kHeapBase + g_heap_size && start < end;
}

bool bit_test(uint64_t page) { return (g_commit_bits[page / 64] >> (page % 64)) & 1; }
void bit_set(uint64_t page) { g_commit_bits[page / 64] |= uint64_t{1} << (page % 64); }
void bit_clear(uint64_t page) { g_commit_bits[page / 64] &= ~(uint64_t{1} << (page % 64)); }

// First address >= from at which `size` bytes are free in the alias region; 0 when there is none.
uint64_t find_free_alias(uint64_t from, uint64_t size) {
    const uint64_t limit = kAliasBase + kAliasSize;
    uint64_t cand = std::max(from, kAliasBase);
    for (;;) {
        if (cand >= limit || size > limit - cand) return 0;
        bool moved = false;
        auto it = g_alias_used.upper_bound(cand);
        if (it != g_alias_used.begin()) {
            auto p = std::prev(it);
            if (p->first + p->second > cand) {
                cand = align_up(p->first + p->second, kAllocGranularity);
                moved = true;
            }
        }
        if (!moved && it != g_alias_used.end() && it->first < cand + size) {
            cand = align_up(it->first + it->second, kAllocGranularity);
            moved = true;
        }
        if (!moved) return cand;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// init / shutdown

bool init(const Config& cfg) {
    std::lock_guard lk(g_state_mu);
    if (g_inited.load()) return true;

    uint64_t heap = cfg.heap_reserve;
    if (heap == 0) {
        int64_t gb = nxrt::config_get_int("memory", "heap_gb", nxrt::game::info().platform == nxrt::game::Platform::Switch2 ? 12 : 6);
        if (gb < 1 || gb > 1024) {
            NXLOG_W(kTag, "[memory] heap_gb=%lld is out of range: using 6", static_cast<long long>(gb));
            gb = 6;
        }
        heap = static_cast<uint64_t>(gb) << 30;
    }
    g_default_heap.store(align_up(heap, kAllocGranularity));

    if (cfg.reserve_regions) {
        if (!reserve_fixed(kStackBase, kStackSize, "stack")) {
            NXLOG_E(kTag, "init failed: the stack region could not be reserved");
            return false;
        }
        if (!reserve_fixed(kKernelBase, kKernelSize, "kernel")) {
            NXLOG_E(kTag, "init failed: the kernel region could not be reserved");
            release_fixed(kStackBase);
            return false;
        }
    }
    {
        std::lock_guard kl(g_kernel_mu);
        g_kernel_next = kKernelBase;
    }
    g_inited.store(true);
    NXLOG_I(kTag, "init: stack %llx (+%llx), kernel %llx (+%llx), default heap %llu MiB, regions %s",
            static_cast<unsigned long long>(kStackBase), static_cast<unsigned long long>(kStackSize),
            static_cast<unsigned long long>(kKernelBase), static_cast<unsigned long long>(kKernelSize),
            static_cast<unsigned long long>(g_default_heap.load() >> 20), cfg.reserve_regions ? "reserved" : "skipped");
    return true;
}

void shutdown() {
    std::lock_guard lk(g_state_mu);
    {
        std::lock_guard hl(g_heap_mu);
        for (auto& [view, rec] : g_aliases) {
            (void)rec;
            if (!UnmapViewOfFile(reinterpret_cast<void*>(view))) {
                DWORD e = GetLastError();
                NXLOG_E(kTag, "shutdown: UnmapViewOfFile(alias %llx) failed, error %lu",
                        static_cast<unsigned long long>(view), static_cast<unsigned long>(e));
            }
        }
        g_aliases.clear();
        g_alias_used.clear();
        if (g_heap_size && !g_section) {
            VirtualFree(reinterpret_cast<void*>(kHeapBase), 0, MEM_RELEASE);   // the private heap (heap_create)
        } else if (g_heap_size) {
            if (!UnmapViewOfFile(reinterpret_cast<void*>(kHeapBase))) {
                DWORD e = GetLastError();
                NXLOG_E(kTag, "shutdown: UnmapViewOfFile(heap) failed, error %lu", static_cast<unsigned long>(e));
            }
        }
        if (g_section) CloseHandle(g_section);
        g_section = nullptr;
        g_heap_size = 0;
        g_commit_bits.clear();
        g_heap_cls.clear();
        g_committed = 0;
    }
    {
        std::lock_guard fl(g_fixed_mu);
        for (auto& [addr, f] : g_fixed) {
            (void)f;
            if (!VirtualFree(reinterpret_cast<void*>(addr), 0, MEM_RELEASE)) {
                DWORD e = GetLastError();
                NXLOG_E(kTag, "shutdown: VirtualFree(%llx) failed, error %lu", static_cast<unsigned long long>(addr),
                        static_cast<unsigned long>(e));
            }
        }
        g_fixed.clear();
    }
    {
        std::lock_guard vl(g_vm_mu);
        g_vm.clear();
    }
    {
        std::lock_guard kl(g_kernel_mu);
        g_kernel_next = kKernelBase;
    }
    g_default_heap.store(0);
    g_inited.store(false);
    NXLOG_I(kTag, "shutdown: all mappings of this module released");
}

bool initialised() { return g_inited.load(); }

// ---------------------------------------------------------------------------------------------------------------------
// fixed mappings

bool reserve_fixed(uint64_t addr, uint64_t size, const char* tag) {
    if (size == 0 || addr % kAllocGranularity != 0 || addr >= kAddrLimit || size > kAddrLimit - addr) {
        NXLOG_E(kTag, "reserve %llx (+%llx) %s: address must be 64 KiB aligned and inside the user address space",
                static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), tag ? tag : "");
        return false;
    }
    size = align_up(size, kPage);
    std::lock_guard lk(g_fixed_mu);
    void* p = VirtualAlloc(reinterpret_cast<void*>(addr), static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "reserve %llx (+%llx) %s: VirtualAlloc failed, error %lu",
                static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), tag ? tag : "",
                static_cast<unsigned long>(e));
        return false;
    }
    if (reinterpret_cast<uint64_t>(p) != addr) {  // cannot happen with MEM_RESERVE at a fixed address; never accept it
        VirtualFree(p, 0, MEM_RELEASE);
        NXLOG_E(kTag, "reserve %llx %s: got %p instead", static_cast<unsigned long long>(addr), tag ? tag : "", p);
        return false;
    }
    g_fixed[addr] = Fixed{size, tag ? tag : ""};
    NXLOG_D(kTag, "reserved %llx (+%llx) %s", static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(size), tag ? tag : "");
    return true;
}

bool release_fixed(uint64_t addr) {
    std::lock_guard lk(g_fixed_mu);
    auto it = g_fixed.find(addr);
    if (it == g_fixed.end()) {
        NXLOG_W(kTag, "release %llx: no reservation of this module starts there", static_cast<unsigned long long>(addr));
        return false;
    }
    if (!VirtualFree(reinterpret_cast<void*>(addr), 0, MEM_RELEASE)) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "release %llx: VirtualFree failed, error %lu", static_cast<unsigned long long>(addr),
                static_cast<unsigned long>(e));
        return false;
    }
    g_fixed.erase(it);
    return true;
}

bool commit(uint64_t addr, uint64_t size, Prot p) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);   // the write tracker keeps out of the range while it changes
    if (!VirtualAlloc(reinterpret_cast<void*>(start), static_cast<SIZE_T>(end - start), MEM_COMMIT, protect_of(p))) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "commit %llx (+%llx): VirtualAlloc failed, error %lu", static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end - start), static_cast<unsigned long>(e));
        return false;
    }
    return true;
}

bool decommit(uint64_t addr, uint64_t size) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);
    if (!VirtualFree(reinterpret_cast<void*>(start), static_cast<SIZE_T>(end - start), MEM_DECOMMIT)) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "decommit %llx (+%llx): VirtualFree failed, error %lu", static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end - start), static_cast<unsigned long>(e));
        return false;
    }
    return true;
}

bool protect(uint64_t addr, uint64_t size, Prot p) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const DWORD np = protect_of(p);
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);   // pages the write tracker armed get this protection, not its own
    {   // heap: the page table says whether every page is committed, so one VirtualProtect does it (no VirtualQuery over the view)
        std::lock_guard hk(g_heap_mu);
        if (in_heap_view(start, end)) {
            const uint64_t p0 = (start - kHeapBase) / kPage, p1 = (end - kHeapBase) / kPage;
            bool all = true;
            for (uint64_t q = p0; q < p1 && all; ++q) all = g_heap_cls[q] != 0;
            DWORD old = 0;
            if (all && VirtualProtect(reinterpret_cast<void*>(start), static_cast<SIZE_T>(end - start), np, &old)) {
                std::fill(g_heap_cls.begin() + static_cast<std::ptrdiff_t>(p0), g_heap_cls.begin() + static_cast<std::ptrdiff_t>(p1),
                          np == PAGE_NOACCESS ? uint8_t{1} : np == PAGE_READWRITE ? uint8_t{3} : uint8_t{2});
                g_heap_epoch.fetch_add(1, std::memory_order_seq_cst);
                return true;
            }
        }
    }
    // Pass 1: every page must be committed, so a failure never leaves the range half changed.
    for (uint64_t cur = start; cur < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi) != sizeof mbi || mbi.RegionSize == 0) {
            NXLOG_E(kTag, "protect %llx: VirtualQuery failed, error %lu", static_cast<unsigned long long>(cur),
                    static_cast<unsigned long>(GetLastError()));
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            NXLOG_E(kTag, "protect %llx: page is not committed", static_cast<unsigned long long>(cur));
            return false;
        }
        cur = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
    }
    // Pass 2: region by region (a range can span several allocations, each has its own region).
    g_heap_epoch.fetch_add(1, std::memory_order_seq_cst);
    for (uint64_t cur = start; cur < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi);
        const uint64_t chunk_end = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(cur), static_cast<SIZE_T>(chunk_end - cur), np, &old)) {
            NXLOG_E(kTag, "protect %llx (+%llx): VirtualProtect failed, error %lu", static_cast<unsigned long long>(cur),
                    static_cast<unsigned long long>(chunk_end - cur), static_cast<unsigned long>(GetLastError()));
            return false;
        }
        cur = chunk_end;
    }
    return true;
}

uint64_t alloc_kernel(uint64_t size, uint64_t align) {
    if (!initialised() && !init()) return 0;
    if (align == 0) align = kPage;
    if (align & (align - 1)) {
        NXLOG_E(kTag, "alloc_kernel: alignment %llx is not a power of two", static_cast<unsigned long long>(align));
        return 0;
    }
    align = std::max(align, kPage);
    if (size == 0) size = 1;
    if (size > kKernelSize) return 0;
    size = align_up(size, kPage);

    std::lock_guard lk(g_kernel_mu);
    const uint64_t a = align_up(g_kernel_next, align);
    if (a < g_kernel_next || a > kKernelBase + kKernelSize || size > kKernelBase + kKernelSize - a) {
        NXLOG_E(kTag, "alloc_kernel: kernel region exhausted (%llx used of %llx)",
                static_cast<unsigned long long>(g_kernel_next - kKernelBase),
                static_cast<unsigned long long>(kKernelSize));
        return 0;
    }
    if (!commit(a, size, Prot::RW)) return 0;
    g_kernel_next = a + size;
    return a;
}

bool is_mapped(uint64_t addr, uint64_t size) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    {
        std::lock_guard hk(g_heap_mu);
        if (in_heap_view(start, end)) {
            const uint64_t p0 = (start - kHeapBase) / kPage, p1 = (end - kHeapBase) / kPage;
            for (uint64_t q = p0; q < p1; ++q)
                if (g_heap_cls[q] < 2) return false;
            return true;
        }
    }
    for (uint64_t cur = start; cur < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi) != sizeof mbi || mbi.RegionSize == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & 0xFF) == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
        cur = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// heap section

bool heap_create(uint64_t reserve_bytes) {
    if (!initialised() && !init()) return false;
    std::lock_guard lk(g_heap_mu);
    if (g_heap_size) return true;  // a heap already exists: nothing to do

    const uint64_t want = reserve_bytes ? reserve_bytes : g_default_heap.load();
    if (want == 0) {
        NXLOG_E(kTag, "heap_create: no size given and no configured default");
        return false;
    }
    const uint64_t size = align_up(want, kAllocGranularity);
    if (size > kHeapMax) {
        NXLOG_E(kTag, "heap_create: %llu bytes exceed the heap region limit of %llu bytes",
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(kHeapMax));
        return false;
    }
    // [memory] heap_section: auto (default) = a section unless running under Wine (memory.h), 1 = always, 0 = never
    const std::string mode = nxrt::config_get_str("memory", "heap_section", "auto");
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const bool wine = ntdll && GetProcAddress(ntdll, "wine_get_version");
    if (mode == "0" || (mode != "1" && wine)) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(kHeapBase), static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
        if (reinterpret_cast<uint64_t>(p) != kHeapBase) {
            NXLOG_E(kTag, "heap_create: VirtualAlloc(MEM_RESERVE %llu MiB) at %llx failed, error %lu", static_cast<unsigned long long>(size >> 20),
                    static_cast<unsigned long long>(kHeapBase), static_cast<unsigned long>(GetLastError()));
            if (p) VirtualFree(p, 0, MEM_RELEASE);
            return false;
        }
        g_heap_size = size;
        g_commit_bits.assign((size / kPage + 63) / 64, 0);
        g_heap_cls.assign(static_cast<size_t>(size / kPage), 0);
        g_committed = 0;
        NXLOG_I(kTag, "heap: private memory of %llu MiB at %llx, no section (%s): no alias views, pools import at their own address",
                static_cast<unsigned long long>(size >> 20), static_cast<unsigned long long>(kHeapBase), wine ? "Wine" : "[memory] heap_section = 0");
        return true;
    }
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                    static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFFu), nullptr);
    if (!sec) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "heap_create: CreateFileMapping(%llu MiB) failed, error %lu",
                static_cast<unsigned long long>(size >> 20), static_cast<unsigned long>(e));
        return false;
    }
    void* v = MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(size),
                              reinterpret_cast<void*>(kHeapBase));
    if (!v) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "heap_create: MapViewOfFileEx at %llx failed, error %lu",
                static_cast<unsigned long long>(kHeapBase), static_cast<unsigned long>(e));
        CloseHandle(sec);
        return false;
    }
    if (reinterpret_cast<uint64_t>(v) != kHeapBase) {
        NXLOG_E(kTag, "heap_create: primary view landed at %p, not %llx", v, static_cast<unsigned long long>(kHeapBase));
        UnmapViewOfFile(v);
        CloseHandle(sec);
        return false;
    }
    g_section = sec;
    g_heap_size = size;
    g_commit_bits.assign((size / kPage + 63) / 64, 0);
    g_heap_cls.assign(static_cast<size_t>(size / kPage), 0);
    g_committed = 0;
    NXLOG_I(kTag, "heap: section of %llu MiB, primary view at %llx", static_cast<unsigned long long>(size >> 20),
            static_cast<unsigned long long>(kHeapBase));
    return true;
}

bool heap_exists() {
    std::lock_guard lk(g_heap_mu);
    return g_heap_size != 0;
}

bool heap_has_section() {
    std::lock_guard lk(g_heap_mu);
    return g_section != nullptr;
}

uint64_t heap_reserved() {
    std::lock_guard lk(g_heap_mu);
    return g_heap_size;
}

bool heap_commit(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size) {
        NXLOG_W(kTag, "heap_commit: no heap");
        return false;
    }
    if (size == 0) return true;
    if (offset > g_heap_size || size > g_heap_size - offset) {
        NXLOG_E(kTag, "heap_commit: [%llx, +%llx) is outside the %llu MiB section",
                static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(g_heap_size >> 20));
        return false;
    }
    const uint64_t start = offset & ~(kPage - 1);
    const uint64_t end = align_up(offset + size, kPage);
    const ww::OsWriteScope wws(reinterpret_cast<void*>(kHeapBase + start), end - start);   // MEM_COMMIT makes committed pages read-write again
    // The game sets a heap of about 11 GB at boot and all of it is committed here. When the system's commit charge is near its limit (just after
    // the launcher's setup, or with a small page file) this fails, the game turns the result into nn::os OutOfMemory (0x1003) and calls svcBreak.
    // Windows grows a system-managed page file after such a failure and other processes give memory back, so wait a little and try again.
    bool ok = VirtualAlloc(reinterpret_cast<void*>(kHeapBase + start), static_cast<SIZE_T>(end - start), MEM_COMMIT, PAGE_READWRITE) != nullptr;
    for (int attempt = 1; !ok && attempt <= 40; ++attempt) {
        const DWORD e = GetLastError();
        if (e != ERROR_COMMITMENT_LIMIT && e != ERROR_NOT_ENOUGH_MEMORY && e != ERROR_COMMITMENT_MINIMUM) break;
        if (attempt == 1)
            NXLOG_W(kTag, "heap_commit [%llx, +%llx): the system is out of commit (error %lu); waiting up to 10 s for the page file to grow",
                    static_cast<unsigned long long>(start), static_cast<unsigned long long>(end - start), static_cast<unsigned long>(e));
        Sleep(250);
        ok = VirtualAlloc(reinterpret_cast<void*>(kHeapBase + start), static_cast<SIZE_T>(end - start), MEM_COMMIT, PAGE_READWRITE) != nullptr;
        if (ok) NXLOG_I(kTag, "heap_commit: committed after %d retries", attempt);
    }
    if (!ok) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "heap_commit [%llx, +%llx): VirtualAlloc failed, error %lu",
                static_cast<unsigned long long>(start), static_cast<unsigned long long>(end - start),
                static_cast<unsigned long>(e));
        return false;
    }
    for (uint64_t p = start / kPage; p < end / kPage; p++) {
        if (!bit_test(p)) {
            bit_set(p);
            g_committed += kPage;
        }
        g_heap_cls[p] = 3;       // MEM_COMMIT with PAGE_READWRITE applies to pages that were committed already too
    }
    g_heap_epoch.fetch_add(1, std::memory_order_seq_cst);
    return true;
}

bool heap_decommit(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size) return false;
    if (size == 0) return true;
    if (offset > g_heap_size || size > g_heap_size - offset) {
        NXLOG_E(kTag, "heap_decommit: [%llx, +%llx) is outside the section", static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size));
        return false;
    }
    // VirtualFree(MEM_DECOMMIT) is refused on pages of a section view (ERROR_INVALID_PARAMETER), so the pages are made
    // PAGE_NOACCESS through the primary view instead: guest accesses fault, the host pages stay charged to the section
    // (as in smo-recomp's SetMemoryHeapSize), and the alias views still show the section's contents.
    const uint64_t start = offset & ~(kPage - 1);
    const uint64_t end = align_up(offset + size, kPage);
    const ww::OsWriteScope wws(reinterpret_cast<void*>(kHeapBase + start), end - start);
    for (uint64_t p = start / kPage; p < end / kPage;) {
        if (!bit_test(p)) {
            p++;
            continue;
        }
        uint64_t run = p;
        while (run < end / kPage && bit_test(run)) run++;
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(kHeapBase + p * kPage), static_cast<SIZE_T>((run - p) * kPage),
                            PAGE_NOACCESS, &old)) {
            DWORD e = GetLastError();
            NXLOG_E(kTag, "heap_decommit [%llx, +%llx): VirtualProtect failed, error %lu",
                    static_cast<unsigned long long>(p * kPage), static_cast<unsigned long long>((run - p) * kPage),
                    static_cast<unsigned long>(e));
            return false;
        }
        for (uint64_t q = p; q < run; q++) {
            bit_clear(q);
            g_committed -= kPage;
            g_heap_cls[q] = 1;
        }
        g_heap_epoch.fetch_add(1, std::memory_order_seq_cst);
        p = run;
    }
    return true;
}

int heap_rw_pages(uint64_t addr, uint64_t size) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return 0;
    std::lock_guard lk(g_heap_mu);
    if (!in_heap_view(start, end)) return -1;
    const uint64_t p0 = (start - kHeapBase) / kPage, p1 = (end - kHeapBase) / kPage;
    for (uint64_t q = p0; q < p1; ++q)
        if (g_heap_cls[q] != 3) return 0;
    return 1;
}

uint64_t heap_epoch() { return g_heap_epoch.load(std::memory_order_seq_cst); }

uint64_t heap_committed() {
    std::lock_guard lk(g_heap_mu);
    return g_committed;
}

uint64_t heap_alias(uint64_t offset, uint64_t size, uint64_t at, Prot p) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size || !g_section) {
        NXLOG_W(kTag, "heap_alias: %s", g_heap_size ? "the heap is private memory (no section): no alias views" : "no heap");
        return 0;
    }
    if (size == 0 || offset % kAllocGranularity || size % kAllocGranularity || offset > g_heap_size ||
        size > g_heap_size - offset) {
        NXLOG_W(kTag, "heap_alias: [%llx, +%llx) must be 64 KiB multiples inside the section",
                static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));
        return 0;
    }
    if (at % kAllocGranularity) {
        NXLOG_W(kTag, "heap_alias: target %llx is not 64 KiB aligned", static_cast<unsigned long long>(at));
        return 0;
    }
    const DWORD access = (p == Prot::R) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    const DWORD off_hi = static_cast<DWORD>(offset >> 32);
    const DWORD off_lo = static_cast<DWORD>(offset & 0xFFFFFFFFu);

    ww::ban_section(offset, size);   // before the view exists: writes through it would not fault in the primary view
    if (at != 0) {
        void* v = MapViewOfFileEx(g_section, access, off_hi, off_lo, static_cast<SIZE_T>(size), reinterpret_cast<void*>(at));
        if (!v) {
            DWORD e = GetLastError();
            NXLOG_E(kTag, "heap_alias at %llx (section %llx, +%llx): MapViewOfFileEx failed, error %lu",
                    static_cast<unsigned long long>(at), static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(size), static_cast<unsigned long>(e));
            return 0;
        }
        if (reinterpret_cast<uint64_t>(v) != at) {
            UnmapViewOfFile(v);
            NXLOG_E(kTag, "heap_alias: view landed at %p, not %llx", v, static_cast<unsigned long long>(at));
            return 0;
        }
        g_aliases.emplace(at, Alias{size, offset});
        g_alias_used[at] = size;
        NXLOG_I(kTag, "heap alias: section %llx (+%llx) at %llx", static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(at));
        return at;
    }

    uint64_t cand = kAliasBase;
    for (int tries = 0; tries < kAliasTries; tries++) {
        cand = find_free_alias(cand, size);
        if (!cand) break;
        void* v = MapViewOfFileEx(g_section, access, off_hi, off_lo, static_cast<SIZE_T>(size), reinterpret_cast<void*>(cand));
        if (v && reinterpret_cast<uint64_t>(v) == cand) {
            g_aliases.emplace(cand, Alias{size, offset});
            g_alias_used[cand] = size;
            NXLOG_I(kTag, "heap alias: section %llx (+%llx) at %llx", static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(size), static_cast<unsigned long long>(cand));
            return cand;
        }
        if (v) UnmapViewOfFile(v);
        NXLOG_W(kTag, "heap_alias: %llx is taken (error %lu): marking it unusable",
                static_cast<unsigned long long>(cand), static_cast<unsigned long>(GetLastError()));
        g_alias_used[cand] = size;  // refused by the kernel: never offer it again
    }
    NXLOG_E(kTag, "heap_alias: no free range for %llx bytes in the alias region", static_cast<unsigned long long>(size));
    return 0;
}

void* heap_private_view(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_section || size == 0 || offset % kAllocGranularity || offset > g_heap_size || size > g_heap_size - offset) return nullptr;
    return MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset & 0xFFFFFFFFu), static_cast<SIZE_T>(size));
}

bool heap_private_view_unmap(void* view) { return view && UnmapViewOfFile(view) != 0; }

bool heap_alias_unmap(uint64_t view_addr) {
    std::lock_guard lk(g_heap_mu);
    auto it = g_aliases.find(view_addr);
    if (it == g_aliases.end()) {
        NXLOG_W(kTag, "heap_alias_unmap: %llx is not an alias view", static_cast<unsigned long long>(view_addr));
        return false;
    }
    if (!UnmapViewOfFile(reinterpret_cast<void*>(view_addr))) {
        NXLOG_E(kTag, "heap_alias_unmap %llx: UnmapViewOfFile failed, error %lu",
                static_cast<unsigned long long>(view_addr), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    g_alias_used.erase(view_addr);
    g_aliases.erase(it);
    return true;
}

void heap_alias_ranges(std::vector<std::pair<uint64_t, uint64_t>>& out) {
    std::lock_guard lk(g_heap_mu);
    for (const auto& a : g_aliases) out.emplace_back(a.second.offset, a.second.size);
}

size_t heap_alias_count() {
    std::lock_guard lk(g_heap_mu);
    return g_aliases.size();
}

bool heap_offset_of(uint64_t addr, uint64_t* offset) {
    if (!offset) return false;
    std::lock_guard lk(g_heap_mu);
    if (g_heap_size && addr >= kHeapBase && addr - kHeapBase < g_heap_size) {
        *offset = addr - kHeapBase;
        return true;
    }
    auto it = g_aliases.upper_bound(addr);
    if (it == g_aliases.begin()) return false;
    --it;
    if (addr - it->first < it->second.size) {
        *offset = it->second.offset + (addr - it->first);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// vm map (svcQueryMemory)

std::string describe_page(uint64_t addr) {
    std::string out;
    char line[256];
    {
        std::unique_lock lk(g_heap_mu, std::try_to_lock);
        if (!lk.owns_lock()) out += "  heap metadata: busy\n";
        else if (addr >= kHeapBase && addr - kHeapBase < g_heap_size) {
            const uint64_t pg = (addr - kHeapBase) / kPage;
            std::snprintf(line, sizeof line, "  heap metadata: page=%llu committed=%d class=%u epoch=%llu (class: 0=reserved 1=noaccess 2=readonly 3=readwrite)\n",
                (unsigned long long)pg, bit_test(pg), (unsigned)g_heap_cls[pg], (unsigned long long)g_heap_epoch.load());
            out += line;
        } else out += "  heap metadata: outside primary heap\n";
    }
    {
        std::unique_lock lk(g_vm_mu, std::try_to_lock);
        if (!lk.owns_lock()) out += "  guest VM metadata: busy\n";
        else {
            auto it = g_vm.upper_bound(addr);
            if (it != g_vm.begin() && addr - std::prev(it)->second.addr < std::prev(it)->second.size) {
                const auto& v = std::prev(it)->second;
                std::snprintf(line, sizeof line, "  guest VM: base=0x%llx size=0x%llx state=0x%x perm=0x%x\n",
                    (unsigned long long)v.addr, (unsigned long long)v.size, v.state, v.perm);
                out += line;
            } else out += "  guest VM: free\n";
        }
    }
    return out;
}

void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm) {
    if (size == 0 || addr + size < addr) return;
    const uint64_t end = addr + size;
    std::lock_guard lk(g_vm_mu);
    // Cut every block that overlaps [addr, end); the parts outside the new range stay.
    auto it = g_vm.lower_bound(addr);
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (p->second.addr + p->second.size > addr) it = p;
    }
    while (it != g_vm.end() && it->second.addr < end) {
        VmInfo b = it->second;
        it = g_vm.erase(it);
        if (b.addr < addr) g_vm.emplace(b.addr, VmInfo{b.addr, addr - b.addr, b.state, b.perm});
        if (b.addr + b.size > end) g_vm.emplace(end, VmInfo{end, b.addr + b.size - end, b.state, b.perm});
    }
    if (state != kMemFree) g_vm.emplace(addr, VmInfo{addr, size, state, perm});
}

VmInfo vm_query(uint64_t addr) {
    std::lock_guard lk(g_vm_mu);
    auto it = g_vm.upper_bound(addr);  // first block that starts after addr
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (addr < p->second.addr + p->second.size) return p->second;
        const uint64_t gap_start = p->second.addr + p->second.size;
        const uint64_t gap_end = it == g_vm.end() ? kVmEnd : it->first;
        return VmInfo{gap_start, gap_end - gap_start, kMemFree, 0};
    }
    const uint64_t gap_end = it == g_vm.end() ? kVmEnd : it->first;
    return VmInfo{0, gap_end, kMemFree, 0};
}

}  // namespace nxrt::mem
