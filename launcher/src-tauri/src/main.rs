// SPDX-License-Identifier: GPL-3.0-or-later
//! Launcher for the fefw recomp (fefw.exe). It works on a checkout of this repository: it reads the player's own dump, runs the
//! repository's own lift and build scripts on this PC, writes fefw.ini in a run folder and starts the game. It ships no game files.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod dump;
mod ini;
mod nup;
mod play;
mod settings;
mod setup;
mod update;
mod xci;

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::Arc;
use tauri::Emitter;
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

/// The launcher's own options, in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Config {
    /// The repository checkout. Empty: found from the launcher's own location.
    repo_dir: String,
    /// The dump folder the player picked (exefs/ and romfs/, or base/ and updates/).
    dump_dir: String,
    /// Where fefw.ini, controls.ini, the log and the saves live. Empty: <repo>/work/play.
    game_dir: String,
    /// Build processes. 0: one fewer than the CPU threads.
    jobs: u32,
    minimize_on_play: bool,
    /// do not ask GitHub for a newer release when the launcher starts
    skip_update_check: bool,
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

fn load_config(app: &AppHandle) -> Config {
    config_path(app).ok().and_then(|p| fs::read_to_string(p).ok()).and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

/// The release package's tools/ folder, when the launcher runs from the release zip (tools/build/fefw_build.py next to it).
fn package_tools() -> Option<PathBuf> {
    let t = exe_dir().join("tools");
    t.join("build").join("fefw_build.py").is_file().then_some(t)
}

fn is_repo(dir: &Path) -> bool {
    dir.join("recomp").join("games").join("fefw").join("config.py").is_file() && dir.join("runtime").join("CMakeLists.txt").is_file()
}

/// The checkout the launcher runs in: the configured one, else the first parent of the launcher's exe that is one.
fn repo_of(cfg: &Config) -> Option<PathBuf> {
    if package_tools().is_some() {
        // the release package: the install folder plays the checkout's part
        return Some(if cfg.game_dir.trim().is_empty() { exe_dir() } else { PathBuf::from(cfg.game_dir.trim()) });
    }
    if !cfg.repo_dir.trim().is_empty() {
        let p = PathBuf::from(cfg.repo_dir.trim());
        return is_repo(&p).then_some(p);
    }
    let exe = std::env::current_exe().ok()?;
    exe.ancestors().skip(1).take(8).find(|d| is_repo(d)).map(|d| d.to_path_buf())
}

fn game_dir_of(cfg: &Config, repo: &Path) -> PathBuf {
    if cfg.game_dir.trim().is_empty() {
        if package_tools().is_some() {
            return exe_dir();
        }
        repo.join("work").join("play")
    } else {
        PathBuf::from(cfg.game_dir.trim())
    }
}

fn need_repo(cfg: &Config) -> Result<PathBuf, String> {
    repo_of(cfg).ok_or_else(|| "The repository was not found. Build the launcher inside a checkout of this project, or set the repository folder in Settings.".to_string())
}

/// Setup's paths from the configuration, when the dump is usable.
fn paths(cfg: &Config) -> Result<setup::Paths, String> {
    let repo = need_repo(cfg)?;
    let report = dump::scan(&cfg.dump_dir, &builds_of(&repo));
    let chosen = report.chosen.ok_or_else(|| {
        if cfg.dump_dir.trim().is_empty() { "Choose your game files first.".to_string() } else { format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")) }
    })?;
    let game_dir = game_dir_of(cfg, &repo);
    Ok(setup::Paths {
        repo,
        exefs: PathBuf::from(chosen.exefs),
        romfs: PathBuf::from(chosen.romfs),
        dlc: report.dlc_dir.map(PathBuf::from),
        game_dir,
        package: package_tools(),
    })
}

/// The supported builds of main: the package's dump_hashes.json, else the checkout's recomp/games/fefw/config.py.
fn builds_of(repo: &Path) -> Vec<(String, String)> {
    if let Some(t) = package_tools() {
        let v: serde_json::Value = fs::read_to_string(t.join("build").join("dump_hashes.json")).ok().and_then(|x| serde_json::from_str(&x).ok()).unwrap_or_default();
        if let (Some(id), Some(ver)) = (v["main_build_id"].as_str(), v["version"].as_str()) {
            return vec![(id.to_uppercase(), ver.to_string())];
        }
        return vec![];
    }
    dump::supported_builds(repo)
}

fn jobs_of(cfg: &Config) -> usize {
    if cfg.jobs > 0 {
        return cfg.jobs as usize;
    }
    std::thread::available_parallelism().map(|n| n.get().saturating_sub(1)).unwrap_or(2).max(1)
}

#[derive(Serialize)]
struct Overview {
    repo_dir: String,
    repo_ok: bool,
    game_dir: String,
    exe: String,
    exe_exists: bool,
}

#[tauri::command]
fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
fn save_config(app: AppHandle, config: Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    settings::write_atomic(&config_path(&app)?, &text).map_err(|e| format!("cannot save launcher settings: {e}"))
}

#[tauri::command]
fn get_overview(app: AppHandle) -> Overview {
    let cfg = load_config(&app);
    match repo_of(&cfg) {
        Some(repo) => {
            let exe = if package_tools().is_some() { game_dir_of(&cfg, &repo).join("fefw.exe") } else { setup::exe_path(&repo) };
            Overview {
                repo_dir: dump::fwd(&repo),
                repo_ok: true,
                game_dir: dump::fwd(&game_dir_of(&cfg, &repo)),
                exe_exists: exe.is_file(),
                exe: dump::fwd(&exe),
            }
        }
        None => Overview { repo_dir: cfg.repo_dir.clone(), repo_ok: false, game_dir: String::new(), exe: String::new(), exe_exists: false },
    }
}

#[tauri::command]
async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title(title);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[tauri::command]
async fn pick_nup(app: AppHandle, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title("Choose your .nup package").add_filter("NUP package", &["nup"]);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

// Serialize imports with setup startup: neither may change inputs beneath the other.
static IMPORT_BUSY: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
struct ImportGuard;
impl ImportGuard {
    fn acquire() -> Result<Self, String> {
        IMPORT_BUSY.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .map(|_| Self).map_err(|_| "A game import is already running.".into())
    }
}
impl Drop for ImportGuard {
    fn drop(&mut self) { IMPORT_BUSY.store(false, Ordering::SeqCst); }
}

#[derive(Serialize, Deserialize)]
struct XciSelection {
    image: String,
    update: Option<String>,
    keys: String,
    tool: String,
}

#[tauri::command]
async fn pick_xci(app: AppHandle) -> Option<XciSelection> {
    let pick = |title: &str, extensions: &[&str]| app.dialog().file().set_title(title)
        .add_filter(title, extensions).blocking_pick_file().and_then(|p| p.into_path().ok());
    let image = pick("Choose your XCI cartridge dump", &["xci"])?;
    let tool = pick("Choose hactool (required for XCI import)", &["exe"])?;
    let keys = pick("Choose your own prod.keys", &["keys"])?;
    let update = pick("Optional: choose the matching update NSP (Cancel to skip)", &["nsp"]);
    Some(XciSelection { image: dump::fwd(&image), update: update.map(|p| dump::fwd(&p)),
        keys: dump::fwd(&keys), tool: dump::fwd(&tool) })
}

#[derive(Serialize, Clone)]
struct XciProgress { message: String }

#[tauri::command]
async fn import_xci(app: AppHandle, state: State<'_, Arc<setup::Shared>>, selection: XciSelection) -> Result<String, String> {
    let guard = ImportGuard::acquire()?;
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Pause it first.".into());
    }
    let cfg = load_config(&app);
    let repo = need_repo(&cfg)?;
    let builds = builds_of(&repo);
    let parent = if package_tools().is_some() { game_dir_of(&cfg, &repo).join("xci_dump") }
        else { repo.join("game").join("xci_dump") };
    let inputs = xci::Inputs { image: selection.image.into(), update: selection.update.map(PathBuf::from),
        keys: selection.keys.into(), tool: selection.tool.into() };
    let shared = state.inner().clone();
    shared.cancel.store(false, Ordering::SeqCst);
    let out = tauri::async_runtime::spawn_blocking(move || {
        let _guard = guard;
        let out = xci::extract(&inputs, &parent, &builds, &mut |message| {
            let _ = app.emit("xci-progress", XciProgress { message: message.into() });
        }, &|| shared.cancel.load(Ordering::SeqCst), &shared.pid)?;
        let report = dump::scan(&dump::fwd(&out), &builds);
        if report.chosen.is_none() {
            return Err(format!("The extracted dump did not pass validation: {}", report.errors.join(" ")));
        }
        Ok(dump::fwd(&out))
    }).await.map_err(|e| e.to_string())?;
    out
}

#[derive(Serialize, Clone)]
struct NupProgress {
    done: u64,
    total: u64,
    file: String,
}

/// Unpacks a .nup (https://github.com/a55uka/nup) into `<repo>/game/nup_dump` (base/ and updates/<version>/, the layout of a dump
/// folder) and returns that folder. Emits "nup-progress". A finished unpack of the same package is reused.
#[tauri::command]
async fn import_nup(app: AppHandle, state: State<'_, Arc<setup::Shared>>, path: String) -> Result<String, String> {
    let guard = ImportGuard::acquire()?;
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Pause it first.".into());
    }
    let cfg = load_config(&app);
    let repo = need_repo(&cfg)?;
    let out = if package_tools().is_some() { game_dir_of(&cfg, &repo).join("nup_dump") } else { repo.join("game").join("nup_dump") };
    let src = PathBuf::from(path.trim());
    let shared = state.inner().clone();
    shared.cancel.store(false, Ordering::SeqCst);
    let emitter = app.clone();
    let res = tauri::async_runtime::spawn_blocking(move || {
        let _guard = guard;
        let mut last = 0u64;
        nup::extract(
            &src,
            &out,
            &mut |done, total, file| {
                if total == 0 || done == total || done - last >= total / 100 {
                    last = done;
                    let _ = emitter.emit("nup-progress", NupProgress { done, total, file: file.to_string() });
                }
            },
            &|| shared.cancel.load(Ordering::SeqCst),
        )
    })
    .await
    .map_err(|e| e.to_string())??;
    Ok(dump::fwd(&res))
}

#[tauri::command]
fn validate_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    Ok(dump::scan(&path, &builds_of(&need_repo(&load_config(&app))?)))
}

