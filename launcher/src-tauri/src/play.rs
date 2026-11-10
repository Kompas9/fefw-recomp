// SPDX-License-Identifier: GPL-3.0-or-later
//! Starts fefw.exe in the run folder, watches it, and reports how it ended. On Linux fefw.exe (a Windows program) runs through a
//! runner: the launcher's `runner` setting, else umu-run (Proton outside Steam), else wine.

use serde::Serialize;
use std::path::Path;
use std::process::Command;
use std::sync::Mutex;
use std::time::{Duration, SystemTime};
use tauri::{AppHandle, Emitter, Manager};

/// Files the launcher may open for the player, by name, inside the run folder (the runtime writes the reports next to the log).
pub const REPORT_FILES: [&str; 4] = ["fefw_crash.txt", "fefw_freeze.txt", "fefw_fatal.txt", "fefw.log"];

#[derive(Serialize, Clone, Default)]
pub struct ReportFile {
    pub name: String,
    /// written since this run started
    pub fresh: bool,
}

#[derive(Serialize, Clone)]
pub struct Status {
    /// "idle", "running" or "exited"
    pub state: String,
    pub code: Option<i32>,
    pub crashed: bool,
    pub files: Vec<ReportFile>,
    pub message: String,
}

impl Default for Status {
    fn default() -> Self {
        Status { state: "idle".into(), code: None, crashed: false, files: vec![], message: String::new() }
    }
}

#[derive(Default)]
pub struct PlayState(pub Mutex<Status>);

fn set_status(app: &AppHandle, s: Status) {
    *app.state::<PlayState>().0.lock().unwrap() = s.clone();
    let _ = app.emit("game-status", s);
}

fn report_files(dir: &Path, started: SystemTime) -> Vec<ReportFile> {
    REPORT_FILES
        .iter()
        .filter_map(|n| {
            let md = std::fs::metadata(dir.join(n)).ok()?;
            let fresh = md.modified().map(|m| m + Duration::from_secs(2) >= started).unwrap_or(false);
            Some(ReportFile { name: n.to_string(), fresh })
        })
        .collect()
}

/// The command that starts fefw.exe: the program and its arguments before the exe. On Windows nothing (the exe itself). On Linux
/// `custom` (the runner setting, split into words like a shell does, so `"/path/Proton - Experimental/proton" run` works), else
/// umu-run, else wine.
pub fn runner(custom: &str) -> Result<Vec<String>, String> {
    if cfg!(windows) {
        return Ok(vec![]);
    }
    let words = words(custom);
    if !words.is_empty() {
        return Ok(words);
    }
    for r in ["umu-run", "wine"] {
        if on_path(r) {
            return Ok(vec![r.to_string()]);
        }
    }
    Err("fefw.exe is a Windows program: on Linux it runs through Proton or Wine. Install umu-launcher (umu-run, which uses Proton) or \
         Wine, or set the runner in Settings."
        .into())
}

/// Shell-like words: spaces separate them, "..." and '...' group them, a backslash outside '...' takes the next character as is.
fn words(s: &str) -> Vec<String> {
    let (mut out, mut cur, mut any) = (Vec::new(), String::new(), false);
    let mut quote: Option<char> = None;
    let mut it = s.chars();
    while let Some(c) = it.next() {
        match (quote, c) {
            (Some(q), c) if c == q => quote = None,
            (Some('\''), c) => cur.push(c),
            (_, '\\') => {
                if let Some(n) = it.next() {
                    cur.push(n);
                }
                any = true;
            }
            (Some(_), c) => cur.push(c),
            (None, '"' | '\'') => {
                quote = Some(c);
                any = true;
            }
            (None, c) if c.is_whitespace() => {
                if any || !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                }
                any = false;
            }
            (None, c) => cur.push(c),
        }
    }
    if any || !cur.is_empty() {
        out.push(cur);
    }
    out
}

/// Whether the runner is umu-run, also behind `env VAR=...`.
fn is_umu(runner: &[String]) -> bool {
    runner
        .iter()
        .find(|w| *w != "env" && !w.contains('='))
        .is_some_and(|p| Path::new(p).file_name().is_some_and(|n| n == "umu-run"))
}

fn on_path(name: &str) -> bool {
    std::env::var_os("PATH").is_some_and(|p| std::env::split_paths(&p).any(|d| d.join(name).is_file()))
}

