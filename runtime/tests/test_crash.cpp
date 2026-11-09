// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for nxrt::crash_* (crash report, handler install, real termination in child processes).
// In-process cases build reports with crash_build_report() from __except filters; child modes (--child crash_av /
// crash_trap / crash_stackoverflow) install the real handler, fault, and must exit with code 4 and write a report.
#include "test_framework.h"

#include "nxrt/abi.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/symbols.h"
#include "nxrt/memory.h"
#include "nxrt/write_watch.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

NXTEST_GROUP(crash)

namespace {

constexpr uint64_t kFakeGuestAddr = 0x7100001000ull;

volatile int g_fake_mode = 0;             // 0: the fake guest writes through null, 1: it reads through null
uint64_t* volatile g_null_ptr = nullptr;  // a null pointer the compiler cannot see through
int* volatile g_null_int = nullptr;
std::string g_report;                     // filled by crash_test_capture

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

void ensure_symbols() {
    static const bool loaded = (nxrt::symbols_load_map() > 0);
    CHECK_MSG(loaded, "nxrt_tests.map not loaded (symbols needed for the report)");
}

// The register set and frame chain the fake guest runs with.
struct GuestFixture {
    ::Ctx ctx{};
    uint64_t frames[6] = {};

    void setup() {
        nxrt::reg(&ctx, 0) = 0x0001111122223333ull;
        nxrt::reg(&ctx, 1) = 0x0002222233334444ull;
        nxrt::reg(&ctx, 2) = 0x0003333344445555ull;
        nxrt::reg(&ctx, 29) = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&frames[0]));
        nxrt::lr(&ctx) = 0x7100000aaaull;
        nxrt::sp(&ctx) = 0x7e00001000ull;
        nxrt::tpidr_el0(&ctx) = 0x0000000000abcdefull;
        nxrt::tpidrro_el0(&ctx) = 0x7e00000100ull;
        // frame 0 -> frame 1 -> frame 2 (end). Each record is {saved x29, saved x30}.
        frames[0] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&frames[2]));
        frames[1] = 0x7100000111ull;
        frames[2] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&frames[4]));
        frames[3] = 0x7100000222ull;
        frames[4] = 0;
        frames[5] = 0x7100000333ull;
    }
};

}  // namespace

// The fake lifted function. extern "C" keeps the name undecorated in the map, so the report can name it.
extern "C" __declspec(noinline) void crash_test_fake_guest_fn(::Ctx* c) {
    if (g_fake_mode == 1) {
        volatile uint64_t v = *g_null_ptr;
        c->x[5] = v;
    } else {
        *g_null_ptr = c->x[0];
    }
}

// Filter-expression helper: the report is built while the exception context is still live.
__declspec(noinline) void crash_test_capture(EXCEPTION_POINTERS* ep) { g_report = nxrt::crash_build_report(ep); }

