// SPDX-License-Identifier: GPL-3.0-or-later
//! Optional XCI import through a player-supplied hactool. No keys or helper binaries are shipped.
//! Helper output can contain key material: never log it or include it in an error.
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const TITLE: u64 = 0x04007EF01F8AE000;

pub struct Inputs {
    pub image: PathBuf,
    pub update: Option<PathBuf>,
    pub keys: PathBuf,
    pub tool: PathBuf,
}

struct Program {
    file: PathBuf,
    patch: bool,
}

fn field<'a>(text: &'a str, name: &str) -> Option<&'a str> {
    text.lines()
        .filter_map(|line| line.split_once(':'))
        .find(|(key, _)| key.trim() == name)
        .map(|(_, value)| value.trim())
}

fn program(file: PathBuf, info: &str) -> Option<Program> {
    let id = u64::from_str_radix(field(info, "Title ID")?, 16).ok()?;
    if id & !0x800 != TITLE || field(info, "Content Type")? != "Program" {
        return None;
    }
    // Match the section label, not incidental BKTR text in warnings or filenames.
    Some(Program {
        file,
        patch: info.lines().any(|line| {
            line.split_once(':').is_some_and(|(key, value)| {
                key.trim() == "Partition Type" && value.trim() == "Patch RomFS"
            })
        }),
    })
}

fn run(
    tool: &Path,
    keys: &Path,
    args: &[String],
    info: bool,
    cancel: &dyn Fn() -> bool,
    pid: &AtomicU32,
) -> Result<String, String> {
    if cancel() {
        return Err("XCI import cancelled.".into());
    }
    let mut command = Command::new(tool);
    command
        .arg("--disablekeywarns")
        .arg("--keyset")
        .arg(keys)
        .args(args);
    // Info output is needed only to select Program NCAs. It is never persisted.
    command
        .stdout(if info { Stdio::piped() } else { Stdio::null() })
        .stderr(Stdio::null());
    crate::setup::quiet(&mut command);
    let mut child = command
        .spawn()
        .map_err(|_| "Cannot start hactool. Choose a working hactool executable.".to_string())?;
    pid.store(child.id(), Ordering::SeqCst);
    let reader = child.stdout.take().map(|stdout| {
        std::thread::spawn(move || {
            let mut bytes = Vec::new();
            // Bound the info report, drain excess output so the helper never blocks on a full pipe.
            let mut stdout = stdout;
            let mut block = [0u8; 8192];
            while let Ok(n) = stdout.read(&mut block) {
                if n == 0 {
                    break;
                }
                if bytes.len() + n <= 1024 * 1024 {
                    bytes.extend_from_slice(&block[..n]);
                }
            }
            bytes
        })
    });
    let status = loop {
        if cancel() {
            crate::setup::kill_tree(child.id());
            let _ = child.kill();
            break child.wait();
        }
        match child.try_wait() {
            Ok(Some(status)) => break Ok(status),
            Ok(None) => std::thread::sleep(Duration::from_millis(100)),
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                break Err(error);
            }
        }
    };
    pid.store(0, Ordering::SeqCst);
    let stdout = reader.and_then(|r| r.join().ok()).unwrap_or_default();
    if cancel() {
        return Err("XCI import cancelled.".into());
    }
    if !status
        .map_err(|_| "Cannot wait for hactool.".to_string())?
        .success()
    {
        return Err("hactool could not read or extract this archive. Check that your keys and hactool build support this game, and that the base and update match. Helper output is not logged because it may contain keys.".into());
    }
    Ok(String::from_utf8_lossy(&stdout).into_owned())
}

fn arg(name: &str, path: &Path) -> String {
    format!("--{name}={}", path.to_string_lossy())
}

fn files(dir: &Path) -> Result<Vec<PathBuf>, String> {
    let mut out = Vec::new();
    for entry in
        fs::read_dir(dir).map_err(|_| "hactool did not create the archive contents.".to_string())?
    {
        let entry = entry.map_err(|e| e.to_string())?;
        let p = entry.path();
        if entry.file_type().map_err(|e| e.to_string())?.is_file()
            && p.extension()
                .is_some_and(|ext| ext.eq_ignore_ascii_case("nca"))
        {
            out.push(p);
        }
    }
    out.sort();
    Ok(out)
}