#[tauri::command]
fn save_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    let mut cfg = load_config(&app);
    let report = dump::scan(&path, &builds_of(&need_repo(&cfg)?));
    if report.chosen.is_none() {
        return Err(report.errors.join(" "));
    }
    cfg.dump_dir = report.path.clone();
    save_config(app, cfg)?;
    Ok(report)
}

#[tauri::command]
fn read_settings(app: AppHandle) -> Result<settings::SettingsDump, String> {
    let cfg = load_config(&app);
    Ok(settings::read(&game_dir_of(&cfg, &need_repo(&cfg)?)))
}

#[tauri::command]
fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    let cfg = load_config(&app);
    settings::apply(&game_dir_of(&cfg, &need_repo(&cfg)?), &edits)
}

#[derive(Serialize)]
struct SetupStatus {
    progress: setup::Progress,
    steps: Vec<setup::StepState>,
    complete: bool,
    tools: Vec<setup::Tool>,
    log: Vec<String>,
    jobs: usize,
    /// running from the release package (no build tools needed)
    package: bool,
}

#[tauri::command]
fn setup_status(app: AppHandle, state: State<Arc<setup::Shared>>) -> SetupStatus {
    let cfg = load_config(&app);
    let mut progress = state.progress.lock().unwrap().clone();
    if progress.state.is_empty() {
        progress.state = "idle".into();
    }
    let running = (progress.state == "running").then(|| progress.step.clone());
    let steps = paths(&cfg).map(|p| setup::steps(&p, running.as_deref())).unwrap_or_default();
    SetupStatus {
        complete: !steps.is_empty() && steps.iter().all(|s| s.state == "done"),
        steps,
        progress,
        tools: setup::check_tools(package_tools().is_some()),
        package: package_tools().is_some(),
        log: state.log.lock().unwrap().iter().cloned().collect(),
        jobs: jobs_of(&cfg),
    }
}