/// `launcher_dir`: the folder the package's upscalers/ is in (the AppImage's folder, not its mount).
pub fn start(app: &AppHandle, exe: &Path, game_dir: &Path, launcher_dir: &Path, minimize: bool, runner_setting: &str) -> Result<(), String> {
    if app.state::<PlayState>().0.lock().unwrap().state == "running" {
        return Err("The game is already running.".into());
    }
    if !exe.is_file() {
        return Err(format!("fefw.exe was not found at {}. Run Set up first.", crate::dump::fwd(exe)));
    }
    let runner = runner(runner_setting)?;
    let app2 = app.clone();
    let dir = game_dir.to_path_buf();
    let exe = exe.to_path_buf();
    let launcher_dir = launcher_dir.to_path_buf();
    set_status(app, Status { state: "running".into(), message: "Starting fefw.exe...".into(), ..Default::default() });
    std::thread::spawn(move || {
        let mut cmd = match runner.split_first() {
            None => Command::new(&exe),
            Some((prog, args)) => {
                let mut c = Command::new(prog);
                c.args(args).arg(&exe);
                // umu-run wants a game id (it picks protonfixes for known games from it); "0" is the generic one its README uses
                if is_umu(&runner) && std::env::var_os("GAMEID").is_none() {
                    c.env("GAMEID", "0");
                }
                c
            }
        };
        crate::setup::host_env(&mut cmd);
        // fefw.exe reads fefw.ini from its working directory: every path comes from there
        cmd.current_dir(&dir);
        // the upscaler DLLs ship next to the launcher (upscalers/fsr, upscalers/streamline), which need not be the run folder
        // (on Linux as fefw.exe sees it under Wine: Z:/...)
        if cfg!(windows) {
            cmd.env("NXRT_LAUNCHER_DIR", &launcher_dir);
        } else {
            cmd.env("NXRT_LAUNCHER_DIR", crate::dump::game_path(&launcher_dir));
        }
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no stray console
        }
        let started = SystemTime::now();
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                let what = if runner.is_empty() { String::new() } else { format!(" ({})", runner.join(" ")) };
                set_status(&app2, Status { state: "exited".into(), crashed: true, message: format!("Could not start fefw.exe: {e}{what}"), ..Default::default() });
                return;
            }
        };
        set_status(&app2, Status { state: "running".into(), message: format!("fefw.exe started (process {})", child.id()), ..Default::default() });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.minimize();
            }
        }
        let (code, wait_err) = match child.wait() {
            Ok(s) => (s.code(), None),
            Err(e) => (None, Some(e.to_string())),
        };
        let files = report_files(&dir, started);
        let crash_file = files.iter().any(|f| f.fresh && f.name != "fefw.log");
        let crashed = crash_file || code.map_or(true, |c| c != 0);
        let message = match (&wait_err, code) {
            (Some(e), _) => format!("Lost track of fefw.exe: {e}"),
            (None, Some(0)) => "fefw.exe exited normally.".to_string(),
            (None, Some(4)) => "fefw.exe stopped before the game started: this processor lacks FMA3, AVX or CMPXCHG16B, which the game needs (an Intel Haswell or AMD Piledriver, 2012-2013, or newer). fefw_fatal.txt says which.".to_string(),
            (None, Some(c)) => format!("fefw.exe exited with code {} (0x{:08X}).", c, c as u32),
            (None, None) => "fefw.exe was ended by the system.".to_string(),
        };
        set_status(&app2, Status { state: "exited".into(), code, crashed, files, message });
        if minimize {
            if let Some(w) = app2.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }
    });
    Ok(())
}

pub fn open_with_shell(path: &Path) -> Result<(), String> {
    let mut c = Command::new(if cfg!(windows) { "explorer.exe" } else { "xdg-open" });
    crate::setup::host_env(&mut c);
    c.arg(path).spawn().map(|_| ()).map_err(|e| format!("Could not open {}: {}", path.display(), e))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_runner_setting_is_split_into_words() {
        if cfg!(windows) {
            assert!(runner("wine").unwrap().is_empty());
        } else {
            assert_eq!(runner("  env WINEPREFIX=/p  wine ").unwrap(), ["env", "WINEPREFIX=/p", "wine"]);
            assert_eq!(runner(r#""/s/Proton - Experimental/proton" run"#).unwrap(), ["/s/Proton - Experimental/proton", "run"]);
            assert_eq!(runner(r"/a\ b/wine '' x").unwrap(), ["/a b/wine", "", "x"]);
            assert!(is_umu(&words("env GAMEID=1 /usr/bin/umu-run")));
            assert!(!is_umu(&words("wine")));
        }
    }
}
