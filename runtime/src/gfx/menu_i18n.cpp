// SPDX-License-Identifier: GPL-3.0-or-later
// The F1 menu's translation table (declarations in include/nxrt/menu_i18n.h).
//
// [gfx] ui_language names a file next to fefw_rt.dll: locales\<name>.ini. "auto" or empty: English (no table).
// The file holds one "key = value" pair per line (UTF-8, no BOM; '#' or ';' starts a comment). The key is the
// English string as the source code shows it; the value is what the menu displays. A missing key or file falls
// back to the English string, so a partial or broken table only means some lines stay English.
#include "nxrt/menu_i18n.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::gfx {
namespace {

constexpr const char* kTag = "menu-i18n";

std::unordered_map<std::string, std::string>& table() {
    static std::unordered_map<std::string, std::string> t;
    return t;
}

std::string exe_dir() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string p(buf);
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "key = value" lines; the value keeps inner spaces, the key is trimmed. UTF-8 in, UTF-8 out (ImGui is UTF-8 aware).
void load_file(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return;
    std::string line;
    size_t n = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line[0] == '\xef' && line.size() >= 3 && (unsigned char)line[1] == '\xbb' &&
                              (unsigned char)line[2] == '\xbf'))
            line = line.substr(3);   // strip a BOM if one slipped in
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        // A tab splits key from value, so a key may itself contain " = " (several help texts do).
        // Without a tab the first '=' splits, as before.
        size_t eq = t.find('\t');
        size_t skip = 1;
        if (eq == std::string::npos) {
            eq = t.find('=');
            skip = 1;
        }
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + skip));
        if (key.empty() || value.empty()) continue;
        table()[key] = value;
        ++n;
    }
    NXLOG_I(kTag, "loaded %zu translations from %s", n, path.c_str());
}

} // namespace

void menu_i18n_init() {
    table().clear();
    std::string lang = config_get_str("gfx", "ui_language", "auto");
    lang = trim(lang);
    if (lang.empty() || lang == "auto" || lang == "en" || lang == "en-US") return;   // English: the source strings
    const std::string dir = exe_dir();
    if (dir.empty()) return;
    load_file(dir + "\\locales\\" + lang + ".ini");
}

const char* tr(const char* s) {
    if (!s || !*s) return s;
    const auto& t = table();
    auto it = t.find(s);
    if (it != t.end()) return it->second.c_str();
    // Some display strings carry padding (" settings", a trailing space in a wrapped help);
    // the ini keys are trimmed, so try the trimmed form before giving up.
    const std::string key = trim(std::string(s));
    if (key != s) {
        it = t.find(key);
        if (it != t.end()) return it->second.c_str();
    }
    return s;
}

} // namespace nxrt::gfx
