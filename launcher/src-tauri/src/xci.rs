// SPDX-License-Identifier: GPL-3.0-or-later
//! Imports the player's XCI with the bundled, extraction-only Lichii archive reader.
use serde::Serialize;
use std::fs;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

pub struct Inputs {
    pub image: PathBuf,
    pub update: Option<PathBuf>,
    pub keys: PathBuf,
}

#[derive(Debug, Serialize, Clone, Default)]
pub struct Progress {
    pub message: String,
    pub done: u64,
    pub total: u64,
}

#[derive(Debug)]
enum Event {
    Progress(Progress),
    Complete(String),
    Error(String),
}

fn event(line: &str) -> Result<Event, String> {
    let v: serde_json::Value =
        serde_json::from_str(line).map_err(|_| "Invalid archive helper response.".to_string())?;
    match v["kind"].as_str() {
        Some("stage") => Ok(Event::Progress(Progress {
            message: v["message"].as_str().unwrap_or("Reading archive...").into(),
            ..Default::default()
        })),
        Some("progress") => {
            let done = v["done"].as_u64().ok_or("Invalid archive progress.")?;
            let total = v["total"].as_u64().ok_or("Invalid archive progress.")?;
            if done > total || total == 0 {
                return Err("Invalid archive progress range.".into());
            }
            Ok(Event::Progress(Progress {
                message: format!(
                    "Extracting game files: {}%",
                    (done as f64 * 100.0 / total as f64) as u64
                ),
                done,
                total,
            }))
        }
        Some("complete") => Ok(Event::Complete(
            v["build_id"]
                .as_str()
                .ok_or("Missing extracted build ID.")?
                .into(),
        )),
        Some("error") => Ok(Event::Error(
            v["message"]
                .as_str()
                .unwrap_or("Archive extraction failed.")
                .into(),
        )),
        _ => Err("Unknown archive helper response.".into()),
    }
}

fn run(
    helper: &Path,
    inputs: &Inputs,
    out: &Path,
    builds: &[(String, String)],
    progress: &mut dyn FnMut(Progress),
    cancel: &dyn Fn() -> bool,
    pid: &AtomicU32,
) -> Result<(), String> {
    if cancel() {
        return Err("XCI import cancelled.".into());
    }
    let mut cmd = Command::new(helper);
    cmd.arg("--xci")
        .arg(&inputs.image)
        .arg("--keys")
        .arg(&inputs.keys)
        .arg("--out")
        .arg(out);
    if let Some(update) = &inputs.update {
        cmd.arg("--update").arg(update);
    }
    for (id, _) in builds {
        cmd.arg("--build-id").arg(id);
    }
    cmd.stdout(Stdio::piped()).stderr(Stdio::null());
    crate::setup::quiet(&mut cmd);
    let mut child = cmd.spawn().map_err(|_| "Cannot start the bundled archive reader. Check the tools/xci files in your release package.".to_string())?;
    pid.store(child.id(), Ordering::SeqCst);
    let (tx, rx) = mpsc::sync_channel(32);
    let stdout = child.stdout.take().unwrap();
    let reader = std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines() {
            let parsed = line
                .map_err(|_| "Cannot read the archive helper response.".to_string())
                .and_then(|line| event(&line));
            if tx.send(parsed).is_err() {
                break;
            }
        }
    });
    let mut completed = None;
    let mut failure = None;
    let mut handle = |value: Result<Event, String>| match value {
        Ok(Event::Progress(p)) => progress(p),
        Ok(Event::Complete(id)) => completed = Some(id),
        Ok(Event::Error(error)) | Err(error) => failure = Some(error),
    };
    let status = loop {
        if cancel() {
            crate::setup::kill_tree(child.id());
            let _ = child.kill();
            break child.wait();
        }
        if let Ok(value) = rx.recv_timeout(Duration::from_millis(100)) {
            handle(value);
        }
        match child.try_wait() {
            Ok(Some(status)) => break Ok(status),
            Ok(None) => (),
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                break Err(error);
            }
        }
    };
    pid.store(0, Ordering::SeqCst);
    // Keep draining while the reader finishes: a bounded channel may still hold its final events.
    while let Ok(value) = rx.recv() {
        handle(value);
    }
    let _ = reader.join();
    if cancel() {
        return Err("XCI import cancelled.".into());
    }
    if let Some(error) = failure {
        return Err(error);
    }
    if !status
        .map_err(|_| "Cannot wait for archive reader.".to_string())?
        .success()
    {
        return Err(
            "Archive reader failed. Check your archive, keys and available disk space.".into(),
        );
    }
    if !completed.is_some_and(|id| {
        builds
            .iter()
            .any(|(supported, _)| supported.eq_ignore_ascii_case(&id))
    }) {
        return Err("The archive reader did not finish with a supported executable.".into());
    }
    Ok(())
}