// The __try functions hold no C++ objects.
__declspec(noinline) void run_fake_guest_guarded(::Ctx* c) {
    __try {
        crash_test_fake_guest_fn(c);
    } __except (crash_test_capture(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(noinline) void raise_guarded(DWORD code) {
    __try {
        RaiseException(code, 0, 0, nullptr);
    } __except (crash_test_capture(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The fault sits in a called function: without /EHa, clang-cl does not catch a fault in the __try body itself.
__declspec(noinline) void crash_test_null_write() { *g_null_int = 1; }

__declspec(noinline) void null_write_guarded() {
    __try {
        crash_test_null_write();
    } __except (crash_test_capture(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Registers the fixture on this thread, runs the fake guest with `mode`, returns the captured report.
static std::string run_guest_fault(int mode) {
    ensure_symbols();
    GuestFixture fx;
    fx.setup();
    nxrt::fn_register(kFakeGuestAddr, &crash_test_fake_guest_fn);
    nxrt::diag_register_thread("test-main", &fx.ctx);
    g_fake_mode = mode;
    g_report.clear();
    run_fake_guest_guarded(&fx.ctx);
    nxrt::diag_unregister_thread();
    nxrt::fn_clear();
    return g_report;
}

NXTEST(write_fault_report_fields) {
    nxrt::diag_clear_recent_files();
    nxrt::diag_note_file("open", "romfs:/test/a.pak");
    const std::string rep = run_guest_fault(0);
    CHECK_MSG(has(rep, "HOST EXCEPTION 0xC0000005"), "access violation header missing");
    CHECK_MSG(has(rep, "writing address 0x0"), "writing address line missing");
    CHECK(has(rep, "host page:"));
    CHECK(has(rep, "write-watch owner=none"));
    CHECK(has(rep, "guest VM:"));
    CHECK_MSG(has(rep, "last guest function entered: f_7100001000"), "guest function not resolved");
    CHECK_MSG(has(rep, "x0 "), "x0 register line missing");
    CHECK_MSG(has(rep, "0001111122223333"), "x0 value missing");
    CHECK_MSG(has(rep, "lr=0x7100000aaa <- 0x7100000111 <- 0x7100000222 <- 0x7100000333"), "guest frame chain wrong");
    CHECK_MSG(has(rep, "(crash_test_fake_guest_fn+0x"), "host frame not symbolised from the map");
    CHECK_MSG(has(rep, "guest call stack (innermost first): f_7100001000"), "guest call stack did not map the fault");
    CHECK_MSG(has(rep, "romfs:/test/a.pak"), "noted file missing from the report");
}

NXTEST(read_fault_says_reading) {
    const std::string rep = run_guest_fault(1);
    CHECK_MSG(has(rep, "HOST EXCEPTION 0xC0000005"), "access violation header missing");
    CHECK_MSG(has(rep, "reading address 0x0"), "reading address line missing");
    CHECK_MSG(!has(rep, "writing address"), "a read fault must not say writing");
}

NXTEST(all_sections_present_and_small) {
    const std::string rep = run_guest_fault(0);
    const char* sections[] = {"HOST EXCEPTION", "last guest function entered", "guest registers", "guest frame chain",
                              "host call stack", "guest call stack", "threads:", "last files opened"};
    for (const char* s : sections) CHECK_MSG(has(rep, s), std::string("section missing: ") + s);
    CHECK_MSG(rep.size() < 64 * 1024, "report is 64 KiB or larger");
}

NXTEST(guest_trap_names_the_code) {
    ensure_symbols();
    GuestFixture fx;
    fx.setup();
    nxrt::diag_register_thread("test-main", &fx.ctx);
    g_report.clear();
    raise_guarded(0xE0000001u);
    nxrt::diag_unregister_thread();
    CHECK_MSG(has(g_report, "HOST EXCEPTION 0xE0000001 (GUEST TRAP)"), "guest trap header missing");
    CHECK_MSG(!has(g_report, "reading address"), "a trap is not an access violation");
}

NXTEST(untranslated_instruction_names_the_code) {
    ensure_symbols();
    g_report.clear();
    raise_guarded(0xE0000002u);
    CHECK_MSG(has(g_report, "HOST EXCEPTION 0xE0000002 (UNTRANSLATED INSTRUCTION)"), "untranslated header missing");
}

NXTEST(thread_without_context_says_so) {
    ensure_symbols();
    g_report.clear();
    std::thread t([] { null_write_guarded(); });
    t.join();
    CHECK_MSG(has(g_report, "guest registers: (no guest context on this thread)"), "no-context line missing");
    CHECK_MSG(has(g_report, "on thread unregistered"), "unregistered thread not named");
    CHECK_MSG(has(g_report, "guest frame chain (x29): (none)"), "frame chain for no-context thread");
}

NXTEST(recent_files_keep_last_32) {
    ensure_symbols();
    nxrt::diag_clear_recent_files();
    char path[64];
    for (int i = 0; i < 40; ++i) {
        std::snprintf(path, sizeof path, "romfs:/test/n%02d.pak", i);
        nxrt::diag_note_file("open", path);
    }
    const std::string rep = run_guest_fault(0);
    CHECK_MSG(has(rep, "n39.pak"), "newest file missing");
    CHECK_MSG(has(rep, "n08.pak"), "32nd newest file missing");
    CHECK_MSG(!has(rep, "n07.pak"), "file beyond the ring still listed");
    CHECK_MSG(!has(rep, "n00.pak"), "oldest file still listed");
    CHECK_MSG(rep.find("n08.pak") < rep.find("n39.pak"), "files not oldest first");
    nxrt::diag_clear_recent_files();
}

NXTEST(install_is_idempotent_and_uninstall_restores) {
    LPTOP_LEVEL_EXCEPTION_FILTER before = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(before);
    nxrt::CrashOptions opt;
    opt.terminate = false;
    nxrt::crash_install(opt);
    nxrt::crash_install(opt);
    LPTOP_LEVEL_EXCEPTION_FILTER ours = SetUnhandledExceptionFilter(nullptr);
    CHECK_MSG(ours != before, "crash filter not installed");
    SetUnhandledExceptionFilter(ours);
    nxrt::crash_uninstall();
    LPTOP_LEVEL_EXCEPTION_FILTER after = SetUnhandledExceptionFilter(before);
    CHECK_MSG(after == before, "uninstall did not restore the previous filter");
}

NXTEST(exception_counter_counts_reports) {
    ensure_symbols();
    const uint32_t before = nxrt::crash_exceptions_reported();
    run_guest_fault(0);
    CHECK_EQ(nxrt::crash_exceptions_reported(), before + 1u);
}

// ---- child processes: the real handler, real termination ---------------------------------------------------------

namespace {

constexpr int kChildExitCode = 4;

// Unbounded on purpose; volatile pad keeps the call from being turned into a loop.
__declspec(noinline) int crash_test_recurse(int n) {
    volatile char pad[256];
    pad[0] = static_cast<char>(n);
    if (pad[0] == 123 && n < 0) return 0;  // never true in practice; keeps the function from being provably infinite
    return crash_test_recurse(n + 1) + pad[0];
}

void child_options(int argc, char** argv, nxrt::CrashOptions& o) {
    o.report_path = argc > 0 ? argv[0] : nullptr;
    o.terminate = true;
    o.exit_code = kChildExitCode;
}

std::string temp_file(const char* prefix) {
    char dir[MAX_PATH] = {};
    char path[MAX_PATH] = {};
    GetTempPathA(sizeof dir, dir);
    GetTempFileNameA(dir, prefix, 0, path);
    return path;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Starts this exe as `--child <mode> <report>` with NXRT_LOG pointing at `log`, waits up to 30 s.
bool run_child(const char* mode, const std::string& report, const std::string& log, DWORD& exit_code) {
    char exe[MAX_PATH * 4] = {};
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string cmd = std::string("\"") + exe + "\" --child " + mode + " \"" + report + "\"";
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');
    SetEnvironmentVariableA("NXRT_LOG", log.c_str());
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(exe, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableA("NXRT_LOG", nullptr);
    if (!ok) return false;
    bool finished = WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0;
    if (!finished) TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished;
}

void remove_temp(const std::string& path) {
    DeleteFileA(path.c_str());
    DeleteFileA((path + ".prev").c_str());
}

void check_report_sections(const std::string& text) {
    const char* sections[] = {"HOST EXCEPTION", "last guest function entered", "guest registers", "guest frame chain",
                              "host call stack", "guest call stack", "threads", "last files opened"};
    for (const char* s : sections) CHECK_MSG(has(text, s), std::string("section missing: ") + s);
}

}  // namespace

NXTEST_CHILD(crash_av) {
    nxrt::CrashOptions o;
    child_options(argc, argv, o);
    ensure_symbols();
    nxrt::crash_install(o);
    GuestFixture fx;
    fx.setup();
    nxrt::fn_register(kFakeGuestAddr, &crash_test_fake_guest_fn);
    nxrt::diag_register_thread("child-main", &fx.ctx);
    nxrt::diag_note_file("open", "romfs:/test/a.pak");
    g_fake_mode = 0;
    crash_test_fake_guest_fn(&fx.ctx);  // unhandled: the filter writes the report and terminates with 4
    return 3;
}

NXTEST_CHILD(crash_trap) {
    nxrt::CrashOptions o;
    child_options(argc, argv, o);
    ensure_symbols();
    nxrt::crash_install(o);
    nxrt::trap("test trap", 0x1234);  // logs "GUEST TRAP: test trap", raises 0xE0000001
}

NXTEST_CHILD(crash_stackoverflow) {
    nxrt::CrashOptions o;
    child_options(argc, argv, o);
    ensure_symbols();
    nxrt::crash_install(o);
    return crash_test_recurse(0);
}

NXTEST(child_access_violation_terminates_with_report) {
    const std::string report = temp_file("nxr");
    const std::string log = temp_file("nxl");
    DWORD code = 0;
    CHECK_MSG(run_child("crash_av", report, log, code), "child did not finish within 30 s");
    CHECK_EQ(code, static_cast<DWORD>(kChildExitCode));
    const std::string text = read_file(report);
    CHECK_MSG(!text.empty(), "no report file written");
    check_report_sections(text);
    CHECK_MSG(has(text, "HOST EXCEPTION 0xC0000005"), "access violation header missing");
    CHECK_MSG(has(text, "writing address 0x0"), "writing address line missing");
    CHECK_MSG(has(text, "last guest function entered: f_7100001000"), "guest function not resolved");
    CHECK_MSG(has(text, "romfs:/test/a.pak"), "noted file missing");
    remove_temp(report);
    remove_temp(log);
}

NXTEST(child_guest_trap_terminates_and_logs) {
    const std::string report = temp_file("nxr");
    const std::string log = temp_file("nxl");
    DWORD code = 0;
    CHECK_MSG(run_child("crash_trap", report, log, code), "child did not finish within 30 s");
    CHECK_EQ(code, static_cast<DWORD>(kChildExitCode));
    const std::string text = read_file(report);
    CHECK_MSG(has(text, "GUEST TRAP"), "report does not name the guest trap");
    CHECK_MSG(has(text, "HOST EXCEPTION 0xE0000001"), "trap header missing");
    CHECK_MSG(has(read_file(log), "test trap"), "log does not contain the trap message");
    remove_temp(report);
    remove_temp(log);
}

NXTEST(child_stack_overflow_terminates_with_report) {
    const std::string report = temp_file("nxr");
    const std::string log = temp_file("nxl");
    DWORD code = 0;
    CHECK_MSG(run_child("crash_stackoverflow", report, log, code), "child did not finish within 30 s");
    CHECK_EQ(code, static_cast<DWORD>(kChildExitCode));
    const std::string text = read_file(report);
    CHECK_MSG(has(text, "EXCEPTION 0xC00000FD"), "stack overflow header missing");
    CHECK_MSG(has(text, "host call stack"), "host call stack missing");
    remove_temp(report);
    remove_temp(log);
}

NXTEST(guest_heap_fault_reports_protection_and_owner) {
    namespace mem = nxrt::mem;
    mem::shutdown();
    REQUIRE(mem::init() && mem::heap_create(64ull << 20) && mem::heap_commit(0, mem::kPage));
    auto* r = mem::ww::region_create(mem::kHeapBase, mem::kPage, "test compute");
    REQUIRE(r != nullptr);
    REQUIRE(mem::ww::arm(r, 0, 1) && mem::ww::guard(r, 0, 1));
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    rec.NumberParameters = 2;
    rec.ExceptionInformation[0] = 1;
    rec.ExceptionInformation[1] = mem::kHeapBase;
    CONTEXT context{};
    EXCEPTION_POINTERS ep{&rec, &context};
    const auto rep = nxrt::crash_build_report(&ep);
    CHECK(has(rep, "protect=0x1")); // PAGE_NOACCESS
    CHECK(has(rep, "committed=1 class=3")); // guest RW, host guard
    CHECK(has(rep, "owner=test compute"));
    CHECK(has(rep, "prot=1 valid=1 guard=1"));
    mem::ww::region_destroy(r);
    mem::shutdown();
}