#[tauri::command]
fn start_setup(app: AppHandle, state: State<Arc<setup::Shared>>) -> Result<(), String> {
    let _guard = ImportGuard::acquire()?;
    let cfg = load_config(&app);
    let p = paths(&cfg)?;
    {
        let mut pr = state.progress.lock().unwrap();
        if pr.state == "running" {
            return Err("Setup is already running.".into());
        }
        *pr = setup::Progress { state: "running".into(), step: "python".into(), message: "Starting...".into(), ..Default::default() };
    }
    let shared = state.inner().clone();
    shared.cancel.store(false, Ordering::SeqCst);
    let jobs = jobs_of(&cfg);
    std::thread::spawn(move || {
        let a1 = app.clone();
        let a2 = app.clone();
        let emit = move |p: setup::Progress| {
            let _ = a1.emit("setup-status", p);
        };
        let emit_log = move |lines: Vec<String>| {
            let _ = a2.emit("setup-log", lines);
        };
        let end = setup::run(&shared, &p, jobs, &emit, &emit_log);
        let _ = app.emit("setup-status", end);
    });
    Ok(())
}

#[tauri::command]
fn cancel_setup(state: State<Arc<setup::Shared>>) {
    state.cancel.store(true, Ordering::SeqCst);
    setup::kill_tree(state.pid.load(Ordering::SeqCst));
}

