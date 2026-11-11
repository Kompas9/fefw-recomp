// SPDX-License-Identifier: GPL-3.0-or-later
// Haptic feedback for the play-mode gamepads (S-haptic). The game sends the Switch HD Rumble value (amp_low, freq_low,
// amp_high, freq_high per hand) through the vibration sink in input_source.h; this module decides how it reaches the pad:
//
//   Auto       DualSense on a wire gets haptics, a Switch Pro Controller gets HD Rumble packets, everything else gets the
//              standard two-motor rumble the runtime always had.
//   Off        nothing is sent (the game's vibration values are dropped).
//   Rumble     always the standard SDL two-motor rumble, whatever the pad is.
//   DualSense  haptics when a DualSense is on USB; falls back to the mode above it would have picked, and says why.
//
// The haptic backends come from renhdrum.dll (MIT OR Apache-2.0, https://github.com/trzz1214/RenHDrum), loaded next to the
// executable when it is there: the library is optional, and without it Auto and DualSense behave as Rumble.
//
// A haptic backend only owns the pad while it is really driving it: when the library is missing, the engine or the audio
// stream fails to start, the DualSense audio endpoint disappears, or SDL refuses a packet, the module drops back to the
// standard rumble on its own (legacy_rumble() turns true again), so the pad is never left without feedback.
//
// Every function is safe to call from any thread. Nothing here blocks the calling thread for long: the packets are sent by a
// 1 ms worker, and the DualSense audio stream is filled by SDL's audio thread. The state the other threads read is atomic;
// the two engines are behind one mutex; no lock is ever held while waiting for the worker.
#pragma once
#include <cstdint>
#include <cstring>

namespace nxrt::gfx::haptic {

// What the settings ask for ([pad] haptic).
enum class Mode : uint8_t { Auto = 0, Off, Rumble, DualSense };
// Inline: play_config.cpp uses them in builds without this module (mpr_launcher).
inline const char* mode_name(Mode m) {   // "auto" "off" "rumble" "dualsense"
    switch (m) {
        case Mode::Off: return "off";
        case Mode::Rumble: return "rumble";
        case Mode::DualSense: return "dualsense";
        default: return "auto";
    }
}

inline bool mode_from_name(const char* s, Mode* out) {
    if (!s || !out) return false;
    if (!std::strcmp(s, "auto")) *out = Mode::Auto;
    else if (!std::strcmp(s, "off") || !std::strcmp(s, "none")) *out = Mode::Off;
    else if (!std::strcmp(s, "rumble") || !std::strcmp(s, "standard")) *out = Mode::Rumble;
    else if (!std::strcmp(s, "dualsense") || !std::strcmp(s, "haptics")) *out = Mode::DualSense;
    else return false;
    return true;
}

// What is actually driving the pad.
enum class Backend : uint8_t {
    None = 0,        // no pad, or nothing is sent
    Rumble,          // SDL_RumbleGamepad, two motors
    ProController,   // renhdrum 10-byte sub-command 0x10 packets
    DualSenseUsb,    // renhdrum 4-channel 48 kHz audio haptics
    DualSenseBt,     // DualSense over Bluetooth: the standard rumble is all there is
};
const char* backend_name(Backend b);

struct Status {
    bool dll = false;          // renhdrum.dll was found and loaded
    Mode requested = Mode::Auto;
    Backend backend = Backend::None;
    bool running = false;      // a haptic backend is driving the pad right now (not just chosen)
    bool wired = false;        // the pad is on a cable
    char device[64] = {};      // SDL's name for the pad
    char detail[192] = {};     // why the backend is what it is, and what it is doing
    float last_amp = 0.f;      // the largest amplitude pushed in the last second
    uint64_t pushes = 0;       // values the game sent since the pad was attached
    uint64_t packets = 0;      // haptic packets/audio frames sent since then
};

// Applies [pad] haptic, [pad] haptic_strength, [pad] rumble_scale and [pad] rumble.
//   strength_percent >= 0 : the haptic amplitude scale, 0..100 percent.
//   strength_percent <  0 : inherit the player's own [pad] rumble_scale, which is the multiplier the standard rumble uses
//                           (1.0 = unchanged, up to 2.0), so an untouched controls.ini keeps exactly one strength knob.
//   rumble_on             : the [pad] rumble master switch. False silences every output path, the standard rumble included
//                           (legacy_rumble() returns false too), and clears what the backends are still playing.
// Only a different mode or master switch re-attaches: the strength applies to the next value, so moving its slider never
// reopens the DualSense audio stream.
void configure(Mode m, int strength_percent, double rumble_scale, bool rumble_on);

// Pads, from the platform layer's hot-plug events (main thread).
void pad_added(uint32_t instance_id);
void pad_removed(uint32_t instance_id);

// SDL reported an audio endpoint coming or going: look for the DualSense one right away instead of on the two second poll.
void audio_devices_changed();

// One HD Rumble value from the game: hand 0 = left (or the only device), 1 = right. Called from the game's thread.
void push(uint32_t npad_slot, int hand, float amp_low, float freq_low, float amp_high, float freq_high);

// True when the caller should keep sending the standard two-motor rumble itself (play.cpp's map_rumble). False only while a
// haptic backend is really driving the pad, so the two never fight over it and a dead backend never leaves the pad silent; also
// false whenever [pad] rumble is off, because then nothing is to vibrate at all.
bool legacy_rumble();

// Stops everything and drops the pads (play mode is ending).
void shutdown();

// A short, safe test pattern on the attached pad; it stops by itself. False when there is nothing to drive.
bool test_start();

Status status();

}  // namespace nxrt::gfx::haptic
