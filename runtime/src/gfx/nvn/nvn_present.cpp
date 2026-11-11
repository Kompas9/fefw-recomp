// SPDX-License-Identifier: GPL-3.0-or-later
// nvnQueuePresentTexture: the window texture is drawn into the Renderer's frame (swapchain or headless image) and presented.
// The shell's FrameLimiter, when given, paces the frame (begin_frame before the image is acquired).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/extras.h"
#include "nxrt/game.h"
#include "nxrt/lowlat.h"
#include "extras/output_pass.h"
#include "extras/framegen.h"
#include "nvn_framegen.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/gauge.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"
#include "nxrt/render_status.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {

namespace {
struct CaptureEnd {
    ~CaptureEnd() {
        trace_dump_targets();
    }
};

// [nvn] shot_frames = "300,1000:2000:50" (NXRT_NVN_SHOT_FRAMES): the presented frames (counted from 1 like the trace) whose picture is written as
// <shot_dir>/<number, 6 digits>.png (default shot_dir "shots"). A range is from:to:step. Lets a run be compared frame by frame against a reference capture.
struct ShotSpec { int64_t from, to, step; };
std::vector<ShotSpec> g_shots;
bool g_shots_read = false;
std::string g_shot_dir;

void read_shots() {
    g_shots_read = true;
    g_shot_dir = nxrt::config_get_str("nvn", "shot_dir", "shots");
    const std::string spec = nxrt::config_get_str("nvn", "shot_frames", "");
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string item = spec.substr(i, j - i);
        long long a = 0, b = 0, c = 1;
        const int n = std::sscanf(item.c_str(), "%lld:%lld:%lld", &a, &b, &c);
        if (n == 1) g_shots.push_back({a, a, 1});
        else if (n >= 2 && c > 0) g_shots.push_back({a, b, c});
        i = j + 1;
    }
}

bool shot_wanted(int64_t n) {
    if (!g_shots_read) read_shots();
    for (const ShotSpec& s : g_shots)
        if (n >= s.from && n <= s.to && (n - s.from) % s.step == 0) return true;
    return false;
}

// The console's display paces a game that steps its logic by a fixed amount per frame: nvnWindowSetPresentInterval(n) shows each frame
// for at least n refreshes of its 60 Hz display. [nvn] present_interval_hz (default: the title's game::Info::present_interval_hz, 0 = off)
// gives the frame limiter that rate / n as the game's rate, independent of the host display's refresh rate and of vsync. Interval 0
// (present at once) sets no rate.
double game_rate(Window* w) {
    static const double cfg_hz = nxrt::config_get_double("nvn", "present_interval_hz", static_cast<double>(nxrt::game::info().present_interval_hz));
    if (!(cfg_hz > 0)) return 0;
    const double set_hz = nxrt::present_interval_hz_override();
    const double hz = set_hz > 0 ? set_hz : cfg_hz;
    int n;
    {
        std::lock_guard<std::mutex> lk(w->lock);
        n = w->present_interval;
    }
    static int logged = -1;
    static double logged_hz = 0;
    if (n != logged || hz != logged_hz) {
        logged = n;
        logged_hz = hz;
        NXLOG_I("nvn", "present interval %d: the game's frame rate is %.2f fps", n, n > 0 ? hz / n : 0.0);
    }
    return n > 0 ? hz / n : 0;
}

// [nvn] frame_csv = <file> (default empty: off): one line per present for frame pacing analysis: the present's number, the time the
// frame left the limiter and the time its present call returned (ms since the first line), the limiter's wait (ms), the game's rate (fps).
struct FrameCsv {
    FILE* f = nullptr;
    uint64_t t0 = 0;
    bool tried = false;
};
FrameCsv g_csv;