#[tauri::command]
fn play(app: AppHandle, state: State<Arc<setup::Shared>>) -> Result<(), String> {
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Wait for it or pause it first.".into());
    }
    let cfg = load_config(&app);
    let p = paths(&cfg)?;
    if !setup::complete(&p) {
        return Err("Setup is not finished: open Setup and press Set up.".into());
    }
    settings::ensure_play_paths(&p.game_dir, &p)?;
    play::start(&app, &p.exe(), &p.game_dir, cfg.minimize_on_play)
}

#[tauri::command]
fn game_status(state: State<play::PlayState>) -> play::Status {
    state.0.lock().unwrap().clone()
}

#[tauri::command]
fn open_game_file(app: AppHandle, name: String) -> Result<(), String> {
    if !play::REPORT_FILES.contains(&name.as_str()) {
        return Err("That file cannot be opened from here.".into());
    }
    let cfg = load_config(&app);
    let path = game_dir_of(&cfg, &need_repo(&cfg)?).join(&name);
    if !path.is_file() {
        return Err(format!("{name} does not exist."));
    }
    play::open_with_shell(&path)
}

#[tauri::command]
fn open_game_dir(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg, &need_repo(&cfg)?);
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    play::open_with_shell(&dir)
}

/// Opens <run folder>/mods ([fs] mods), creating it: RomFS mods go in mods/<name>/romfs.
#[tauri::command]
fn open_mods_dir(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg, &need_repo(&cfg)?).join("mods");
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    play::open_with_shell(&dir)
}

#[tauri::command]
fn open_setup_log(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    let repo = need_repo(&cfg)?;
    let log = if package_tools().is_some() { game_dir_of(&cfg, &repo).join("setup.log") } else { repo.join("work").join("setup.log") };
    if !log.is_file() {
        return Err("There is no setup log yet.".into());
    }
    play::open_with_shell(&log)
}

#[tauri::command]
async fn check_update(app: AppHandle) -> Result<update::Check, String> {
    // FEFW_LAUNCHER_PRETEND_VERSION: test the update path against the published release from an older-looking launcher
    let current = std::env::var("FEFW_LAUNCHER_PRETEND_VERSION").ok().filter(|v| update::parse_version(v).is_some()).unwrap_or_else(|| app.package_info().version.to_string());
    let package = package_tools().is_some();
    tauri::async_runtime::spawn_blocking(move || update::check(&current, package)).await.map_err(|e| e.to_string())?
}

