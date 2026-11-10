// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay (declarations in include/nxrt/overlay.h). ImGui and its Vulkan backend are pinned in cmake/gfx_deps.cmake.
#include "nxrt/overlay.h"

#include <windows.h>

#include <atomic>
#include <mutex>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/menu_i18n.h"
#include "nxrt/nvn.h"
#include "nxrt/platform.h"
#include "nxrt/services_api.h"
#include "extras/framegen.h"
#include "extras/overlay_composite.h"
#include "fps_stats.h"

namespace nxrt::gfx {
namespace {
constexpr const char* kTag = "overlay";

ImGuiKey map_key(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return static_cast<ImGuiKey>(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc == SDL_SCANCODE_0) return ImGuiKey_0;
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
    switch (sc) {
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
        case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

const double kFpsChoices[] = {0, 30, 60, 90, 120, 144, 165, 240, 360};

// ---- on-screen keyboard: text helpers and the key grid ----
// UTF-8 to code points (a bad byte becomes U+FFFD) and back. The applet's max_len counts characters, so the text is kept as code points.
std::u32string utf8_to_u32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) { out.push_back(0xFFFD); ++i; continue; }
        char32_t v = n == 1 ? c : (c & (0x7Fu >> n));
        bool ok = true;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            v = (v << 6) | (cc & 0x3Fu);
        }
        out.push_back(ok ? v : 0xFFFD);
        i += ok ? n : 1;
    }
    return out;
}

std::string u32_to_utf8(const std::u32string& s, size_t from = 0, size_t to = std::u32string::npos) {
    std::string out;
    to = std::min(to, s.size());
    auto put = [&](uint32_t b) { out.push_back(static_cast<char>(b)); };
    for (size_t i = from; i < to; ++i) {
        const uint32_t c = s[i];
        if (c < 0x80) put(c);
        else if (c < 0x800) { put(0xC0 | (c >> 6)); put(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { put(0xE0 | (c >> 12)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F)); }
        else { put(0xF0 | (c >> 18)); put(0x80 | ((c >> 12) & 0x3F)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F)); }
    }
    return out;
}

// Typographic punctuation the overlay's font has no glyph for, as plain ASCII, for the game's header and guide text (Pokopia's name question
// is "What\u2019s your name?", drawn as "What?s" otherwise).
std::string fold_punct(const std::string& s) {
    std::u32string in = utf8_to_u32(s), out;
    for (char32_t c : in) {
        switch (c) {
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += U'\''; break;
            case 0x201C: case 0x201D: case 0x201E: case 0x2033: out += U'"'; break;
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: out += U'-'; break;
            case 0x2026: out += U"..."; break;
            case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x3000: out += U' '; break;
            default: out += c;
        }
    }
    return u32_to_utf8(out);
}

// The grid: rows of 10 column units, a key spans `w` of them. Shift turns letters upper case and punctuation into its second character (alt).
enum class KbOp : uint8_t { Char, Shift, Space, Back, Ok, Cancel };
struct KbKey { char32_t ch; char32_t alt; int w; KbOp op; };
constexpr int kKbUnits = 10;
const std::vector<std::vector<KbKey>>& kb_rows() {
    static const std::vector<std::vector<KbKey>> rows = [] {
        std::vector<std::vector<KbKey>> r;
        auto row = [&](const char* chars, const char* alts) {
            std::vector<KbKey> v;
            for (int i = 0; chars[i]; ++i) v.push_back({static_cast<char32_t>(chars[i]), static_cast<char32_t>(alts[i]), 1, KbOp::Char});
            r.push_back(v);
        };
        row("1234567890", "1234567890");
        row("qwertyuiop", "QWERTYUIOP");
        row("asdfghjkl-", "ASDFGHJKL_");
        row("zxcvbnm,.?", "ZXCVBNM;:/");
        r.push_back({{0, 0, 2, KbOp::Shift}, {U' ', U' ', 4, KbOp::Space}, {U'!', U'@', 1, KbOp::Char}, {U'\'', U'"', 1, KbOp::Char}, {0, 0, 2, KbOp::Back}});
        r.push_back({{0, 0, 5, KbOp::Cancel}, {0, 0, 5, KbOp::Ok}});
        return r;
    }();
    return rows;
}
// The key of a row that covers column unit u (past the end: the last key); *start = its first unit.
int kb_key_at(const std::vector<KbKey>& row, int u, int* start) {
    int x = 0;
    for (int i = 0; i < static_cast<int>(row.size()); ++i) {
        if (u < x + row[i].w || i + 1 == static_cast<int>(row.size())) {
            if (start) *start = x;
            return i;
        }
        x += row[i].w;
    }
    return 0;
}

// Pad directions the keyboard moves on: the d-pad, or the left stick past half way (set_keyboard_pad adds the stick bits).
constexpr uint64_t kKbDirBits[4] = {
    hid::btn(hid::kBtnUp) | hid::btn(hid::kBtnStickLUp), hid::btn(hid::kBtnDown) | hid::btn(hid::kBtnStickLDown),
    hid::btn(hid::kBtnLeft) | hid::btn(hid::kBtnStickLLeft), hid::btn(hid::kBtnRight) | hid::btn(hid::kBtnStickLRight)};
constexpr double kKbRepeatDelay = 0.40, kKbRepeatEvery = 0.07;   // seconds: a held direction or B repeats

// Drops pressed keys and mouse buttons so nothing stays latched in ImGui (the caller holds the context's mutex).
void clear_imgui_input(ImGuiContext* ctx) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    ImGui::SetCurrentContext(prev);
}
}  // namespace

namespace {
std::atomic<int> g_shader_backlog{0};
}
void set_shader_backlog(int pending) { g_shader_backlog.store(pending < 0 ? 0 : pending, std::memory_order_relaxed); }
int shader_backlog() { return g_shader_backlog.load(std::memory_order_relaxed); }

struct Overlay::Impl {
    Context ctx;
    ImGuiContext* imgui = nullptr;
    ImGuiStyle base_style;            // the style at scale 1; style = base_style scaled by ui_scale (update_scale)
    float ui_scale = 0.f;             // 0: not applied yet
    bool backend_ready = false;
    std::atomic<bool> visible{false};
    std::mutex mu;                    // guards the ImGui context: events arrive on the game's main thread, drawing happens on the presenting thread
    int toggle_sc = SDL_SCANCODE_F1;
    bool restore_relative = false;
    std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
    uint64_t drawn = 0;

    struct Panel { std::string title; std::function<void()> fn; };
    std::vector<Panel> panels;
    std::function<void()> menu;           // set_menu: the game's own menu replaces the Diagnostics window and the panels
    std::atomic<bool> want_hide{false};   // request_hide: applied at the start of the next draw (the menu runs with mu held)

    // The pad for the menu (set_menu_pad, on the thread that polls the pads)
    uint64_t menu_chord = 0;              // [gfx] overlay_pad as npad bits; 0 = no chord
    uint64_t menu_prev = 0;               // the buttons at the last poll
    uint64_t menu_mask = 0;               // held back from the game until released (the press that opened or closed the menu)
    std::atomic<bool> hold_on_hide{false};   // the overlay was hidden: what is held at the next poll stays away from the game
    uint64_t menu_stick = 0;              // d-pad bits the left stick pressed at the last poll