void frame_csv_line(int64_t n, uint64_t start_ns, uint64_t end_ns, uint64_t wait_ns, double game_fps, uint64_t present_call_ns, uint64_t sleep_ns) {
    if (!g_csv.tried) {
        g_csv.tried = true;
        const std::string path = nxrt::config_get_str("nvn", "frame_csv", "");
        if (!path.empty() && (g_csv.f = std::fopen(path.c_str(), "w"))) {
            std::fprintf(g_csv.f, "present,start_ms,end_ms,wait_ms,game_fps,present_call_ms,reflex_sleep_ms\n");
            NXLOG_I("nvn", "frame csv %s: its time 0 is the time of this line", path.c_str());
        }
        g_csv.t0 = start_ns;
    }
    if (!g_csv.f) return;
    std::fprintf(g_csv.f, "%lld,%.3f,%.3f,%.3f,%.2f,%.3f,%.3f\n", static_cast<long long>(n), static_cast<double>(start_ns - g_csv.t0) * 1e-6,
                 static_cast<double>(end_ns - g_csv.t0) * 1e-6, static_cast<double>(wait_ns) * 1e-6, game_fps,
                 static_cast<double>(present_call_ns) * 1e-6, static_cast<double>(sleep_ns) * 1e-6);
    if ((n & 255) == 0) std::fflush(g_csv.f);
}
}  // namespace