fn supported(dir: &Path, builds: &[(String, String)]) -> bool {
    let mut header = [0; 0x54];
    if fs::File::open(dir.join("main"))
        .and_then(|mut f| f.read_exact(&mut header))
        .is_err()
        || &header[..4] != b"NSO0"
    {
        return false;
    }
    let id: String = header[0x40..0x54]
        .iter()
        .map(|b| format!("{b:02X}"))
        .collect();
    builds
        .iter()
        .any(|(build, _)| build.eq_ignore_ascii_case(&id))
}

/// Imports to a new directory under `parent`; never removes or overwrites another import.
/// The full merged RomFS is produced with --basenca for a patch, not an update-only overlay.
/// The caller validates with dump::scan before offering the folder for setup.
pub fn extract(
    inputs: &Inputs,
    parent: &Path,
    builds: &[(String, String)],
    progress: &mut dyn FnMut(&str),
    cancel: &dyn Fn() -> bool,
    pid: &AtomicU32,
) -> Result<PathBuf, String> {
    for (file, label) in [
        (&inputs.image, "XCI"),
        (&inputs.keys, "prod.keys"),
        (&inputs.tool, "hactool"),
    ] {
        if !file.is_file() {
            return Err(format!("Choose an existing {label} file."));
        }
    }
    if !inputs
        .image
        .extension()
        .is_some_and(|e| e.eq_ignore_ascii_case("xci"))
    {
        return Err("Choose an uncompressed .xci file.".into());
    }
    if let Some(update) = &inputs.update {
        if !update.is_file()
            || !update
                .extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("nsp"))
        {
            return Err("Choose an uncompressed update .nsp file, or skip the update.".into());
        }
    }
    if builds.is_empty() {
        return Err("No supported game builds were found in this package.".into());
    }
    fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let work = parent.join(format!("import-{stamp}.partial"));
    // create_dir, rather than create_dir_all, guarantees ownership of this scratch directory.
    fs::create_dir(&work).map_err(|e| e.to_string())?;
    let result = extract_inner(inputs, &work, builds, progress, &mut |args, info| {
        run(&inputs.tool, &inputs.keys, args, info, cancel, pid)
    });
    match result {
        Ok(()) => {
            let finished = parent.join(format!("import-{stamp}"));
            fs::rename(&work, &finished).map_err(|e| e.to_string())?;
            Ok(finished)
        }
        Err(error) => {
            // Only the directory this call created; input archives and older imports stay intact.
            let _ = fs::remove_dir_all(&work);
            Err(error)
        }
    }
}