    // menu state applied by apply_pending()
    bool want_mode = false;
    PresentMode new_mode = PresentMode::Fifo;
    bool want_fullscreen = false;
    platform::FullscreenMode new_fullscreen = platform::FullscreenMode::Windowed;

    float frame_ms[120] = {};
    int frame_ms_pos = 0;
    int fps_choice = 0;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    bool hdr = false;                 // HDR swapchain: the overlay is drawn into an RGBA8 target and composited (draw)
    extras::OverlayComposite composite;

    // Corner frame-rate counter. set_counter() stores the settings from any thread; the rest is used by the presenting thread only.
    std::atomic<bool> ptr_show{false}, ptr_in_shots{true};
    std::atomic<float> ptr_x{0}, ptr_y{0};
    std::atomic<bool> counter_show{false};
    std::atomic<int> counter_corner{0};
    std::atomic<int> counter_detail{1};
    std::atomic<bool> counter_in_shots{false};
    bool counter_was_on = false;
    bool counter_have_prev = false;
    std::chrono::steady_clock::time_point counter_prev{};
    FpsStats stats;
    double text_s = -1e9;                 // time of the last text refresh, seconds (far past: refresh at once)
    FpsStats::Snapshot snap;              // the snapshot taken at text_s
    std::string counter_lines[5];
    int counter_n = 0;

    void build_ui();
    void update_scale(const VkExtent2D& extent);
    void refresh_counter_text(double now_s);
    void draw_counter(const FrameInfo& f, double now_s);
    // The "Preparing shaders" note: shown once the backlog has stood for a second and a half, gone a second after it is empty.
    bool note_enabled = true;
    bool note_read = false;
    double note_since_s = -1.0, note_empty_s = -1.0;
    int note_peak = 0;
    bool note_visible(double now_s);
    void draw_note(const FrameInfo& f);

    // The on-screen keyboard (services_api.h swkbdui). kb_mu guards the question and its text: key events edit it on the game's main thread,
    // the pad and the mouse in draw() on the presenting thread. Lock order: mu (ImGui), then kb_mu, then the swkbdui question's own lock.
    std::mutex kb_mu;
    std::atomic<bool> kb_open{false};
    std::atomic<bool> kb_by_label{false};
    uint64_t kb_id = 0;                   // the question on screen, 0 = none
    uint64_t kb_answered = 0;             // the last question this keyboard answered (ids only grow): never reopened
    services::swkbdui::Request kb_req;
    std::u32string kb_text;               // code points
    size_t kb_caret = 0;
    int kb_shift = 0;                     // 0 off, 1 the next character only, 2 locked
    int kb_row = 1, kb_unit = 0;          // the pad's key: row, column unit
    double kb_rep[5] = {};                // the next repeat of up, down, left, right, B (seconds; DBL_MAX: not held since a press)
    // The pad (set_keyboard_pad, on the thread that polls it): what is held, and presses not yet taken by draw().
    std::atomic<uint64_t> pad_held{0}, pad_new{0};
    uint64_t pad_prev = 0, pad_mask = 0;  // set_keyboard_pad only

    bool kb_poll();                       // follows swkbdui::pending(): opens a new question, drops one answered elsewhere; true while open
    bool kb_finish(bool ok);              // kb_mu held: answers the question; false when there is none or OK is not allowed yet (min_len)
    void kb_insert(const std::u32string& t);   // kb_mu held: at the caret, control characters dropped, up to max_len
    void kb_backspace();                  // kb_mu held
    bool kb_press(const KbKey& k);        // kb_mu held: an on-screen key; true when it answered the question
    bool kb_key_event(const input::Event& e);   // a physical key or text; kb_mu and mu not held
    void draw_keyboard(double now_s);     // mu held, inside an ImGui frame
};

