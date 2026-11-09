// SPDX-License-Identifier: GPL-3.0-or-later
// Switch 2 Joy-Con 2 mouse ("Nmouse" / "Jcms") from the PC mouse. The hid service reports one virtual Joy-Con 2 (R) that works as a
// mouse and writes its state into the game's Nmouse transfer memory (layout: docs/hid_mouse_sw2.md). The play layer feeds it the host
// pointer once per frame; whoever owns the mouse camera asks here whether it must hand the mouse over.
//
// The game sees what a real Joy-Con 2 (R) mouse sends: the pointer moves by the PC mouse's motion counts (x, y in the pointer range, which
// the game itself re-centres every frame with SetNmousePointerPosition and reads back as a movement), and "on its surface" (attribute
// bit 1) says the mouse is in use. Fortune's Weave switches to its mouse controls after it sees that bit for a few frames and back to the
// buttons a moment after it clears (docs/hid_mouse_sw2.md).
//
// Config: [input] jc2_mouse = auto | on | off (default auto; game::Info::jc2_mouse_default may change it).
//   off   no Joy-Con 2 mouse is listed; the game never sees one.
//   on    the mouse is always on its surface while the window has focus (the game stays in its mouse controls; keys and pads still
//         work but do not lift it).
//   auto  the last device used wins: the mouse goes on its surface when it really moves (more than the wake distance within a short
//         time) or clicks, and is lifted at once by any key or pad input (jc2_mouse_other_input). A still or jittery mouse does nothing.
//
// The play layer feeds the host mouse once per frame (motion counts, buttons, wheel), tells hid about other devices, captures the mouse
// while jc2_mouse_engaged() (the game draws its own cursor and the motion must not stop at the window's edge) and releases it as soon
// as the mouse is lifted. All functions are thread safe and cheap.
#pragma once
#include <cstdint>

namespace nxrt::hid {

enum class Jc2MouseMode : uint8_t { Off = 0, Auto = 1, On = 2 };

Jc2MouseMode jc2_mouse_mode();                 // [input] jc2_mouse, read on first use (auto when missing or unknown)
void jc2_mouse_set_mode(Jc2MouseMode m);       // overrides the config value (tests, a future menu)
const char* jc2_mouse_mode_name(Jc2MouseMode m);

// The host pointer, once per rendered frame (play mode, main thread).
struct HostMouse {
    float dx = 0, dy = 0;        // motion since the previous feed, in pointer-range units (the play layer applies [mouse] jc2_speed)
    float wake = 0;              // motion since the previous feed in raw mouse counts (the wake distance is in these)
    uint32_t buttons = 0;        // nn::hid::MouseButton bits held: Left 0, Right 1, Middle 2, Forward 3, Back 4
    uint32_t presses = 0;        // buttons pressed since the previous feed (a click shorter than a frame still counts)
    float wheel_x = 0, wheel_y = 0;   // wheel notches since the previous feed (y: away from the user is +)
    bool focused = false;        // the window has focus and no runtime menu is up
    float u = 0, v = 0;          // the Windows pointer on the game picture (0..1, v down) while the mouse is not captured
    bool pointer = false;        // u, v are valid (not captured, over the picture)
};
void jc2_mouse_feed(const HostMouse& m);
// Lining the pointers up (the game layer, main thread): where the Windows pointer was on the picture when it was last free (false: never
// seen over the picture), so the game's own pointer can start there; and where the game's pointer is now (picture 0..1), so play mode
// puts the Windows pointer there when it frees the mouse.
bool jc2_mouse_host_pointer(float* u, float* v);
void jc2_mouse_set_return_point(float u, float v);
bool jc2_mouse_take_return_point(float* u, float* v);   // play mode: the point, once
// A key or a pad button / stick was used: the mouse is lifted at once (auto mode).
void jc2_mouse_other_input();
// The wake distance in raw mouse counts ([mouse] jc2_wake, default 8): the mouse goes on its surface after moving this far within 0.25 s.
void jc2_mouse_set_wake(float counts);
// The mouse is on its surface: the game sees mouse input and switches to (or stays in) its mouse controls.
bool jc2_mouse_engaged();
// Counters for logs and the F1 menu.
struct Jc2Stats {
    uint64_t engages = 0, lifts = 0;   // times the mouse went on its surface / was lifted
    uint64_t recentres = 0;            // SetNmousePointerPosition calls (Fortune's Weave: one per frame)
    int32_t x = 0, y = 0;              // the pointer as last sampled
};
Jc2Stats jc2_mouse_stats();

// Camera owner / game layer: the game is in a mode that wants a pointer (kept for games with a camera of their own; Fortune's Weave
// does not use it).
void jc2_mouse_request(bool want);
// Play mode: true = do not capture the mouse for the camera (mode on, or auto with a request).
bool jc2_mouse_wants_pointer();
// True while the virtual Joy-Con 2 mouse is listed, the mode is not off and the feed is live (focused window, a feed in the last 250 ms).
bool jc2_mouse_active();

}  // namespace nxrt::hid
