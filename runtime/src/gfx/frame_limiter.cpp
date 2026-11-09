// SPDX-License-Identifier: GPL-3.0-or-later
// FrameLimiter: schedule on QueryPerformanceCounter; coarse wait on a high-resolution waitable timer; spin for the last
// stretch. The behaviour and the thread rules are the contract in include/nxrt/frame_limiter.h.
#include "nxrt/frame_limiter.h"

#include "nxrt/log.h"

#include <windows.h>
#include <emmintrin.h>  // _mm_pause

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace nxrt {

namespace {
constexpr double kMinMarginNs = 300000.0;   // 0.3 ms: the spin always covers at least this much
constexpr double kMaxMarginNs = 3000000.0;  // 3 ms: the spin never covers more than this
constexpr double kMaxCarryPeriods = 0.1;    // a late frame is made up for by at most this fraction of a period (no bursts)
constexpr double kResyncPeriods = 4.0;      // a schedule more than this many periods behind restarts from now
constexpr std::size_t kRing = 8192;         // frame intervals kept for stats()
}  // namespace

struct FrameLimiter::Impl {
    std::atomic<double> target_fps{0.0};
    std::atomic<double> game_fps{0.0};
    std::atomic<FrameLimiter::PreFrameFn> pre_fn{nullptr};
    std::atomic<void*> pre_user{nullptr};
    HANDLE timer = nullptr;  // high-resolution waitable timer; null means the Sleep(1) coarse wait

    // Guards the state below. begin_frame holds it only for bookkeeping, never during the wait.
    std::mutex mu;
    bool has_sched = false;
    double sched_ns = 0.0;    // scheduled start of the last frame (double, so a fractional period does not round away)
    bool has_prev = false;
    uint64_t prev_start = 0;  // actual start of the last frame
    uint64_t ring[kRing] = {};
    std::size_t ring_n = 0, ring_pos = 0;
    uint64_t late_ring[kRing] = {};          // lateness (ns) of the frames that waited
    std::size_t late_n = 0, late_pos = 0;
    uint64_t overran_frames = 0;             // frames that did not have to wait
    uint64_t last_wait = 0, last_late = 0;

    // Used only by begin_frame, which is called from one thread per limiter.
    double overshoot_ema_ns = 0.0;  // average of (actual wake - requested wake) of the timer

    void record_locked(uint64_t t) {
        if (has_prev) {
            ring[ring_pos] = t - prev_start;
            ring_pos = (ring_pos + 1) % kRing;
            if (ring_n < kRing) ++ring_n;
        }
        has_prev = true;
        prev_start = t;
    }

    // How long before the target the timer stops and the spin takes over.
    uint64_t margin_ns() const {
        const double m = std::min(std::max(kMinMarginNs, 2.0 * overshoot_ema_ns), kMaxMarginNs);
        return static_cast<uint64_t>(m);
    }

    // Sleeps on the timer for coarse_ns (relative), then updates the overshoot average. False if the timer call failed.
    // The timer is per thread: the main and render threads can both pace on one limiter, and a shared auto-reset timer wakes only one
    // waiter, leaving the other blocked for good (the window-play hang at frame 4). The wait is also bounded so a lost signal can't hang.
    bool coarse_wait_timer(uint64_t coarse_ns) {
        thread_local HANDLE tl_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!tl_timer) return false;
        LARGE_INTEGER due;
        const LONGLONG units = static_cast<LONGLONG>(coarse_ns / 100);  // 100 ns units; negative = relative
        due.QuadPart = -(units > 0 ? units : 1);
        const uint64_t set_at = FrameLimiter::now_ns();
        if (!SetWaitableTimerEx(tl_timer, &due, 0, nullptr, nullptr, nullptr, 0)) return false;
        WaitForSingleObject(tl_timer, static_cast<DWORD>(coarse_ns / 1000000ull) + 50);
        const uint64_t woke = FrameLimiter::now_ns();
        const uint64_t requested = set_at + coarse_ns;
        const double over = woke > requested ? static_cast<double>(woke - requested) : 0.0;
        overshoot_ema_ns = overshoot_ema_ns * 0.875 + over * 0.125;
        return true;
    }

    // Returns once the monotonic clock has reached target (absolute ns).
    void wait_until(uint64_t target) {
        for (;;) {
            const uint64_t t = FrameLimiter::now_ns();
            if (t >= target) return;
            const uint64_t remaining = target - t;
            if (remaining > margin_ns()) {
                if (timer) {
                    if (coarse_wait_timer(remaining - margin_ns())) continue;
                    // The timer call failed: spin the rest.
                } else {
                    Sleep(1);  // fallback when no high-resolution timer exists; no timeBeginPeriod
                    continue;
                }
            }
            while (FrameLimiter::now_ns() < target) _mm_pause();
            return;
        }
    }
};