void Overlay::Impl::build_ui() {
    if (menu) {
        menu();
        return;
    }
    Renderer& r = *ctx.renderer;
    // First-use layout, in units of the UI scale (update_scale): the panels (the settings menu) sit at the top left, the diagnostics window
    // at the top right and folded up. The player moves them; nothing is remembered between runs.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float diag_w = std::min(420.f * k, std::max(200.f, disp.x - 32.f * k));
    ImGui::SetNextWindowPos(ImVec2(std::max(16.f * k, disp.x - diag_w - 16.f * k), 16.f * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(diag_w, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Diagnostics")) {
        const GpuInfo& g = ctx.device->gpu();
        ImGui::Text("GPU: %s", g.name.c_str());
        ImGui::Text("Vulkan 1.3, driver 0x%08x, validation %s%s", g.driver_version, ctx.device->validation_enabled() ? "on" : "off",
                    ctx.device->headless() ? ", headless" : "");
        const VkExtent2D e = r.extent();
        ImGui::Text("Target: %ux%u, %u images, %u frames in flight", e.width, e.height, r.image_count(), r.frames_in_flight());

        ImGui::SeparatorText("Presentation");
        const PresentMode modes[] = {PresentMode::Fifo, PresentMode::FifoRelaxed, PresentMode::Mailbox, PresentMode::Immediate};
        int cur = static_cast<int>(r.requested_present_mode());
        if (ImGui::BeginCombo("Present mode", present_mode_name(modes[cur]))) {
            for (int i = 0; i < 4; ++i) {
                const bool ok = r.present_mode_supported(modes[i]);
                ImGui::BeginDisabled(!ok);
                if (ImGui::Selectable(present_mode_name(modes[i]), i == cur)) { want_mode = true; new_mode = modes[i]; }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (r.present_mode() != r.requested_present_mode()) ImGui::TextDisabled("in use: %s (requested mode unavailable)", present_mode_name(r.present_mode()));

        if (ctx.limiter) {
            char label[32];
            const double t = ctx.limiter->target_fps();
            std::snprintf(label, sizeof label, t > 0 ? "%.0f fps" : "unlimited", t);
            if (ImGui::BeginCombo("Frame cap", label)) {
                for (double v : kFpsChoices) {
                    std::snprintf(label, sizeof label, v > 0 ? "%.0f fps" : "unlimited", v);
                    if (ImGui::Selectable(label, v == t)) ctx.limiter->set_target_fps(v);
                }
                ImGui::EndCombo();
            }
            const float hz = platform::has_window() ? platform::display_refresh_hz() : 0.f;
            if (hz > 0) {
                ImGui::TextDisabled("display %.2f Hz; VRR-friendly cap %.1f fps", hz, suggest_vrr_cap(hz));
                if (ImGui::SmallButton("Use VRR cap")) ctx.limiter->set_target_fps(suggest_vrr_cap(hz));
            }
        }

        if (ctx.has_window && platform::has_window()) {
            ImGui::SeparatorText("Window");
            const auto fm = platform::fullscreen_mode();
            if (ImGui::RadioButton("Windowed", fm == platform::FullscreenMode::Windowed)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Windowed; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Borderless", fm == platform::FullscreenMode::Borderless)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Borderless; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Exclusive", fm == platform::FullscreenMode::Exclusive)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Exclusive; }
            bool rel = platform::relative_mouse();
            if (ImGui::Checkbox("Raw mouse (relative mode)", &rel)) platform::set_relative_mouse(rel);
            ImGui::SameLine();
            ImGui::TextDisabled(platform::relative_mouse_is_raw() ? "(unaccelerated)" : "(system scaling!)");
        }

        ImGui::SeparatorText("Timing");
        const RendererStats rs = r.stats();
        const float dt_ms = ImGui::GetIO().DeltaTime * 1000.f;
        frame_ms[frame_ms_pos] = dt_ms;
        frame_ms_pos = (frame_ms_pos + 1) % 120;
        ImGui::Text("CPU frame %.2f ms (%.0f fps)   GPU %.2f ms (avg %.2f)", dt_ms, dt_ms > 0 ? 1000.f / dt_ms : 0.f, rs.gpu_ms_last, rs.gpu_ms_avg);
        ImGui::PlotLines("##ft", frame_ms, 120, frame_ms_pos, nullptr, 0.f, 33.f, ImVec2(0, 48));
        ImGui::Text("frames %llu, swapchains %llu, out-of-date %llu, screenshots %llu", static_cast<unsigned long long>(rs.frames),
                    static_cast<unsigned long long>(rs.swapchain_creates), static_cast<unsigned long long>(rs.out_of_date),
                    static_cast<unsigned long long>(rs.screenshots));
        const ValidationStats vs = ctx.device->validation_stats();
        ImGui::Text("validation: %llu errors, %llu warnings", static_cast<unsigned long long>(vs.errors), static_cast<unsigned long long>(vs.warnings));

        ImGui::SeparatorText("NVN front end");
        ImGui::Text("%zu entry points, %zu implemented; stub calls %llu (%zu distinct)", nvn::entry_count(), nvn::implemented_count(),
                    static_cast<unsigned long long>(nvn::stub_calls_total()), nvn::stubs_called_distinct());
        if (ImGui::TreeNode("Most called stubs")) {
            auto calls = nvn::called_entries();
            int shown = 0;
            for (auto& c : calls) {
                if (c.implemented) continue;
                ImGui::Text("%8llu  %s", static_cast<unsigned long long>(c.calls), c.entry->name);
                if (++shown >= 12) break;
            }
            if (!shown) ImGui::TextDisabled("none called");
            ImGui::TreePop();
        }
    }
    ImGui::End();
    for (auto& p : panels) {
        ImGui::SetNextWindowPos(ImVec2(16.f * k, 16.f * k), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(std::min(760.f * k, std::max(240.f, disp.x - 32.f * k)), std::min(680.f * k, std::max(200.f, disp.y - 32.f * k))),
                                 ImGuiCond_FirstUseEver);
        if (ImGui::Begin(p.title.c_str())) p.fn();
        ImGui::End();
    }
}

// DPI: the UI is laid out for a 1080 pixel high frame at 16 px text and grows with the frame (4K: twice the size; a Windows display scaling
// of 150% shows up as more pixels in the same window, so it is covered too). Never below 1: a 720p window keeps the 16 px text.
void Overlay::Impl::update_scale(const VkExtent2D& extent) {
    const float want = std::round(std::clamp(static_cast<float>(extent.height) / 1080.f, 1.0f, 3.0f) * 20.f) / 20.f;   // steps of 0.05
    if (want == ui_scale) return;
    ui_scale = want;
    ImGuiStyle& style = ImGui::GetStyle();
    style = base_style;
    style.ScaleAllSizes(want);
    style.FontScaleDpi = want;
}

std::unique_ptr<Overlay> Overlay::create(const Context& ctx, std::string* error) {
    if (!ctx.device || !ctx.renderer) {
        if (error) *error = "overlay needs a device and a renderer";
        return nullptr;
    }
    std::unique_ptr<Overlay> o(new Overlay());
    o->impl_ = new Impl();
    Impl& s = *o->impl_;
    s.ctx = ctx;
    s.color_format = ctx.renderer->format();
    if (ctx.renderer->output_kind() != extras::OutputKind::Sdr8) {
        // HDR swapchain: ImGui draws into an RGBA8 target that the composite maps onto the swapchain (see draw)
        if (s.composite.init(*ctx.device)) {
            s.hdr = true;
            s.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            NXLOG_W(kTag, "HDR overlay composite failed to initialise: the overlay is drawn straight into the HDR swapchain (colours will look wrong)");
        }
    }
    const std::string key = config_get_str("gfx", "overlay_key", "F1");
    if (const SDL_Scancode sc = SDL_GetScancodeFromName(key.c_str()); sc != SDL_SCANCODE_UNKNOWN) s.toggle_sc = sc;
    else NXLOG_W(kTag, "unknown [gfx] overlay_key '%s': using F1", key.c_str());
    {   // [gfx] overlay_pad: Switch buttons joined by '+' (after the player's mapping), "none" = no chord
        std::string chord = config_get_str("gfx", "overlay_pad", "minus+plus");
        for (char& c : chord) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static const std::pair<const char*, hid::NpadButtonBit> kNames[] = {
            {"a", hid::kBtnA},       {"b", hid::kBtnB},       {"x", hid::kBtnX},         {"y", hid::kBtnY},         {"l", hid::kBtnL},
            {"r", hid::kBtnR},       {"zl", hid::kBtnZL},     {"zr", hid::kBtnZR},       {"plus", hid::kBtnPlus},   {"minus", hid::kBtnMinus},
            {"up", hid::kBtnUp},     {"down", hid::kBtnDown}, {"left", hid::kBtnLeft},   {"right", hid::kBtnRight}, {"lstick", hid::kBtnStickL},
            {"rstick", hid::kBtnStickR}};
        uint64_t bits = 0;
        bool ok = true;
        size_t from = 0;
        while (chord != "none" && !chord.empty() && from <= chord.size()) {
            size_t plus = chord.find('+', from);
            if (plus == std::string::npos) plus = chord.size();
            const std::string name = chord.substr(from, plus - from);
            bool found = false;
            for (const auto& kv : kNames)
                if (name == kv.first) { bits |= hid::btn(kv.second); found = true; }
            ok = ok && found;
            from = plus + 1;
        }
        if (!ok) {
            NXLOG_W(kTag, "unknown [gfx] overlay_pad '%s': using minus+plus", chord.c_str());
            bits = hid::btn(hid::kBtnMinus) | hid::btn(hid::kBtnPlus);
        }
        s.menu_chord = bits;
    }

    IMGUI_CHECKVERSION();
    s.imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // no imgui.ini next to the exe
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;      // set_menu_pad feeds it
    ImGui::StyleColorsDark();
    // UI font: a CJK-capable face for [gfx] ui_language packs, else the scalable built-in default. ImGui 1.92's
    // dynamic glyphs load only what the UI shows, so a system CJK font costs nothing until it is drawn.
    {
        std::string font = config_get_str("gfx", "ui_font", "");
        std::string lang = config_get_str("gfx", "ui_language", "auto");
        const bool wants_cjk = !(lang.empty() || lang == "auto" || lang == "en" || lang == "en-US") || !font.empty();
        std::string chosen;
        if (wants_cjk) {
            const auto exists = [](const std::string& p) {
                WIN32_FILE_ATTRIBUTE_DATA a;
                return p.size() < MAX_PATH && GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a) &&
                       !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            };
            if (!font.empty() && exists(font)) chosen = font;
            if (chosen.empty()) {
                char buf[MAX_PATH];
                if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
                    std::string dir(buf);
                    const size_t slash = dir.find_last_of("\\/");
                    if (slash != std::string::npos) {
                        dir.resize(slash);
                        static const char* const kLocal[] = {"fonts\\ui.ttf", "fonts\\font.ttf"};
                        for (const char* c : kLocal)
                            if (exists(dir + "\\" + c)) { chosen = dir + "\\" + c; break; }
                    }
                }
            }
            if (chosen.empty()) {
                static const char* const kSystem[] = {
                    "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\Deng.ttf",
                };
                for (const char* c : kSystem)
                    if (exists(c)) { chosen = c; break; }
            }
        }
        if (!chosen.empty() && io.Fonts->AddFontFromFileTTF(chosen.c_str(), 0.0f))
            NXLOG_I(kTag, "UI font: %s ([gfx] ui_language %s)", chosen.c_str(), lang.c_str());
        else {
            if (wants_cjk) NXLOG_W(kTag, "no CJK font found ([gfx] ui_font '%s'): using the built-in default", font.c_str());
            io.Fonts->AddFontDefaultVector();   // the scalable default font: stays sharp at every UI scale (update_scale)
        }
    }
    ImGui::GetStyle().FontSizeBase = 16.0f;
    s.base_style = ImGui::GetStyle();
    const VkExtent2D e = ctx.renderer->extent();
    io.DisplaySize = ImVec2(static_cast<float>(e.width), static_cast<float>(e.height));

    VkPipelineRenderingCreateInfoKHR rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &s.color_format;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = ctx.device->instance();
    ii.PhysicalDevice = ctx.device->physical();
    ii.Device = ctx.device->device();
    ii.QueueFamily = ctx.device->queue_family();
    ii.Queue = ctx.device->queue();
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = std::max(2u, ctx.renderer->image_count());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = rci;
    ii.CheckVkResultFn = [](VkResult r) {
        static bool logged = false;
        if (r != VK_SUCCESS && !logged) {
            logged = true;
            NXLOG_E(kTag, "ImGui Vulkan call failed (%d); incomplete overlay draws are skipped", static_cast<int>(r));
        }
    };
    if (!ImGui_ImplVulkan_Init(&ii)) {
        if (error) *error = "ImGui_ImplVulkan_Init failed";
        ImGui::DestroyContext(s.imgui);
        s.imgui = nullptr;
        return nullptr;
    }
    s.backend_ready = true;
    NXLOG_I(kTag, "overlay ready (toggle key %s)", SDL_GetScancodeName(static_cast<SDL_Scancode>(s.toggle_sc)));
    return o;
}

Overlay::~Overlay() {
    if (!impl_) return;
    if (impl_->ctx.device) impl_->ctx.device->wait_idle();
    impl_->composite.shutdown();
    if (impl_->backend_ready) ImGui_ImplVulkan_Shutdown();
    if (impl_->imgui) ImGui::DestroyContext(impl_->imgui);
    delete impl_;
}

void Overlay::set_visible(bool v) {
    Impl& s = *impl_;
    if (s.visible == v) return;
    s.visible = v;
    if (v) {
        s.restore_relative = platform::has_window() && platform::relative_mouse();
        if (s.restore_relative) platform::set_relative_mouse(false);
    } else {
        s.hold_on_hide.store(true);
        if (s.restore_relative && platform::has_window()) platform::set_relative_mouse(true);
        s.restore_relative = false;
        std::lock_guard<std::mutex> lk(s.mu);
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s.imgui);
        ImGuiIO& io = ImGui::GetIO();   // drop pressed keys and buttons so nothing stays latched
        io.ClearInputKeys();
        io.ClearInputMouse();
        ImGui::SetCurrentContext(prev);
    }
    NXLOG_I(kTag, "overlay %s", v ? "shown" : "hidden");
}

