// SPDX-License-Identifier: GPL-3.0-or-later
//! The CPU features the compiled game needs. Set up compiles the lifted game code with `-mcpu=x86_64+cx16+fma` (game-build/release/fefw_build.py):
//! FMA3 (and AVX, which FMA implies in LLVM) and CMPXCHG16B. A CPU without them hits an illegal instruction at the first one the game runs, so
//! Set up says so before spending an hour on a build that cannot run, and the same test sits in fefw.exe (game-build/fefw/src/fefw_cpu.h).
//! Intel Haswell (2013) / AMD Piledriver (2012) and newer have them. FEFW_CPU_FORCE_MISSING=fma,avx,cx16 simulates missing features (tests).

#[derive(Debug, Clone, PartialEq)]
pub struct Cpu {
    pub brand: String,
    pub fma: bool,
    pub avx: bool,
    pub cx16: bool,
}

#[cfg(target_arch = "x86_64")]
#[allow(unused_unsafe)]
fn brand() -> String {
    use std::arch::x86_64::__cpuid;
    // SAFETY: cpuid is available on every x86-64 CPU (newer Rust marks __cpuid safe: the block is then redundant)
    unsafe {
        if __cpuid(0x8000_0000).eax < 0x8000_0004 {
            return String::new();
        }
        let mut bytes = Vec::with_capacity(48);
        for leaf in 0x8000_0002u32..=0x8000_0004 {
            let r = __cpuid(leaf);
            for v in [r.eax, r.ebx, r.ecx, r.edx] {
                bytes.extend_from_slice(&v.to_le_bytes());
            }
        }
        String::from_utf8_lossy(&bytes).trim_matches(|c: char| c == '\0' || c.is_whitespace()).to_string()
    }
}

#[cfg(target_arch = "x86_64")]
pub fn probe() -> Cpu {
    // is_x86_feature_detected also checks that the OS enabled the YMM state (XGETBV) for avx and fma
    let mut c = Cpu {
        brand: brand(),
        fma: std::is_x86_feature_detected!("fma"),
        avx: std::is_x86_feature_detected!("avx"),
        cx16: std::is_x86_feature_detected!("cmpxchg16b"),
    };
    force(&mut c, std::env::var("FEFW_CPU_FORCE_MISSING").ok().as_deref());
    c
}

#[cfg(not(target_arch = "x86_64"))]
pub fn probe() -> Cpu {
    Cpu { brand: String::new(), fma: true, avx: true, cx16: true }
}

fn force(c: &mut Cpu, list: Option<&str>) {
    for item in list.unwrap_or("").split(',').map(|s| s.trim().to_ascii_lowercase()) {
        match item.as_str() {
            "fma" => c.fma = false,
            "avx" => c.avx = false,
            "cx16" => c.cx16 = false,
            _ => {}
        }
    }
}

/// The plain-language problem, or None when the CPU has what the build needs.
pub fn problem(c: &Cpu) -> Option<String> {
    let mut missing = vec![];
    if !c.fma {
        missing.push("FMA3 (fused multiply-add)");
    }
    if !c.avx {
        missing.push("AVX");
    }
    if !c.cx16 {
        missing.push("CMPXCHG16B");
    }
    if missing.is_empty() {
        return None;
    }
    let who = if c.brand.is_empty() { "This PC's processor".to_string() } else { format!("This PC's processor ({})", c.brand) };
    Some(format!(
        "{who} does not support {}, which the game needs. Fortune's Weave is recompiled for processors with FMA3 and AVX: an Intel Haswell (2013) or newer, or an AMD Piledriver (2012) / Ryzen or newer. Older processors (Ivy Bridge and Xeon E5 v2, Sandy Bridge, Core 2, AMD FX before Piledriver) cannot run it, so Set up is not started.",
        missing.join(", ")
    ))
}

pub fn check() -> Option<String> {
    problem(&probe())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_missing_feature_is_named() {
        let mut c = Cpu { brand: "Intel(R) Xeon(R) CPU E5-1650 v2".into(), fma: true, avx: true, cx16: true };
        assert_eq!(problem(&c), None);
        force(&mut c, Some("fma"));
        let m = problem(&c).unwrap();
        assert!(m.contains("FMA3") && m.contains("Xeon") && m.contains("Haswell"));
        assert!(!m.contains("AVX,") && !m.contains("CMPXCHG16B"));
    }
}