FrameLimiter::FrameLimiter() : impl_(new Impl) {
    impl_->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}

FrameLimiter::~FrameLimiter() {
    if (impl_->timer) CloseHandle(impl_->timer);
    delete impl_;
}

void FrameLimiter::set_target_fps(double fps) { impl_->target_fps.store(fps > 0.0 ? fps : 0.0); }

double FrameLimiter::target_fps() const { return impl_->target_fps.load(); }

void FrameLimiter::set_game_fps(double fps) { impl_->game_fps.store(fps > 0.0 ? fps : 0.0, std::memory_order_relaxed); }

double FrameLimiter::game_fps() const { return impl_->game_fps.load(std::memory_order_relaxed); }

double FrameLimiter::effective_fps() const {
    const double t = impl_->target_fps.load(), g = impl_->game_fps.load(std::memory_order_relaxed);
    return g > 0.0 && !(t > 0.0 && t <= g) ? g : t;
}

void FrameLimiter::set_pre_frame_hook(PreFrameFn fn, void* user) {
    impl_->pre_user.store(user, std::memory_order_relaxed);
    impl_->pre_fn.store(fn, std::memory_order_release);
}

void FrameLimiter::run_pre_frame_hook() {
    if (PreFrameFn fn = impl_->pre_fn.load(std::memory_order_acquire)) fn(impl_->pre_user.load(std::memory_order_relaxed));
}

uint64_t FrameLimiter::begin_frame() {
    run_pre_frame_hook();
    return begin_frame_nohook();
}

namespace {
std::atomic<bool> g_unpaced{false};
std::atomic<double> g_interval_hz{0.0};
std::atomic<double> g_cap_floor{0.0};
}
void frame_limiter_set_unpaced(bool on) { g_unpaced.store(on, std::memory_order_relaxed); }
bool frame_limiter_unpaced() { return g_unpaced.load(std::memory_order_relaxed); }
void set_present_interval_hz(double hz) { g_interval_hz.store(hz > 0.0 ? hz : 0.0, std::memory_order_relaxed); }
double present_interval_hz_override() { return g_interval_hz.load(std::memory_order_relaxed); }
void set_frame_cap_floor(double fps) { g_cap_floor.store(fps > 0.0 ? fps : 0.0, std::memory_order_relaxed); }
double frame_cap_floor() { return g_cap_floor.load(std::memory_order_relaxed); }

uint64_t FrameLimiter::begin_frame_nohook() {
    Impl& s = *impl_;
    const uint64_t entry = now_ns();
    const double fps = effective_fps();

    if (!(fps > 0.0) || g_unpaced.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(s.mu);
        s.has_sched = false;
        s.last_wait = 0;
        s.last_late = 0;
        s.record_locked(entry);
        return entry;
    }

    const double period = 1e9 / fps;
    double next;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        const double now = static_cast<double>(entry);
        if (!s.has_sched) {
            next = now;
        } else {
            next = s.sched_ns + period;
            if (now > next + kResyncPeriods * period) next = now;  // stall: restart from now, no catch-up burst
            else if (now > next + kMaxCarryPeriods * period) next = now - kMaxCarryPeriods * period;  // late: carry only a little
        }
        s.sched_ns = next;
        s.has_sched = true;
    }

    const uint64_t target = static_cast<uint64_t>(std::ceil(next));
    const bool had_to_wait = target > entry;
    s.wait_until(target);
    const uint64_t start = now_ns();
    // A wait that ends long after its target means the thread was not scheduled (the timer fired late); a long planned wait means the schedule ran ahead.
    // Either one is a visible freeze, so say which one it was.
    if (start > target + 40000000ull || target > entry + static_cast<uint64_t>(4.0 * period))
        NXLOG_W("limiter", "frame wait: planned %.1f ms, woke %.1f ms after the target (frame period %.2f ms)", static_cast<double>(target > entry ? target - entry : 0) * 1e-6,
                static_cast<double>(start > target ? start - target : 0) * 1e-6, period * 1e-6);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.last_wait = start - entry;
        s.last_late = start > target ? start - target : 0;
        if (s.has_prev) {
            if (had_to_wait) {
                s.late_ring[s.late_pos] = s.last_late;
                s.late_pos = (s.late_pos + 1) % kRing;
                if (s.late_n < kRing) ++s.late_n;
            } else {
                ++s.overran_frames;
            }
        }
        s.record_locked(start);
    }
    return start;
}