bool Overlay::visible() const { return impl_->visible; }
void Overlay::toggle() { set_visible(!impl_->visible); }
int Overlay::toggle_scancode() const { return impl_->toggle_sc; }
void Overlay::set_toggle_scancode(int sc) { impl_->toggle_sc = sc; }
uint64_t Overlay::frames_drawn() const { return impl_->drawn; }

void Overlay::set_pointer(bool show, float x, float y, bool in_screenshots) {
    Impl& s = *impl_;
    s.ptr_x.store(x, std::memory_order_relaxed);
    s.ptr_y.store(y, std::memory_order_relaxed);
    s.ptr_in_shots.store(in_screenshots, std::memory_order_relaxed);
    s.ptr_show.store(show, std::memory_order_relaxed);
}

void Overlay::set_counter(const CounterConfig& c) {
    Impl& s = *impl_;
    s.counter_corner.store(std::clamp(c.corner, 0, 3), std::memory_order_relaxed);
    s.counter_detail.store(std::clamp(c.detail, 0, 2), std::memory_order_relaxed);
    s.counter_in_shots.store(c.in_screenshots, std::memory_order_relaxed);
    s.counter_show.store(c.show, std::memory_order_relaxed);   // last: the other settings are in place when it turns on
}

namespace {
bool imgui_wants(std::mutex& mu, ImGuiContext* ctx, bool mouse, bool keyboard) {
    std::lock_guard<std::mutex> lk(mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    const ImGuiIO& io = ImGui::GetIO();
    const bool w = (mouse && io.WantCaptureMouse) || (keyboard && io.WantCaptureKeyboard);
    ImGui::SetCurrentContext(prev);
    return w;
}
}  // namespace

bool Overlay::Impl::kb_poll() {
    namespace sk = services::swkbdui;
    const uint64_t id = sk::pending(nullptr);
    {
        std::lock_guard<std::mutex> lk(kb_mu);
        if (id == kb_id || (id && id <= kb_answered)) return kb_id != 0;
    }
    sk::Request r;
    uint64_t now_id = sk::pending(&r);
    std::lock_guard<std::mutex> lk(kb_mu);
    if (now_id && now_id <= kb_answered) now_id = 0;    // answered here meanwhile
    if (now_id == kb_id) return kb_id != 0;
    if (kb_id && !now_id) NXLOG_I(kTag, "on-screen keyboard: question %llu closed elsewhere", static_cast<unsigned long long>(kb_id));
    kb_id = now_id;
    if (now_id) {
        kb_req = std::move(r);
        for (std::string* t : {&kb_req.title, &kb_req.header, &kb_req.sub, &kb_req.guide, &kb_req.ok_text}) *t = fold_punct(*t);
        kb_text = utf8_to_u32(kb_req.initial);
        if (kb_req.max_len && kb_text.size() > kb_req.max_len) kb_text.resize(kb_req.max_len);
        kb_caret = kb_text.size();
        kb_shift = 0;
        kb_row = 1;
        kb_unit = 0;
        for (double& t : kb_rep) t = DBL_MAX;
        pad_new.store(0);
        NXLOG_I(kTag, "on-screen keyboard: question %llu (max %u characters%s)", static_cast<unsigned long long>(now_id), kb_req.max_len,
                kb_req.password ? ", password" : "");
    }
    kb_open = now_id != 0;
    return kb_open;
}

bool Overlay::Impl::kb_finish(bool ok) {
    if (!kb_id || (ok && kb_text.size() < kb_req.min_len)) return false;
    const uint64_t id = kb_id;
    kb_answered = id;
    kb_id = 0;
    kb_open = false;
    NXLOG_I(kTag, "on-screen keyboard: question %llu %s", static_cast<unsigned long long>(id), ok ? "answered" : "cancelled");
    services::swkbdui::answer(id, ok, ok ? u32_to_utf8(kb_text) : std::string());
    return true;
}

void Overlay::Impl::kb_insert(const std::u32string& t) {
    for (const char32_t c : t) {
        if (c < 0x20 || c == 0x7F) continue;
        if (kb_req.max_len && kb_text.size() >= kb_req.max_len) break;
        kb_text.insert(kb_caret++, 1, c);
    }
}

void Overlay::Impl::kb_backspace() {
    if (kb_caret == 0) return;
    kb_text.erase(--kb_caret, 1);
}

bool Overlay::Impl::kb_press(const KbKey& k) {
    switch (k.op) {
        case KbOp::Char:
            kb_insert(std::u32string(1, kb_shift ? k.alt : k.ch));
            if (kb_shift == 1) kb_shift = 0;
            return false;
        case KbOp::Shift: kb_shift = (kb_shift + 1) % 3; return false;
        case KbOp::Space: kb_insert(U" "); return false;
        case KbOp::Back: kb_backspace(); return false;
        case KbOp::Ok: return kb_finish(true);
        case KbOp::Cancel: return kb_finish(false);
    }
    return false;
}

// The physical keyboard while the on-screen keyboard is up: typing goes straight into its text (SDL text events), Enter answers, Escape
// cancels. Every key is consumed, the game sees none.
bool Overlay::Impl::kb_key_event(const input::Event& e) {
    bool closed = false;
    {
        std::lock_guard<std::mutex> lk(kb_mu);
        if (e.type == input::EventType::Text) {
            kb_insert(utf8_to_u32(e.u.text.utf8));
        } else if (e.type == input::EventType::KeyDown && kb_id) {
            const bool rep = (e.flags & input::kFlagRepeat) != 0;
            switch (e.u.key.scancode) {
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER: if (!rep) closed = kb_finish(true); break;
                case SDL_SCANCODE_ESCAPE: if (!rep) closed = kb_finish(false); break;
                case SDL_SCANCODE_BACKSPACE: kb_backspace(); break;
                case SDL_SCANCODE_DELETE: if (kb_caret < kb_text.size()) kb_text.erase(kb_caret, 1); break;
                case SDL_SCANCODE_LEFT: if (kb_caret > 0) --kb_caret; break;
                case SDL_SCANCODE_RIGHT: if (kb_caret < kb_text.size()) ++kb_caret; break;
                case SDL_SCANCODE_HOME: kb_caret = 0; break;
                case SDL_SCANCODE_END: kb_caret = kb_text.size(); break;
                case SDL_SCANCODE_V:
                    if (e.mods & SDL_KMOD_CTRL) {
                        if (char* clip = SDL_GetClipboardText()) {
                            kb_insert(utf8_to_u32(clip));
                            SDL_free(clip);
                        }
                    }
                    break;
                default: break;
            }
        }
    }
    if (closed) {
        std::lock_guard<std::mutex> lk(mu);
        clear_imgui_input(imgui);
    }
    return true;
}

void Overlay::Impl::draw_keyboard(double now_s) {
    std::lock_guard<std::mutex> lk(kb_mu);
    if (!kb_id) return;
    const auto& rows = kb_rows();
    const int nrows = static_cast<int>(rows.size());
    bool closed = false;

    // The pad: presses since the last frame; a direction or B held past the delay repeats.
    const uint64_t held = pad_held.load(), fresh = pad_new.exchange(0);
    auto fired = [&](int slot, uint64_t bits) {
        bool f = false;
        if (fresh & bits) { f = true; kb_rep[slot] = now_s + kKbRepeatDelay; }
        else if ((held & bits) && now_s >= kb_rep[slot]) { f = true; kb_rep[slot] = now_s + kKbRepeatEvery; }
        if (!(held & bits)) kb_rep[slot] = DBL_MAX;
        return f;
    };
    auto move_h = [&](int d) {
        const auto& row = rows[kb_row];
        const int n = static_cast<int>(row.size());
        const int i = (kb_key_at(row, kb_unit, nullptr) + d + n) % n;
        int x = 0;
        for (int j = 0; j < i; ++j) x += row[j].w;
        kb_unit = x;
    };
    if (fired(0, kKbDirBits[0])) kb_row = (kb_row + nrows - 1) % nrows;
    if (fired(1, kKbDirBits[1])) kb_row = (kb_row + 1) % nrows;
    if (fired(2, kKbDirBits[2])) move_h(-1);
    if (fired(3, kKbDirBits[3])) move_h(1);
    const int sel = kb_key_at(rows[kb_row], kb_unit, nullptr);
    if (fresh & hid::btn(hid::kBtnA)) closed = kb_press(rows[kb_row][sel]);
    if (!closed && fired(4, hid::btn(hid::kBtnB))) kb_backspace();
    if (!closed && (fresh & hid::btn(hid::kBtnY))) kb_insert(U" ");
    if (!closed && (fresh & hid::btn(hid::kBtnX))) kb_shift = (kb_shift + 1) % 3;
    if (!closed && (fresh & hid::btn(hid::kBtnL)) && kb_caret > 0) --kb_caret;
    if (!closed && (fresh & hid::btn(hid::kBtnR)) && kb_caret < kb_text.size()) ++kb_caret;
    if (!closed && (fresh & hid::btn(hid::kBtnPlus))) closed = kb_finish(true);
    if (!closed && (fresh & hid::btn(hid::kBtnMinus))) closed = kb_finish(false);
    if (closed) {
        clear_imgui_input(imgui);
        return;
    }

    // A centred panel over the dimmed game, sized in units of the UI scale like the menu.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float w = std::max(320.f, std::min(760.f * k, disp.x - 32.f * k));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 110));
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    const std::string title = (kb_req.title.empty() ? std::string("Keyboard") : kb_req.title) + "###nxrt_swkbd";
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin(title.c_str(), nullptr, wf)) {
        ImGui::PushTextWrapPos(0.f);
        if (!kb_req.header.empty()) ImGui::TextUnformatted(kb_req.header.c_str());
        if (!kb_req.sub.empty()) ImGui::TextDisabled("%s", kb_req.sub.c_str());
        ImGui::PopTextWrapPos();

        // the text field: drawn here, not an ImGui InputText, so the keyboard, the pad and the mouse all edit the same text and caret
        const ImGuiStyle& style = ImGui::GetStyle();
        ImFont* font = ImGui::GetFont();
        const float big = ImGui::GetFontSize() * 1.25f;
        const float fh = std::round(big * 1.7f), pad = style.FramePadding.x * 1.5f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float fw = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(fw, fh));
        const ImVec2 p1(p0.x + fw, p0.y + fh);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
        std::string shown, before;
        if (kb_req.password) {
            shown.assign(kb_text.size(), '*');
            before.assign(kb_caret, '*');
        } else {
            shown = u32_to_utf8(kb_text);
            before = u32_to_utf8(kb_text, 0, kb_caret);
        }
        const float cx = font->CalcTextSizeA(big, FLT_MAX, 0.0f, before.c_str()).x;
        const float scroll = std::max(0.f, cx - (fw - 2.f * pad) + 2.f);   // keep the caret in view
        const float ty = p0.y + (fh - big) * 0.5f;
        dl->PushClipRect(ImVec2(p0.x + pad * 0.5f, p0.y), ImVec2(p1.x - pad * 0.5f, p1.y), true);
        if (shown.empty() && !kb_req.guide.empty()) dl->AddText(font, big, ImVec2(p0.x + pad, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), kb_req.guide.c_str());
        else dl->AddText(font, big, ImVec2(p0.x + pad - scroll, ty), ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
        if (std::fmod(now_s, 1.0) < 0.6) {
            const float x = p0.x + pad - scroll + cx;
            dl->AddLine(ImVec2(x, ty), ImVec2(x, ty + big), ImGui::GetColorU32(ImGuiCol_Text), std::max(1.f, 1.5f * k));
        }
        dl->PopClipRect();
        if (kb_req.max_len) {
            char count[32];
            std::snprintf(count, sizeof count, "%zu / %u", kb_text.size(), kb_req.max_len);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fw - ImGui::CalcTextSize(count).x);
            ImGui::TextDisabled("%s", count);
        }
        ImGui::Spacing();