fn extract_inner(
    inputs: &Inputs,
    work: &Path,
    builds: &[(String, String)],
    progress: &mut dyn FnMut(&str),
    invoke: &mut dyn FnMut(&[String], bool) -> Result<String, String>,
) -> Result<(), String> {
    let ncadir = work.join("nca");
    fs::create_dir(&ncadir).map_err(|e| e.to_string())?;
    progress("Extracting the XCI secure partition (this can take several minutes)...");
    invoke(
        &[
            "-x".into(),
            "-t".into(),
            "xci".into(),
            arg("securedir", &ncadir),
            inputs.image.to_string_lossy().into(),
        ],
        false,
    )?;
    let mut ncas = files(&ncadir)?;
    if let Some(update) = &inputs.update {
        let updatedir = work.join("update_nca");
        fs::create_dir(&updatedir).map_err(|e| e.to_string())?;
        progress("Extracting the optional update NSP...");
        invoke(
            &[
                "-x".into(),
                "-t".into(),
                "pfs0".into(),
                arg("outdir", &updatedir),
                update.to_string_lossy().into(),
            ],
            false,
        )?;
        ncas.extend(files(&updatedir)?);
    }
    let mut programs = Vec::new();
    for nca in ncas {
        progress("Finding Fire Emblem program archives...");
        let info = invoke(
            &[
                "-i".into(),
                "-t".into(),
                "nca".into(),
                nca.to_string_lossy().into(),
            ],
            true,
        )?;
        if let Some(program) = program(nca, &info) {
            programs.push(program);
        }
    }
    let bases: Vec<_> = programs.iter().filter(|p| !p.patch).collect();
    if bases.len() != 1 {
        return Err("Expected exactly one Fire Emblem base Program NCA. Use a single-game XCI and, optionally, one matching update NSP.".into());
    }
    let base = bases[0];
    let mut selected = Vec::new();
    for (index, p) in programs.iter().enumerate() {
        let exefs = work.join(format!("exefs-{index}"));
        fs::create_dir(&exefs).map_err(|e| e.to_string())?;
        progress("Checking the extracted executable against supported game versions...");
        let mut args = vec![
            "-x".into(),
            "-t".into(),
            "nca".into(),
            arg("exefsdir", &exefs),
        ];
        if p.patch {
            args.push(arg("basenca", &base.file));
        }
        args.push(p.file.to_string_lossy().into());
        invoke(&args, false)?;
        if supported(&exefs, builds) {
            selected.push((p, exefs));
        }
    }
    if selected.len() != 1 {
        return Err("The XCI/update does not provide exactly one supported executable. Supply the supported update as an NSP, or use an already extracted dump/NUP. No files were selected for setup.".into());
    }
    let (chosen, exefs) = &selected[0];
    let romfs = work.join("romfs");
    fs::create_dir(&romfs).map_err(|e| e.to_string())?;
    progress("Extracting the full RomFS with the supported update applied...");
    let mut args = vec![
        "-x".into(),
        "-t".into(),
        "nca".into(),
        arg("romfsdir", &romfs),
    ];
    if chosen.patch {
        args.push(arg("basenca", &base.file));
    }
    args.push(chosen.file.to_string_lossy().into());
    invoke(&args, false)?;
    fs::rename(exefs, work.join("exefs")).map_err(|e| e.to_string())?;
    if fs::read_dir(&romfs)
        .map_err(|e| e.to_string())?
        .next()
        .is_none()
    {
        return Err(
            "hactool did not extract the RomFS. Check extractor support and the base/update pair."
                .into(),
        );
    }
    let report = crate::dump::scan(&crate::dump::fwd(work), builds);
    if report.chosen.is_none() {
        return Err(report.errors.join(" "));
    }
    // Remove only this import's temporary encrypted copies and unselected ExeFS probes.
    for entry in fs::read_dir(work).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let name = entry.file_name();
        if name != "exefs" && name != "romfs" {
            fs::remove_dir_all(entry.path()).map_err(|e| e.to_string())?;
        }
    }
    progress("XCI extraction finished. Checking the dump...");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn selects_only_this_games_program_and_recognizes_patches() {
        assert!(program(
            "a".into(),
            "Title ID: 010051f0207b2000\nContent Type: Program"
        )
        .is_none());
        assert!(program(
            "a".into(),
            "Title ID: 04007ef01f8ae000\nContent Type: Control"
        )
        .is_none());
        assert!(
            !program(
                "a".into(),
                "Title ID: 04007ef01f8ae000\nContent Type: Program"
            )
            .unwrap()
            .patch
        );
        assert!(
            program(
                "a".into(),
                "Title ID: 04007ef01f8ae800\nContent Type: Program\nPartition Type: Patch RomFS"
            )
            .unwrap()
            .patch
        );
        assert!(program("a".into(), "invalid").is_none());
    }
    #[test]
    fn supported_requires_a_complete_nso_header_and_matching_build_id() {
        let dir = std::env::temp_dir().join(format!(
            "fefw-xci-test-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&dir).unwrap();
        let builds = vec![("AB".repeat(20), "131072".into())];
        assert!(!supported(&dir, &builds));
        let mut header = vec![0; 0x54];
        header[..4].copy_from_slice(b"NSO0");
        header[0x40..].fill(0xab);
        fs::write(dir.join("main"), &header).unwrap();
        assert!(supported(&dir, &builds));
        header[0x40] = 0;
        fs::write(dir.join("main"), header).unwrap();
        assert!(!supported(&dir, &builds));
        fs::remove_dir_all(dir).unwrap();
    }
    struct Scratch(PathBuf);
    impl Scratch {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "fefw-import-test-{}-{}",
                std::process::id(),
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            ));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn fake_extract(args: &[String], info: bool, matching: bool) -> Result<String, String> {
        let destination = |name: &str| {
            args.iter()
                .find_map(|a| a.strip_prefix(&format!("--{name}=")))
                .map(PathBuf::from)
        };
        let source = args.last().unwrap();
        if info {
            let patch = source.ends_with("patch.nca");
            return Ok(format!(
                "Title ID: 04007ef01f8ae000\nContent Type: Program\nPartition Type: {}",
                if patch { "Patch RomFS" } else { "RomFS" }
            ));
        }
        if let Some(dir) = destination("securedir") {
            fs::write(dir.join("base.nca"), b"synthetic").unwrap();
        } else if let Some(dir) = destination("outdir") {
            fs::write(dir.join("patch.nca"), b"synthetic").unwrap();
        } else if let Some(dir) = destination("exefsdir") {
            let mut header = vec![0; 0x54];
            header[..4].copy_from_slice(b"NSO0");
            if matching && source.ends_with("patch.nca") {
                header[0x40..].fill(0xab);
            }
            fs::write(dir.join("main"), header).unwrap();
            for name in ["sdk", "subsdk0", "subsdk1", "subsdk2", "rtld"] {
                fs::write(dir.join(name), b"synthetic").unwrap();
            }
        } else if let Some(dir) = destination("romfsdir") {
            assert!(args
                .iter()
                .any(|a| a.starts_with("--basenca=") && a.ends_with("base.nca")));
            fs::write(dir.join("asset"), b"synthetic").unwrap();
        }
        Ok(String::new())
    }

    #[test]
    fn update_import_uses_base_for_exefs_and_romfs_and_cleans_intermediates() {
        let scratch = Scratch::new();
        let inputs = Inputs {
            image: "path with spaces/game.xci".into(),
            update: Some("update.nsp".into()),
            keys: "prod.keys".into(),
            tool: "hactool.exe".into(),
        };
        let builds = vec![("AB".repeat(20), "131072".into())];
        let mut calls = Vec::new();
        extract_inner(
            &inputs,
            &scratch.0,
            &builds,
            &mut |_| {},
            &mut |args, info| {
                calls.push(args.to_vec());
                fake_extract(args, info, true)
            },
        )
        .unwrap();
        assert!(scratch.0.join("romfs/asset").is_file());
        assert!(crate::dump::scan(&crate::dump::fwd(&scratch.0), &builds)
            .chosen
            .is_some());
        assert!(!scratch.0.join("nca").exists());
        assert!(!scratch.0.join("update_nca").exists());
        let probes: Vec<_> = calls
            .iter()
            .filter(|a| {
                a.iter().any(|v| v.starts_with("--exefsdir="))
                    && a.last().unwrap().ends_with("patch.nca")
            })
            .collect();
        assert_eq!(probes.len(), 1);
        assert!(probes[0].iter().any(|a| a.starts_with("--basenca=")));
        assert_eq!(calls[0].last().unwrap(), "path with spaces/game.xci");
    }

    #[test]
    fn unsupported_update_is_rejected_before_romfs_extraction() {
        let scratch = Scratch::new();
        let inputs = Inputs {
            image: "game.xci".into(),
            update: Some("update.nsp".into()),
            keys: "prod.keys".into(),
            tool: "hactool.exe".into(),
        };
        let error = extract_inner(
            &inputs,
            &scratch.0,
            &[("AB".repeat(20), "131072".into())],
            &mut |_| {},
            &mut |args, info| {
                assert!(!args.iter().any(|a| a.starts_with("--romfsdir=")));
                fake_extract(args, info, false)
            },
        )
        .unwrap_err();
        assert!(error.contains("supported executable"));
        assert!(!scratch.0.join("romfs").exists());
    }

    #[test]
    fn cancelled_import_cleans_its_scratch_and_preserves_inputs_and_older_imports() {
        let scratch = Scratch::new();
        for name in ["game.xci", "prod.keys", "hactool.exe"] {
            fs::write(scratch.0.join(name), b"private input").unwrap();
        }
        let parent = scratch.0.join("imports");
        fs::create_dir(&parent).unwrap();
        fs::write(parent.join("previous"), b"preserve").unwrap();
        let inputs = Inputs {
            image: scratch.0.join("game.xci"),
            update: None,
            keys: scratch.0.join("prod.keys"),
            tool: scratch.0.join("hactool.exe"),
        };
        let error = extract(
            &inputs,
            &parent,
            &[("AB".repeat(20), "131072".into())],
            &mut |_| {},
            &|| true,
            &AtomicU32::new(0),
        )
        .unwrap_err();
        assert!(error.contains("cancelled"));
        assert_eq!(fs::read_dir(&parent).unwrap().count(), 1);
        assert_eq!(fs::read(parent.join("previous")).unwrap(), b"preserve");
        assert_eq!(fs::read(inputs.keys).unwrap(), b"private input");
        assert_eq!(fs::read(inputs.image).unwrap(), b"private input");
    }
}
