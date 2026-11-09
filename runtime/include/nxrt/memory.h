// SPDX-License-Identifier: GPL-3.0-or-later
// Guest address space on the host. Guest pointers are host pointers (identity map): the loader reserves each module's
// image at the address it was lifted for; the heap is a view of a pagefile-backed section so that further views (NVN
// memory pools, shared memory, MapMemory aliases) can alias the same physical pages.
// Pattern from smo-recomp (runtime/core/hostrt.c, runtime/os/smo_nn.c) and SuperMarioWonderRecomp (runtime/src/memory.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <string>

namespace nxrt::mem {

// Best-effort crash snapshot; never waits for memory-management locks.
std::string describe_page(uint64_t addr);

// ---- address-space layout (documented constants; every value is above what a Windows process with HIGHENTROPYVA off uses) ----
constexpr uint64_t kMainBase = 0x7100000000ull;     // main module image (the game): its lifted address, identity mapped
constexpr uint64_t kRemoteBase = 0x7200000000ull;   // Switch 2 remote region (svcGetInfo 29/30): reserved address space
constexpr uint64_t kRemoteSize = 0x100000000ull;   // 4 GiB
constexpr uint64_t kHleBase = 0x7400000000ull;      // placeholder addresses for host functions (never mapped): see dispatch.h
constexpr uint64_t kHleSize = 0x1000000ull;         // 16 bytes per thunk
constexpr uint64_t kSdkBase = 0x7500000000ull;      // sdk / subsdk modules sit at their own bases from here on
constexpr uint64_t kHeapBase = 0x7800000000ull;     // primary view of the heap section (svcSetHeapSize / nn::os memory heap)
constexpr uint64_t kHeapMax = 0x300000000ull;       // 12 GiB for the heap view (Switch 2 applications use 8+ GiB)
constexpr uint64_t kAliasBase = 0x6000000000ull;    // views of heap pages made on request (pools, shared memory, MapMemory)
constexpr uint64_t kAliasSize = 0x200000000ull;     // 8 GiB
constexpr uint64_t kStackBase = 0x7C00000000ull;    // guest thread stacks handed out by the runtime
constexpr uint64_t kStackSize = 0x100000000ull;
constexpr uint64_t kKernelBase = 0x7E00000000ull;   // TLS regions, main-thread stack, kernel-owned memory
constexpr uint64_t kKernelSize = 0x100000000ull;
constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kAllocGranularity = 0x10000;     // views of a section start at multiples of 64 KiB

enum class Prot { None, R, RW, RX, RWX };

struct Config {
    uint64_t heap_reserve = 0;        // bytes of section; 0 = [memory] heap_gb from the config (default 6 GiB)
    bool reserve_regions = true;      // reserve the stack and kernel regions (heap view is made by heap_create)
};

bool init(const Config& cfg = {});    // idempotent; reserves stack + kernel regions, creates nothing else
void shutdown();                      // unmaps everything this module created (tests)
bool initialised();

// ---- plain fixed mappings (images, stacks) --------------------------------------------------------------------------
// Reserve address space at exactly `addr`. Returns false when the range is not free.
bool reserve_fixed(uint64_t addr, uint64_t size, const char* tag);
bool release_fixed(uint64_t addr);                           // release a reservation made by reserve_fixed (whole region)
bool commit(uint64_t addr, uint64_t size, Prot p = Prot::RW);   // MEM_COMMIT (inside a reserve_fixed range or a heap view)
bool decommit(uint64_t addr, uint64_t size);
bool protect(uint64_t addr, uint64_t size, Prot p);
uint64_t alloc_kernel(uint64_t size, uint64_t align = kPage);   // committed RW memory from the kernel region (bump allocator)
bool is_mapped(uint64_t addr, uint64_t size);                // every byte of the range is committed and accessible (VirtualQuery)

// ---- heap section ---------------------------------------------------------------------------------------------------
// One pagefile-backed section (SEC_RESERVE, nothing is charged until committed) with a primary view at kHeapBase. Under Wine
// ([memory] heap_section = auto) it is plain private memory instead: Wine backs sections with shared files, and Linux GPU drivers
// import only anonymous memory (VK_EXT_external_memory_host through amdgpu userptr is anonymous-only), so no pool could reach the GPU.
// Without a section there are no alias or private views (heap_alias / heap_private_view return 0 / nullptr).
bool heap_create(uint64_t reserve_bytes);       // 0 = use the configured size; a second call is ignored
bool heap_exists();
bool heap_has_section();                        // false: private memory, no other views of the heap pages
uint64_t heap_reserved();                       // section size
bool heap_commit(uint64_t offset, uint64_t size);     // commit [kHeapBase+offset, +size) (page granularity)
bool heap_decommit(uint64_t offset, uint64_t size);
uint64_t heap_committed();                      // bytes committed through heap_commit minus heap_decommit
// The heap's own page table, without VirtualQuery (which walks every page of the run of identical pages after the address: up to 85 ms
// on a 10 GiB heap): 1 when every page of [addr, addr + size) is in the primary view and this module left it committed read-write, 0 when
// one is not, -1 when the table cannot say (outside the primary view, [memory] fast_query = false). heap_epoch() changes with every
// change of the table, inside the change's ww::OsWriteScope: the write tracker reads it before and after, without holding a region lock.
int heap_rw_pages(uint64_t addr, uint64_t size);
uint64_t heap_epoch();
// Another view of section pages [offset, offset+size). offset and size must be multiples of kAllocGranularity.
// `at` == 0 picks a free range in the alias region. Returns the guest(=host) address or 0. The pages must be committed
// (in the section) before they are touched through any view.
uint64_t heap_alias(uint64_t offset, uint64_t size, uint64_t at = 0, Prot p = Prot::RW);
bool heap_alias_unmap(uint64_t view_addr);      // unmap a view returned by heap_alias
// A view of section pages [offset, offset+size) at an address the system picks, outside the guest-visible alias region (the guest never sees it).
// offset must be a multiple of kAllocGranularity. The GPU import of a pool goes through such a view: the driver walks the whole mapping that
// contains the imported range, so importing 64 KB of the multi-gigabyte primary view cost tens of milliseconds, a small view costs 0.05 ms (S18).
void* heap_private_view(uint64_t offset, uint64_t size);
bool heap_private_view_unmap(void* view);
size_t heap_alias_count();
// (section offset, size) of every alias view made by heap_alias (the write tracker bans those pages, nxrt/write_watch.h).
void heap_alias_ranges(std::vector<std::pair<uint64_t, uint64_t>>& out);
// Section offset of an address inside the primary view or an alias view made by heap_alias; false when it is neither.
bool heap_offset_of(uint64_t addr, uint64_t* offset);

// ---- Horizon-style memory map for svcQueryMemory --------------------------------------------------------------------
enum MemState : uint32_t {
    kMemFree = 0x00, kMemIo = 0x01, kMemStatic = 0x02, kMemCode = 0x03, kMemCodeData = 0x04, kMemHeap = 0x05,
    kMemShared = 0x06, kMemStack = 0x0B, kMemThreadLocal = 0x0C, kMemTransfer = 0x0D, kMemAlias = 0x0E,
    kMemInaccessible = 0x10,
};
struct VmInfo {
    uint64_t addr = 0, size = 0;
    uint32_t state = kMemFree, perm = 0;
};
void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm);   // perm: bit0 R, bit1 W, bit2 X
VmInfo vm_query(uint64_t addr);                 // the block containing addr, or the free gap around it

}  // namespace nxrt::mem