        // the keys; the pad's key has a bright frame, the mouse clicks any of them
        const float gap = std::round(style.ItemSpacing.x * 0.75f);
        const float unit = (ImGui::GetContentRegionAvail().x - gap * (kKbUnits - 1)) / kKbUnits;
        const float kh = std::round(ImGui::GetFontSize() * 2.3f);
        const bool ok_allowed = kb_text.size() >= kb_req.min_len;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (int r = 0; r < nrows && !closed; ++r) {
            int x = 0;
            for (int i = 0; i < static_cast<int>(rows[r].size()) && !closed; ++i) {
                const KbKey& key = rows[r][i];
                if (i) ImGui::SameLine();
                std::string label;
                switch (key.op) {
                    case KbOp::Char: label = u32_to_utf8(std::u32string(1, kb_shift ? key.alt : key.ch)); break;
                    case KbOp::Shift: label = kb_shift == 2 ? "CAPS" : kb_shift ? "SHIFT" : "Shift"; break;
                    case KbOp::Space: label = "Space"; break;
                    case KbOp::Back: label = "Delete"; break;
                    case KbOp::Ok: label = kb_req.ok_text.empty() ? "OK" : kb_req.ok_text; break;
                    case KbOp::Cancel: label = "Cancel"; break;
                }
                ImGui::PushID(r * 32 + i);
                const bool is_sel = r == kb_row && i == sel;
                const bool lit = key.op == KbOp::Shift && kb_shift;
                if (lit) ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
                ImGui::BeginDisabled(key.op == KbOp::Ok && !ok_allowed);
                if (ImGui::Button(label.c_str(), ImVec2(unit * key.w + gap * (key.w - 1), kh))) {
                    kb_row = r;
                    kb_unit = x;
                    closed = kb_press(key);
                }
                ImGui::EndDisabled();
                if (lit) ImGui::PopStyleColor();
                if (is_sel)
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 200, 40, 255), style.FrameRounding,
                                                        0, std::max(2.f, 2.5f * k));
                ImGui::PopID();
                x += key.w;
            }
        }
        ImGui::PopStyleVar();
        if (!closed) {
            ImGui::Spacing();
            // the face buttons are Switch buttons after the player's mapping: by printed label they are named so, by position they sit
            // where a Switch pad has them
            const bool lab = kb_by_label.load(std::memory_order_relaxed);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("Pad: %s type, %s delete, %s space, %s shift, L/R move, + OK, - cancel", lab ? "A" : "A (right)", lab ? "B" : "B (bottom)",
                                lab ? "Y" : "Y (left)", lab ? "X" : "X (top)");
            ImGui::TextDisabled("Keyboard: type, Enter OK, Esc cancel, Ctrl+V paste");
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::End();
    if (closed) clear_imgui_input(imgui);
}