void present_on_render_thread(Window* w, int index, bool again) {
    NXFT("present");
    nxrt::HitchTimer ht_present(nxrt::kHpRenderPresent);
    CaptureEnd capture_end;
    gfx::Renderer* rend = renderer();
    PresentPass* pp = present_pass();
    if (!w || index < 0 || static_cast<size_t>(index) >= w->textures.size()) return;
    Texture* t = w->textures[index].get();
    if (!t) return;
    if (!again) counters().presents.fetch_add(1, std::memory_order_relaxed);
    // the freeze watchdog counts presented frames (and the test runner its frames); a frame shown again while the game waits on the player counts too
    { NXFT("present.frame_tick"); diag_frame_tick(); }
    if (!rend || !pp) return;
    { NXFT("present.tex"); if (!tex_gpu(t)) return; }
    extras::pace_note_present();
    uint64_t frame_wait = 0;
    double frame_rate = 0;
    if (FrameLimiter* lim = limiter()) {
        frame_rate = game_rate(w);
        if (extras::framegen_requested()) {   // with vsync, frame generation's two presents per frame must fit the display (framegen.h)
            static float hz = 0;
            static uint64_t hz_at = 0;
            const uint64_t now = FrameLimiter::now_ns();
            if (!hz_at || now - hz_at > 1000000000ull) {
                // [dlss] frame_gen_test_refresh_hz: test aid, the refresh rate the pacing rule assumes (the display itself is unchanged)
                static const double test_hz = nxrt::config_get_double("dlss", "frame_gen_test_refresh_hz", 0.0);
                hz = test_hz > 0 ? static_cast<float>(test_hz) : platform::display_refresh_hz();
                hz_at = now;
            }
            frame_rate = extras::framegen_pace(frame_rate, rend->present_mode() == gfx::PresentMode::Fifo, hz);
        }
        lim->set_game_fps(frame_rate);
        // the pre-frame hook (present mode changes) always runs here; the wait runs here only when no frame boundary has paced this present (S18)
        { NXFT("present.hook"); lim->run_pre_frame_hook(); }
        if (!(extras::pace_in_main() && extras::pace_boundary_consume())) {
            NXFT("limiter");
            nxrt::HitchTimer ht(nxrt::kHpRenderLimiter);
            lim->begin_frame_nohook();
            frame_wait = lim->last_wait_ns();
        }
    }
    const uint64_t frame_start = FrameLimiter::now_ns();
    if (!again) {
        const int64_t n = static_cast<int64_t>(counters().presents.load());
        if (shot_wanted(n)) {
            char name[64];
            std::snprintf(name, sizeof name, "/%06lld.png", static_cast<long long>(n));
            rend->request_screenshot(g_shot_dir + name);
        }
    }
    gfx::FrameInfo fi;
    { NXFT("present.acquire"); nxrt::HitchTimer ht(nxrt::kHpRenderAcquire); if (!rend->begin_frame(&fi)) return; }
    ft::frame();
    Frame f;
    f.cb = fi.cmd;
    f.serial = current_serial();
    VkImageView src = VK_NULL_HANDLE;
    {
        NXFT("present.transition");
        tex_transition(t, f, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        src = tex_present_view(t);
    }
    NXFT("present.record");
    const VkExtent3D e = tex_image_extent(t, 0);     // a scaled target is presented whole (its crop in image texels)
    int32_t crop[4] = {w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
    if (const float sc = tex_scale(t); sc != 1.0f)
        for (int32_t& c : crop) c = static_cast<int32_t>(std::lround(static_cast<float>(c) * sc));
    pp->set_flip_y(window_origin_mode() == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);   // the game runs with the lower-left origin (mode 0)
    // HDR scene / HDR swapchain / FSR 1 or NIS / picture controls go through the output pass (S14); everything else is the plain present pass of the SDR path
    const extras::HdrState hs = extras::hdr_state();
    const extras::PictureLive pic = extras::picture_live();
    extras::OutputPass* op = output_pass();
    auto draw_picture = [&](VkImageView from, VkExtent2D from_extent, const int32_t* from_crop, VkImageView to) {   // also frame generation's HUD-less colour (nvn_framegen.h)
        if (op && (hs.scene_hdr || hs.output != extras::OutputKind::Sdr8 || extras::upscale_mode() == extras::UpscaleMode::Fsr1 ||
                   extras::upscale_mode() == extras::UpscaleMode::Nis || pic.active)) {
            extras::OutputParams prm;
            prm.src = from;
            prm.src_extent = from_extent;
            for (int i = 0; i < 4; ++i) prm.crop[i] = from_crop[i];
            prm.keep_aspect = keep_aspect();
            prm.flip_y = window_origin_mode() == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
            prm.src_linear = hs.scene_hdr;
            prm.out = hs.output;
            prm.paper_white_nits = hs.paper_white_nits;
            prm.peak_nits = hs.peak_nits;
            prm.fsr1 = extras::upscale_mode() == extras::UpscaleMode::Fsr1;
            prm.nis = extras::upscale_mode() == extras::UpscaleMode::Nis;
            prm.sharpness = extras::upscale_sharpness();
            prm.picture = pic.active;
            prm.pic[0] = pic.contrast;
            prm.pic[1] = pic.saturation;
            prm.pic[2] = pic.black;
            prm.pic[3] = pic.gamma;
            op->record(fi.cmd, to, rend->format(), fi.extent, prm);
        } else {
            pp->record(fi.cmd, to, fi.extent, from, from_extent, from_crop, keep_aspect());
        }
    };
    draw_picture(src, VkExtent2D{e.width, e.height}, crop, fi.view);
    fg_hudless_present(w, t, f, fi.extent, rend->format(), again, crop, draw_picture);
    // the frame's own submissions go first: in the present's submission ([nvn] submit_batch = frame), or on their own before it
    static thread_local std::vector<VkCommandBuffer> cmds;
    gfx::Renderer::SubmitPrefix pre;
    const bool merged = take_batch_for_present(&cmds, &pre);
    extras::framegen_present_begin(fi.cmd, fi.extent, VkExtent2D{e.width, e.height}, crop, keep_aspect(), again);
    render_status::set_screen(static_cast<int>(fi.extent.width), static_cast<int>(fi.extent.height));
    const uint64_t call_start = FrameLimiter::now_ns();
    { NXFT("present.end"); nxrt::HitchTimer ht(nxrt::kHpRenderPresentEnd); rend->end_frame(merged ? &pre : nullptr); }
    const uint64_t call_ns = FrameLimiter::now_ns() - call_start;
    extras::framegen_present_end();
    if (merged) present_batch_done(pre);
    if (!again) frame_csv_line(static_cast<int64_t>(counters().presents.load()), frame_start, FrameLimiter::now_ns(), frame_wait, frame_rate, call_ns, extras::framegen_take_sleep_ns());
}

}  // namespace nxrt::nvn::be::gpu