fn attempt(
    parent: &Path,
    builds: &[(String, String)],
    extract: &mut dyn FnMut(&Path) -> Result<(), String>,
) -> Result<PathBuf, String> {
    fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let work = parent.join(format!("import-{stamp}.partial"));
    // Exclusive creation ensures cleanup is limited to the directory owned by this call.
    fs::create_dir(&work).map_err(|e| e.to_string())?;
    let result = extract(&work).and_then(|_| {
        let report = crate::dump::scan(&crate::dump::fwd(&work), builds);
        if report.chosen.is_none() {
            return Err(report.errors.join(" "));
        }
        if fs::read_dir(work.join("romfs"))
            .map_err(|e| e.to_string())?
            .next()
            .is_none()
        {
            return Err("The extracted RomFS is empty.".into());
        }
        if work.join(".private-keys").exists() {
            return Err("The archive reader did not clean up its temporary key files.".into());
        }
        let finished = parent.join(format!("import-{stamp}"));
        fs::rename(&work, &finished).map_err(|e| e.to_string())?;
        Ok(finished)
    });
    if result.is_err() {
        let _ = fs::remove_dir_all(&work);
    }
    result
}

/// No emulator is launched. The bundled helper only reads archives and writes this attempt's dump.
pub fn extract(
    helper: &Path,
    inputs: &Inputs,
    parent: &Path,
    builds: &[(String, String)],
    progress: &mut dyn FnMut(Progress),
    cancel: &dyn Fn() -> bool,
    pid: &AtomicU32,
) -> Result<PathBuf, String> {
    if !helper.is_file() {
        return Err("This package is missing tools/xci/fefw-archive.exe. Install a release containing the bundled archive reader.".into());
    }
    for (file, label) in [(&inputs.image, "XCI"), (&inputs.keys, "prod.keys")] {
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
    attempt(parent, builds, &mut |out| {
        run(helper, inputs, out, builds, progress, cancel, pid)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Scratch(PathBuf);
    impl Scratch {
        fn new() -> Self {
            let p = std::env::temp_dir().join(format!(
                "fefw-xci-test-{}-{}",
                std::process::id(),
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            ));
            fs::create_dir(&p).unwrap();
            Self(p)
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn fixture(out: &Path, matching: bool) {
        fs::create_dir(out.join("exefs")).unwrap();
        fs::create_dir(out.join("romfs")).unwrap();
        let mut h = vec![0; 0x54];
        h[..4].copy_from_slice(b"NSO0");
        h[0x40..].fill(if matching { 0xab } else { 0 });
        fs::write(out.join("exefs/main"), h).unwrap();
        for name in ["sdk", "subsdk0", "subsdk1", "subsdk2", "rtld"] {
            fs::write(out.join("exefs").join(name), b"fixture").unwrap();
        }
        fs::write(out.join("romfs/asset"), b"fixture").unwrap();
    }
    #[test]
    fn helper_protocol_checks_progress_completion_and_errors() {
        assert!(matches!(
            event(r#"{"kind":"stage","message":"Reading"}"#).unwrap(),
            Event::Progress(_)
        ));
        let Event::Progress(p) = event(r#"{"kind":"progress","done":5,"total":10}"#).unwrap()
        else {
            panic!()
        };
        assert_eq!(p.message, "Extracting game files: 50%");
        assert!(matches!(
            event(r#"{"kind":"complete","build_id":"AB"}"#).unwrap(),
            Event::Complete(_)
        ));
        assert!(matches!(
            event(r#"{"kind":"error","message":"Wrong keys"}"#).unwrap(),
            Event::Error(_)
        ));
        assert!(event(r#"{"kind":"progress","done":11,"total":10}"#).is_err());
        assert!(event("unexpected output").is_err());
    }
    #[test]
    fn validated_import_is_promoted_without_touching_older_imports() {
        let scratch = Scratch::new();
        fs::write(scratch.0.join("old"), b"preserve").unwrap();
        let out = attempt(
            &scratch.0,
            &[("AB".repeat(20), "131072".into())],
            &mut |out| {
                fixture(out, true);
                Ok(())
            },
        )
        .unwrap();
        assert!(out.join("romfs/asset").is_file());
        assert!(!out.to_string_lossy().ends_with(".partial"));
        assert_eq!(fs::read(scratch.0.join("old")).unwrap(), b"preserve");
    }
    #[test]
    fn unsupported_build_cancel_and_leftover_keys_do_not_leave_a_dump() {
        for case in 0..3 {
            let scratch = Scratch::new();
            fs::write(scratch.0.join("old"), b"preserve").unwrap();
            assert!(attempt(
                &scratch.0,
                &[("AB".repeat(20), "131072".into())],
                &mut |out| {
                    if case == 0 {
                        return Err("XCI import cancelled.".into());
                    }
                    fixture(out, case == 2);
                    if case == 2 {
                        fs::create_dir(out.join(".private-keys")).unwrap();
                    }
                    Ok(())
                }
            )
            .is_err());
            assert_eq!(fs::read_dir(&scratch.0).unwrap().count(), 1);
            assert_eq!(fs::read(scratch.0.join("old")).unwrap(), b"preserve");
        }
    }
    // Explicitly opt in with your own local archive/keys. Never part of normal CI.
    #[test]
    #[ignore]
    fn real_xci_helper_roundtrip() {
        let helper = PathBuf::from(std::env::var_os("FEFW_XCI_TEST_HELPER").expect("helper"));
        let image = PathBuf::from(std::env::var_os("FEFW_XCI_TEST_IMAGE").expect("image"));
        let keys = PathBuf::from(std::env::var_os("FEFW_XCI_TEST_KEYS").expect("keys"));
        let parent =
            PathBuf::from(std::env::var_os("FEFW_XCI_TEST_OUTPUT").expect("scratch output"));
        let scratch = Scratch(parent.join(format!(
                "rust-roundtrip-{}",
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            )));
        let inputs = Inputs {
            image,
            update: None,
            keys,
        };
        let original_size = fs::metadata(&inputs.image).unwrap().len();
        let mut last_progress = 0;
        let out = extract(
            &helper,
            &inputs,
            &scratch.0,
            &[(
                "DB93B0125F4850601D61C3EDDE012F8EB2ECFE77".into(),
                "131072".into(),
            )],
            &mut |p| {
                last_progress = last_progress.max(p.done);
            },
            &|| false,
            &AtomicU32::new(0),
        )
        .unwrap();
        assert!(last_progress > 0);
        assert!(out.join("exefs/main").is_file());
        assert!(!out.join(".private-keys").exists());
        assert_eq!(fs::metadata(inputs.image).unwrap().len(), original_size);
    }
    #[test]
    #[ignore]
    fn real_xci_helper_cancellation() {
        let helper = PathBuf::from(std::env::var_os("FEFW_XCI_TEST_HELPER").expect("helper"));
        let image = PathBuf::from(std::env::var_os("FEFW_XCI_TEST_IMAGE").expect("image"));
        let keys = PathBuf::from(std::env::var_os("FEFW_XCI_TEST_KEYS").expect("keys"));
        let parent =
            PathBuf::from(std::env::var_os("FEFW_XCI_TEST_OUTPUT").expect("scratch output"));
        let scratch = Scratch(
            parent.join(format!(
                "rust-cancel-{}",
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            )),
        );
        fs::create_dir_all(&scratch.0).unwrap();
        fs::write(scratch.0.join("previous"), b"preserve").unwrap();
        let cancelled = std::sync::atomic::AtomicBool::new(false);
        let inputs = Inputs {
            image,
            update: None,
            keys,
        };
        let key_time = fs::metadata(&inputs.keys).unwrap().modified().unwrap();
        let error = extract(
            &helper,
            &inputs,
            &scratch.0,
            &[(
                "DB93B0125F4850601D61C3EDDE012F8EB2ECFE77".into(),
                "131072".into(),
            )],
            &mut |p| {
                if p.done > 0 {
                    cancelled.store(true, Ordering::SeqCst);
                }
            },
            &|| cancelled.load(Ordering::SeqCst),
            &AtomicU32::new(0),
        )
        .unwrap_err();
        assert!(error.contains("cancelled"));
        assert_eq!(fs::read_dir(&scratch.0).unwrap().count(), 1);
        assert_eq!(fs::read(scratch.0.join("previous")).unwrap(), b"preserve");
        assert_eq!(
            fs::metadata(inputs.keys).unwrap().modified().unwrap(),
            key_time
        );
    }
}