bool Overlay::keyboard_open() const { return impl_->kb_poll(); }
bool Overlay::blocks_game() const { return impl_->visible || impl_->kb_poll(); }
void Overlay::set_keyboard_abxy(bool by_label) { impl_->kb_by_label.store(by_label, std::memory_order_relaxed); }

uint64_t Overlay::set_keyboard_pad(uint64_t buttons, int lx, int ly) {
    Impl& s = *impl_;
    constexpr int kHalf = 16384;     // half of the hid stick range
    uint64_t b = buttons;
    if (lx <= -kHalf) b |= hid::btn(hid::kBtnStickLLeft);
    if (lx >= kHalf) b |= hid::btn(hid::kBtnStickLRight);
    if (ly >= kHalf) b |= hid::btn(hid::kBtnStickLUp);       // hid y: up = +
    if (ly <= -kHalf) b |= hid::btn(hid::kBtnStickLDown);
    const bool open = s.kb_poll();
    if (open) {
        s.pad_new.fetch_or(b & ~s.pad_prev);
        s.pad_mask = b;
    } else {
        s.pad_mask &= b;             // a button leaves the mask once released
    }
    s.pad_held.store(b);
    s.pad_prev = b;
    return open ? ~0ull : s.pad_mask;
}

bool Overlay::wants_input() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, true, true); }
bool Overlay::wants_mouse() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, true, false); }
bool Overlay::wants_keyboard() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, false, true); }

void Overlay::add_panel(const std::string& title, std::function<void()> fn) { impl_->panels.push_back({title, std::move(fn)}); }
void Overlay::set_menu(std::function<void()> fn) { impl_->menu = std::move(fn); }
void Overlay::request_hide() { impl_->want_hide.store(true); }
const Overlay::Context& Overlay::context() const { return impl_->ctx; }