#[tauri::command]
fn update_status(state: State<Arc<update::Shared>>) -> update::Status {
    let mut s = state.status.lock().unwrap().clone();
    if s.state.is_empty() {
        s.state = "idle".into();
    }
    s
}

#[tauri::command]
fn start_update(
    app: AppHandle,
    state: State<Arc<update::Shared>>,
    setup_state: State<Arc<setup::Shared>>,
    play_state: State<play::PlayState>,
    release: update::Release,
) -> Result<(), String> {
    if package_tools().is_none() {
        return Err("Updates are for the release package. In a checkout, use git.".into());
    }
    if setup_state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Wait for it or pause it first.".into());
    }
    if play_state.0.lock().unwrap().state == "running" {
        return Err("The game is running. Close it first.".into());
    }
    {
        let mut s = state.status.lock().unwrap();
        if s.state == "running" {
            return Err("An update is already running.".into());
        }
        *s = update::Status { state: "running".into(), phase: "download".into(), message: "Starting".into(), version: release.version.clone(), ..Default::default() };
    }
    let cfg = load_config(&app);
    let install = exe_dir();
    let game_dir = repo_of(&cfg).map(|r| game_dir_of(&cfg, &r)).unwrap_or_else(|| install.clone());
    let shared = state.inner().clone();
    shared.cancel.store(false, Ordering::SeqCst);
    std::thread::spawn(move || {
        let a = app.clone();
        let emit = move |s: update::Status| {
            let _ = a.emit("update-status", s);
        };
        let run = update::Run { shared: &shared, install, game_dir, emit: &emit };
        let end = match run.run(&release) {
            Ok(s) => s,
            Err(e) => update::Status { state: "error".into(), message: e, version: release.version.clone(), ..Default::default() },
        };
        *shared.status.lock().unwrap() = end.clone();
        let _ = app.emit("update-status", end);
    });
    Ok(())
}

#[tauri::command]
fn cancel_update(state: State<Arc<update::Shared>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

/// Starts the (new) launcher from the install folder and closes this one.
#[tauri::command]
fn restart_launcher(app: AppHandle) -> Result<(), String> {
    let exe = exe_dir().join("fefw-launcher.exe");
    std::process::Command::new(&exe).current_dir(exe_dir()).spawn().map_err(|e| format!("cannot start {}: {e}", exe.display()))?;
    app.exit(0);
    Ok(())
}

fn main() {
    if package_tools().is_some() {
        update::cleanup(&exe_dir());
    }
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(play::PlayState::default())
        .manage(Arc::new(setup::Shared::default()))
        .manage(Arc::new(update::Shared::default()))
        .on_window_event(|window, event| {
            // closing the window pauses a running setup rather than leaving its compilers behind
            if let tauri::WindowEvent::Destroyed = event {
                let st = window.state::<Arc<setup::Shared>>();
                st.cancel.store(true, Ordering::SeqCst);
                setup::kill_tree(st.pid.load(Ordering::SeqCst));
            }
        })
        .invoke_handler(tauri::generate_handler![
            get_config,
            save_config,
            get_overview,
            pick_folder,
            pick_nup,
            pick_xci,
            import_xci,
            import_nup,
            validate_dump,
            save_dump,
            read_settings,
            write_settings,
            setup_status,
            start_setup,
            cancel_setup,
            play,
            game_status,
            open_game_file,
            open_game_dir,
            open_mods_dir,
            open_setup_log,
            check_update,
            update_status,
            start_update,
            cancel_update,
            restart_launcher,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}

#[cfg(test)]
mod import_tests {
    use super::*;
    #[test]
    fn imports_and_setup_startup_are_mutually_exclusive_and_release_on_error() {
        let guard = ImportGuard::acquire().unwrap();
        assert!(ImportGuard::acquire().is_err());
        drop(guard);
        let _next = ImportGuard::acquire().unwrap();
    }
}
