// SPDX-License-Identifier: GPL-3.0-or-later
//! Language packs: plain data files next to the launcher, `<launcher folder>/locales/<code>.json`.
//!
//! A pack is only data. Nothing here evaluates it: it is parsed as JSON and the launcher reads
//! strings out of it, so a pack cannot add code, change a command or reach the filesystem. A broken
//! or half-finished pack cannot break the launcher either — a missing key falls back to English in
//! the front end, and a file that does not parse is reported once and simply not offered.
//!
//! Format (UTF-8, no BOM):
//!
//! ```json
//! { "code": "de", "name": "Deutsch", "strings": { "nav.play": "Spielen" } }
//! ```
//!
//! Only `code` and `strings` are used; `name` falls back to the code. Entries whose value is not a
//! string are skipped, so a comment or a nested object cannot smuggle anything in.
//!
//! The `code` inside a pack has to match the file name, because `read_locale` opens `<code>.json`.
//! A pack that declares a different code is skipped with a message instead of being offered: listing
//! it would show a language that silently falls back to English once picked.

use serde::Serialize;
use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Serialize, Clone)]
pub struct LocaleInfo {
    pub code: String,
    pub name: String,
}

#[derive(Serialize)]
pub struct LocalePack {
    pub code: String,
    pub name: String,
    pub strings: BTreeMap<String, String>,
}

/// The folder the launcher reads packs from: `<launcher folder>/locales`.
pub fn locales_dir() -> PathBuf {
    crate::exe_dir().join("locales")
}

/// Every usable pack in the folder, sorted by code. The built-in languages ship inside the
/// executable, so they are not listed here; `list_locales` adds them in the front end.
#[tauri::command]
pub fn list_locales() -> Vec<LocaleInfo> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(locales_dir()) else {
        return out;
    };
    for entry in rd.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        // the template shipped for translators is not a language of its own
        if path.file_name().and_then(|n| n.to_str()).map(|n| n.eq_ignore_ascii_case("template.json")).unwrap_or(false) {
            continue;
        }
        match read_pack(&path) {
            Ok(p) => out.push(LocaleInfo { code: p.code, name: p.name }),
            // a pack that does not parse is skipped, not fatal: one bad file must not hide the rest
            Err(e) => eprintln!("[locale] skipping {}: {}", path.display(), e),
        }
    }
    out.sort_by(|a, b| a.code.cmp(&b.code));
    out.dedup_by(|a, b| a.code == b.code);
    out
}

/// One pack by code, from `<launcher folder>/locales/<code>.json`.
#[tauri::command]
pub fn read_locale(code: String) -> Result<LocalePack, String> {
    if code.is_empty() || !code.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_') {
        return Err(format!("not a language code: {code}"));
    }
    let path = locales_dir().join(format!("{code}.json"));
    read_pack(&path)
}

fn read_pack(path: &std::path::Path) -> Result<LocalePack, String> {
    let stem = path.file_stem().and_then(|s| s.to_str()).map(|s| s.to_string());
    let text = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let text = text.trim_start_matches('\u{feff}');
    let root: serde_json::Value = serde_json::from_str(text).map_err(|e| format!("not JSON: {e}"))?;
    let code = root
        .get("code")
        .and_then(|v| v.as_str())
        .map(|s| s.to_string())
        .or_else(|| stem.clone())
        .ok_or("no code")?;
    // The file name is the code `read_locale` looks the pack up by, so the two have to agree.
    if let Some(stem) = stem.as_deref() {
        if !code.eq_ignore_ascii_case(stem) {
            return Err(format!("code \"{code}\" does not match the file name \"{stem}.json\""));
        }
    }
    let name = root.get("name").and_then(|v| v.as_str()).unwrap_or(&code).to_string();
    let mut strings = BTreeMap::new();
    if let Some(map) = root.get("strings").and_then(|v| v.as_object()) {
        for (k, v) in map {
            if let Some(s) = v.as_str() {
                strings.insert(k.clone(), s.to_string());
            }
        }
    }
    if strings.is_empty() {
        return Err("no strings".to_string());
    }
    Ok(LocalePack { code, name, strings })
}
