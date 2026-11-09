// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime menu translation (F1 overlay). Load a key = value table once and translate strings through nxrt::gfx::tr().
// The table lives next to fefw_rt.dll in locales\<name>.ini, chosen by [gfx] ui_language ("auto"/empty: English, no table).
// Missing keys fall back to the input string, so a partial table is always safe.
#pragma once

namespace nxrt::gfx {

// Translate a UI string. Returns the input unchanged when no table is loaded or the key is missing.
const char* tr(const char* s);

// Load the table for [gfx] ui_language. Called once from the overlay's font setup; safe to call again (reloads).
void menu_i18n_init();

} // namespace nxrt::gfx