void FrameLimiter::reset_schedule() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->has_sched = false;
    impl_->has_prev = false;  // the interval spanning the pause is not measured
}

FrameLimiter::Stats FrameLimiter::stats() const {
    Stats st;
    std::vector<double> ms, late;
    uint64_t overran = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        ms.reserve(impl_->ring_n);
        for (std::size_t i = 0; i < impl_->ring_n; ++i) ms.push_back(static_cast<double>(impl_->ring[i]) * 1e-6);
        for (std::size_t i = 0; i < impl_->late_n; ++i) late.push_back(static_cast<double>(impl_->late_ring[i]) * 1e-6);
        overran = impl_->overran_frames;
    }
    st.waited = late.size();
    st.overran = overran;
    st.overran_fraction = (late.size() + overran) ? static_cast<double>(overran) / static_cast<double>(late.size() + overran) : 0.0;
    if (!late.empty()) {
        std::sort(late.begin(), late.end());
        st.late_max_ms = late.back();
        st.late_over_half_ms = static_cast<double>(late.end() - std::upper_bound(late.begin(), late.end(), 0.5)) / static_cast<double>(late.size());
        st.late_p99_ms = late[std::min(late.size() - 1, static_cast<std::size_t>(0.99 * static_cast<double>(late.size())))];
    }
    const std::size_t n = ms.size();
    if (n == 0) return st;

    double sum = 0.0, mn = ms[0], mx = ms[0];
    for (double d : ms) {
        sum += d;
        mn = std::min(mn, d);
        mx = std::max(mx, d);
    }
    const double mean = sum / static_cast<double>(n);
    double var = 0.0;
    for (double d : ms) var += (d - mean) * (d - mean);

    const std::size_t k = std::min(n - 1, static_cast<std::size_t>(0.99 * static_cast<double>(n)));
    std::nth_element(ms.begin(), ms.begin() + static_cast<std::ptrdiff_t>(k), ms.end());

    st.frames = n;
    st.mean_ms = mean;
    st.min_ms = mn;
    st.max_ms = mx;
    st.p99_ms = ms[k];
    st.stddev_ms = std::sqrt(var / static_cast<double>(n));
    st.fps = mean > 0.0 ? 1000.0 / mean : 0.0;
    return st;
}

void FrameLimiter::reset_stats() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->ring_n = 0;
    impl_->ring_pos = 0;
    impl_->late_n = 0;
    impl_->late_pos = 0;
    impl_->overran_frames = 0;
    impl_->has_prev = false;  // the interval spanning the reset is not measured
}

uint64_t FrameLimiter::last_wait_ns() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_wait;
}

uint64_t FrameLimiter::last_late_ns() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_late;
}

uint64_t FrameLimiter::now_ns() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t q = static_cast<uint64_t>(c.QuadPart);
    // Split so the multiplication cannot overflow: (q % freq) < freq, so the product stays below 1e9 * freq.
    return (q / freq) * 1000000000ull + ((q % freq) * 1000000000ull) / freq;
}

double suggest_vrr_cap(double refresh_hz) {
    const double under = std::max(3.0, refresh_hz * refresh_hz / 3600.0);
    const double cap = refresh_hz - under;
    return cap < 1.0 ? 1.0 : cap;
}

}  // namespace nxrt