uint64_t Overlay::set_menu_pad(uint64_t b, int lx, int ly) {
    Impl& s = *impl_;
    if (s.kb_poll()) {               // the on-screen keyboard has the pad (set_keyboard_pad)
        s.menu_prev = b;
        return 0;
    }
    if (s.hold_on_hide.exchange(false)) s.menu_mask |= b;   // closed (B, F1, the chord): what is still held stays away from the game
    const uint64_t fresh = b & ~s.menu_prev;
    if (s.menu_chord && (b & s.menu_chord) == s.menu_chord && (fresh & s.menu_chord)) {
        toggle();
        s.menu_mask |= b;
        NXLOG_I(kTag, "overlay %s by the pad chord", s.visible ? "opened" : "closed");
    }
    // ImGui moves with the d-pad only: the left stick past half way (with some hysteresis) presses the d-pad direction too
    auto dir = [](bool was, int v, bool positive) {
        const int t = was ? 12000 : 18000;
        return positive ? v >= t : v <= -t;
    };
    const uint64_t up = hid::btn(hid::kBtnUp), down = hid::btn(hid::kBtnDown), left = hid::btn(hid::kBtnLeft), right = hid::btn(hid::kBtnRight);
    uint64_t stick = 0;
    if (dir((s.menu_stick & up) != 0, ly, true)) stick |= up;      // hid y: up = +
    if (dir((s.menu_stick & down) != 0, ly, false)) stick |= down;
    if (dir((s.menu_stick & left) != 0, lx, false)) stick |= left;
    if (dir((s.menu_stick & right) != 0, lx, true)) stick |= right;
    const uint64_t eff = b | stick, eff_prev = s.menu_prev | s.menu_stick;
    s.menu_stick = stick;
    if (s.visible) {
        // ImGui's gamepad keys, by the Switch button's meaning: A activates, B goes back (the game's own convention, after the player's
        // mapping); L / R and ZL / ZR are the shoulders (the menu switches its tabs with them). X, Y, + and - are not given to ImGui (Y would
        // be its windowing mode).
        static const std::pair<hid::NpadButtonBit, ImGuiKey> kMap[] = {
            {hid::kBtnA, ImGuiKey_GamepadFaceDown},     {hid::kBtnB, ImGuiKey_GamepadFaceRight},  {hid::kBtnL, ImGuiKey_GamepadL1},
            {hid::kBtnR, ImGuiKey_GamepadR1},           {hid::kBtnZL, ImGuiKey_GamepadL2},        {hid::kBtnZR, ImGuiKey_GamepadR2},
            {hid::kBtnUp, ImGuiKey_GamepadDpadUp},      {hid::kBtnDown, ImGuiKey_GamepadDpadDown}, {hid::kBtnLeft, ImGuiKey_GamepadDpadLeft},
            {hid::kBtnRight, ImGuiKey_GamepadDpadRight}};
        // a button held from before the menu opened (the chord) counts only once it is released and pressed again
        const uint64_t changed = (eff ^ eff_prev) & ~(s.menu_mask & b);
        if (changed) {
            std::lock_guard<std::mutex> lk(s.mu);
            ImGuiContext* prev = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(s.imgui);
            ImGuiIO& io = ImGui::GetIO();
            for (const auto& kv : kMap) {
                const uint64_t bit = hid::btn(kv.first);
                if (changed & bit) io.AddKeyEvent(kv.second, (eff & bit) != 0);
            }
            ImGui::SetCurrentContext(prev);
        }
    }
    s.menu_mask &= b;                // a button leaves the mask once released
    s.menu_prev = b;
    return s.visible ? ~0ull : s.menu_mask;
}

bool Overlay::handle_event(const input::Event& e) {
    Impl& s = *impl_;
    using input::EventType;
    if ((e.type == EventType::KeyDown || e.type == EventType::KeyUp) && e.u.key.scancode == s.toggle_sc) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) toggle();
        return true;   // the toggle key never reaches the game (press, repeat and release)
    }
    const bool kb = s.kb_poll();
    if (kb && (e.type == EventType::KeyDown || e.type == EventType::KeyUp || e.type == EventType::Text)) return s.kb_key_event(e);
    if (!s.visible && !kb) return false;
    std::lock_guard<std::mutex> lk(s.mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.imgui);
    ImGuiIO& io = ImGui::GetIO();
    bool consumed = false;
    switch (e.type) {
        case EventType::KeyDown:
        case EventType::KeyUp: {
            const ImGuiKey k = map_key(e.u.key.scancode);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == EventType::KeyDown);
            consumed = io.WantCaptureKeyboard;
            break;
        }
        case EventType::Text:
            io.AddInputCharactersUTF8(e.u.text.utf8);
            consumed = io.WantTextInput;
            break;
        case EventType::MouseMotion:
            io.AddMousePosEvent(e.u.motion.x, e.u.motion.y);
            consumed = io.WantCaptureMouse;
            break;
        case EventType::MouseButtonDown:
        case EventType::MouseButtonUp: {
            const int b = e.u.button.button;   // SDL: 1 left, 2 middle, 3 right
            const int ib = b == 1 ? 0 : b == 3 ? 1 : b == 2 ? 2 : b - 1;
            io.AddMousePosEvent(e.u.button.x, e.u.button.y);
            io.AddMouseButtonEvent(ib, e.type == EventType::MouseButtonDown);
            consumed = io.WantCaptureMouse;
            break;
        }
        case EventType::MouseWheel:
            io.AddMouseWheelEvent(e.u.wheel.dx, e.u.wheel.dy);
            consumed = io.WantCaptureMouse;
            break;
        case EventType::WindowFocusLost: io.AddFocusEvent(false); break;
        case EventType::WindowFocusGained: io.AddFocusEvent(true); break;
        default: break;
    }
    ImGui::SetCurrentContext(prev);
    // the on-screen keyboard takes the whole mouse while it is up
    if (kb) consumed = e.type == EventType::MouseMotion || e.type == EventType::MouseButtonDown || e.type == EventType::MouseButtonUp || e.type == EventType::MouseWheel;
    return consumed;
}

void Overlay::apply_pending() {
    apply_present_pending();
    apply_window_pending();
}

void Overlay::apply_present_pending() {
    Impl& s = *impl_;
    if (s.want_mode) {
        s.want_mode = false;
        s.ctx.renderer->set_present_mode(s.new_mode);
    }
}

void Overlay::apply_window_pending() {
    Impl& s = *impl_;
    if (s.want_fullscreen) {
        s.want_fullscreen = false;
        platform::set_fullscreen(s.new_fullscreen);
    }
}

void Overlay::Impl::refresh_counter_text(double now_s) {
    if (now_s - text_s < 0.25) return;    // the text is rebuilt at most every 250 ms
    text_s = now_s;
    snap = stats.snapshot(now_s);
    const int detail = counter_detail.load(std::memory_order_relaxed);
    char buf[64];
    counter_n = 0;
    // With frame generation the frames that reach the screen: this counter sees the game's own frames only, the generated ones are made
    // after it. The most detailed view adds the game's own rate, and its 1% low and worst frame (measured on the game's frames) say so.
    const float ratio = extras::framegen_shown_ratio();
    const double shown = ratio > 1.05f ? ratio : 1.0;
    const char* real = shown > 1.0 ? "real " : "";
    if (snap.valid) std::snprintf(buf, sizeof buf, "%.0f FPS", snap.fps * shown);
    else std::snprintf(buf, sizeof buf, "-- FPS");
    counter_lines[counter_n++] = buf;
    if (detail >= 1) {
        if (snap.valid) std::snprintf(buf, sizeof buf, "%.1f ms", snap.avg_ms / shown);
        else std::snprintf(buf, sizeof buf, "-- ms");
        counter_lines[counter_n++] = buf;
    }
    if (detail >= 2 && shown > 1.0 && snap.valid) {
        std::snprintf(buf, sizeof buf, "%.0f real", snap.fps);
        counter_lines[counter_n++] = buf;
    }
    if (detail >= 2) {
        if (snap.valid) std::snprintf(buf, sizeof buf, "%s1%% low %.0f", real, snap.low1_fps);
        else std::snprintf(buf, sizeof buf, "1%% low --");
        counter_lines[counter_n++] = buf;
        if (snap.valid) std::snprintf(buf, sizeof buf, "%smax %.1f ms", real, snap.max_ms);
        else std::snprintf(buf, sizeof buf, "max -- ms");
        counter_lines[counter_n++] = buf;
    }
}

// Draws the counter with the foreground draw list: no window, so it never takes mouse or keyboard input.
void Overlay::Impl::draw_counter(const FrameInfo& f, double now_s) {
    refresh_counter_text(now_s);
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 45.f), 14.f, 40.f);
    const float pad = size * 0.4f;
    const float gap = size * 0.15f;
    const float margin = size * 0.8f;
    const int n = counter_n;
    float lines_w[4] = {};
    float line_h = 0, text_w = 0;
    for (int i = 0; i < n; ++i) {
        const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, counter_lines[i].c_str());
        lines_w[i] = sz.x;
        line_h = std::max(line_h, sz.y);
        text_w = std::max(text_w, sz.x);
    }
    const float box_w = text_w + 2.f * pad;
    const float box_h = static_cast<float>(n) * line_h + static_cast<float>(n - 1) * gap + 2.f * pad;
    const int corner = counter_corner.load(std::memory_order_relaxed);
    const bool right = corner == 1 || corner == 3;
    const bool bottom = corner >= 2;
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float x0 = right ? W - margin - box_w : margin;
    const float y0 = bottom ? H - margin - box_h : margin;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + box_w, y0 + box_h), IM_COL32(0, 0, 0, 140), size * 0.3f);
    for (int i = 0; i < n; ++i) {
        const float x = right ? x0 + box_w - pad - lines_w[i] : x0 + pad;
        const float y = y0 + pad + static_cast<float>(i) * (line_h + gap);
        dl->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), counter_lines[i].c_str());
    }
}

bool Overlay::Impl::note_visible(double now_s) {
    if (!note_read) {
        note_read = true;
        note_enabled = nxrt::config_get_bool("nvn", "compile_note", true);
    }
    const int pending = shader_backlog();
    if (!note_enabled) return false;
    if (pending > 0) {
        if (note_since_s < 0) { note_since_s = now_s; note_peak = 0; }
        note_empty_s = -1.0;
        note_peak = std::max(note_peak, pending);
        return now_s - note_since_s >= 1.5;
    }
    if (note_since_s < 0) return false;
    if (note_empty_s < 0) note_empty_s = now_s;
    if (now_s - note_empty_s >= 1.0) { note_since_s = -1.0; return false; }
    return now_s - note_since_s >= 1.5;
}

void Overlay::Impl::draw_note(const FrameInfo& f) {
    char text[96];
    const int pending = shader_backlog();
    if (pending > 0) std::snprintf(text, sizeof text, "Preparing shaders (%d left)", pending);
    else std::snprintf(text, sizeof text, "Preparing shaders (done)");
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 60.f), 12.f, 28.f);
    const float pad = size * 0.4f;
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float x0 = (W - sz.x) * 0.5f - pad, y0 = H - size * 0.8f - sz.y - 2.f * pad;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sz.x + 2.f * pad, y0 + sz.y + 2.f * pad), IM_COL32(0, 0, 0, 120), size * 0.3f);
    dl->AddText(font, size, ImVec2(x0 + pad, y0 + pad), IM_COL32(255, 255, 255, 220), text);
}

void Overlay::draw(const FrameInfo& f) {
    Impl& s = *impl_;
    if (s.want_hide.exchange(false)) set_visible(false);   // the menu asked to close during the last frame (mu is not held here)
    const bool counter_on = s.counter_show.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const double now_s = std::chrono::duration<double>(now.time_since_epoch()).count();
    const bool note_on = s.note_visible(now_s) && (s.counter_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    const bool ptr_on = s.ptr_show.load(std::memory_order_relaxed) && (s.ptr_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    const bool kb_on = s.kb_poll();
    if (!s.visible && !counter_on && !note_on && !ptr_on && !kb_on) {
        s.last_frame = now;
        s.counter_was_on = false;
        return;
    }
    if (counter_on) {
        if (!s.counter_was_on) {          // just switched on: start from an empty history
            s.counter_was_on = true;
            s.counter_have_prev = false;
            s.stats.reset();
            s.text_s = -1e9;
        }
        if (s.counter_have_prev) {
            const double dt_ms = std::chrono::duration<double, std::milli>(now - s.counter_prev).count();
            if (dt_ms <= 1000.0) s.stats.push(now_s, dt_ms);   // over a second: a pause (debugger, window in the background), not a frame
        }
        s.counter_prev = now;
        s.counter_have_prev = true;
    } else {
        s.counter_was_on = false;
    }
    // a screenshot leaves the counter out unless asked for; with the menu hidden there is then nothing to draw
    const bool show_counter = counter_on && (s.counter_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    if (!s.visible && !show_counter && !note_on && !ptr_on && !kb_on) {
        s.last_frame = now;
        return;
    }
    const bool hdr_frame = s.hdr && f.hdr_output;
    if (s.hdr && !f.hdr_output) return;      // the display left HDR mode: the RGBA8 overlay pipeline does not match the SDR swapchain, so no overlay until the next start
    if (hdr_frame && !s.composite.ensure_target(f.extent)) return;
    std::lock_guard<std::mutex> lk(s.mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - s.last_frame).count(), 1.0f / 1000.f, 0.25f);
    s.last_frame = now;
    io.DisplaySize = ImVec2(static_cast<float>(f.extent.width), static_cast<float>(f.extent.height));

    s.update_scale(f.extent);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (s.visible && !kb_on) s.build_ui();    // the keyboard is modal: the menu comes back once it is answered
    if (kb_on) s.draw_keyboard(now_s);
    if (show_counter) s.draw_counter(f, now_s);
    if (note_on) s.draw_note(f);
    if (ptr_on) {   // a plain arrow, white with a dark outline, sized with the frame height
        const float k = std::max(1.f, static_cast<float>(f.extent.height) / 720.f);
        const float x = s.ptr_x.load(std::memory_order_relaxed), y = s.ptr_y.load(std::memory_order_relaxed);
        const ImVec2 pts[7] = {{0, 0}, {0, 16 * k}, {4 * k, 12.5f * k}, {7.5f * k, 19 * k}, {10 * k, 18 * k}, {6.5f * k, 11.5f * k}, {12 * k, 11.5f * k}};
        ImVec2 poly[7];
        for (int i = 0; i < 7; ++i) poly[i] = ImVec2(x + pts[i].x, y + pts[i].y);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddConvexPolyFilled(poly, 7, IM_COL32(255, 255, 255, 255));
        dl->AddPolyline(poly, 7, IM_COL32(0, 0, 0, 255), ImDrawFlags_Closed, 1.5f * k);
    }
    ImGui::Render();

    // one dynamic-rendering pass that draws the ImGui draw data into view
    auto render_into = [&](VkImageView view, VkAttachmentLoadOp load) {
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = load;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, f.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(f.cmd, &ri);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
        vkCmdEndRendering(f.cmd);
    };
    if (hdr_frame) {
        // HDR: the overlay goes into the cleared RGBA8 target, then the composite maps it onto the swapchain image (UI white = paper white)
        const extras::HdrState hs = extras::hdr_state();
        s.composite.begin_overlay(f.cmd);
        render_into(s.composite.target_view(), VK_ATTACHMENT_LOAD_OP_CLEAR);
        s.composite.composite(f.cmd, f.view, f.format, f.extent, s.ctx.renderer->output_kind(), hs.paper_white_nits, hs.peak_nits);
    } else {
        render_into(f.view, VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    s.drawn++;
    ImGui::SetCurrentContext(prev);
}

void Overlay::install_on_renderer() {
    impl_->ctx.renderer->set_overlay_hook([this](const FrameInfo& f) { draw(f); });
}

}  // namespace nxrt::gfx
