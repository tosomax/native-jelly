//! **EXPERIMENT (`swdec`, o24n): the software-audio-decoder gate, app-scoped.**
//!
//! The G5 exposes board `o24n`, which LG's compiled whitelist in `libplayerAPIs` does not list:
//! `smp::util::isPlatformSupportSoftwareAudioDecoder("o24n")` answers `false`, so the pipeline
//! never emits `option.useSoftwareAudioDecoder` and never converts the ADEC to PCMMC, whatever
//! the Load payload says. The DTS Load on this set then still asks the Resource Manager for two
//! ADECs and is refused with `601 Resource Allocation Error` (the evidence and the 4 call sites
//! are in `interposer/platformfix.cpp`'s header and
//! `docs/swdec-o24n-experiment.md`).
//!
//! This module is the experiment's one switch and its one boot-time action:
//!
//! * [`enabled`] — is the variant armed? The debug INSTALL only (`paths::flavour()` is `debug`),
//!   a present `nativejelly-swdec` trigger (empty or any value except an explicit false), and
//!   `NJ_PLATFORMFIX` not set to `"0"`. The decision is latched at boot: changing the trigger after
//!   launch cannot arm only the payload or leave only the interposer active; an A/B flip needs an app
//!   restart. The re-exec'd child inherits the exact sentinel `NJ_PLATFORMFIX_ACTIVE=1`, which keeps
//!   its payload decision tied to the parent's even if the file changes during exec. A release build
//!   reads no trigger at all ([`devtrig`] is compile-time disabled), and a stable install cannot arm
//!   it even if somebody drops the file into `/tmp` — which is what keeps this out of production by
//!   construction rather than by a flag somebody has to remember.
//! * [`maybe_reexec`] — the interposer must be present at `exec` time: `libplayerAPIs` is linked
//!   with `BIND_NOW`, so its PLT slots are resolved when the library loads and a later `dlopen`
//!   cannot replace them. If the variant is armed and `libplatformfix.so` sits beside the binary,
//!   re-exec the app once with `LD_PRELOAD` pointing at it, plus [`SENTINEL`] so this cannot
//!   recur. A kill switch (`NJ_PLATFORMFIX=0`), a missing library, or an exec failure all leave
//!   the app on the stock path.
//!
//! Nothing here writes `/etc/ld.so.preload` and nothing installs a system-wide library: the
//! interposer lives in this app's own install directory and is preloaded into this process only.

use std::path::PathBuf;
use std::sync::OnceLock;

/// The dev trigger's bare name (`devtrig::read` adds the `nativejelly-` prefix and the runtime
/// root, so the file is `<rundir>/nativejelly-swdec` — `/tmp/com.sostk.nativejelly.debug/…` for
/// the debug install).
pub const TRIGGER: &str = "swdec";
/// `NJ_PLATFORMFIX=0` disables the whole experiment, in this module and inside the interposer.
pub const KILL_SWITCH: &str = "NJ_PLATFORMFIX";
/// The one exact active value in the re-exec'd process, shared with the C++ interposer.
pub const SENTINEL: &str = "NJ_PLATFORMFIX_ACTIVE";
pub const SENTINEL_VALUE: &str = "1";
/// The dynamic loader's preload list.
pub const PRELOAD: &str = "LD_PRELOAD";
/// The interposer's file name, beside the app binary (`paths::in_app_dir`).
pub const LIBRARY: &str = "libplatformfix.so";

/// Does the trigger's content arm the experiment?
///
/// Absent is off. Present is on — an empty file included, the way every `devtrig` presence flag
/// works. `false` / `0` / `no` / `off` (any case) is an explicit off, so an A/B run can flip the
/// file without deleting it. Any other value is a presence and therefore on.
pub fn trigger_arms(raw: Option<&str>) -> bool {
    let Some(raw) = raw else { return false };
    !matches!(
        raw.trim().to_ascii_lowercase().as_str(),
        "false" | "0" | "no" | "off"
    )
}

/// `NJ_PLATFORMFIX=0` — exactly the spelling the interposer honours. Any other value (or none)
/// leaves the experiment alone.
pub fn kill_switched(raw: Option<&str>) -> bool {
    raw == Some("0")
}

/// Does the environment carry the one active sentinel value? Empty, `0` and arbitrary strings do
/// not count; Rust must agree with the C++ override or the payload could claim an inert preload.
pub fn sentinel_active(raw: Option<&str>) -> bool {
    raw == Some(SENTINEL_VALUE)
}

/// The pure boot decision. A child carrying the exact sentinel keeps the parent's armed decision
/// even if the trigger changed during exec; the kill switch still wins in both languages.
fn boot_armed(
    debug: bool,
    triggers_enabled: bool,
    trigger: Option<&str>,
    killed: bool,
    sentinel: Option<&str>,
) -> bool {
    debug && triggers_enabled && !killed && (trigger_arms(trigger) || sentinel_active(sentinel))
}

/// Latch one sampled decision. The sampler is called exactly once, which makes the runtime-change
/// contract testable without mutating process environment or the shared trigger directory.
fn enabled_latched(
    slot: &OnceLock<bool>,
    sample: impl FnOnce() -> (bool, bool, Option<String>, bool, Option<String>),
) -> bool {
    *slot.get_or_init(|| {
        let (debug, triggers_enabled, trigger, killed, sentinel) = sample();
        boot_armed(
            debug,
            triggers_enabled,
            trigger.as_deref(),
            killed,
            sentinel.as_deref(),
        )
    })
}

/// Is the `swdec` variant armed on THIS install?
///
/// Three separate gates, and each one is load-bearing: the debug flavour keeps it off the app
/// users install; the trigger is the developer's switch; the kill switch is the escape hatch that
/// does not need the trigger removed. The trigger read is `None` at compile time without the
/// `devtriggers` feature, so a release build cannot arm any of it. The whole decision is boot-scoped
/// because the interposer itself can only be installed by the one boot-time re-exec.
pub fn enabled() -> bool {
    static DECISION: OnceLock<bool> = OnceLock::new();
    enabled_latched(&DECISION, || {
        let debug = crate::paths::flavour() == Some("debug");
        let trigger = debug.then(|| crate::devtrig::read(TRIGGER)).flatten();
        let killed = kill_switched(std::env::var(KILL_SWITCH).ok().as_deref());
        let sentinel = std::env::var(SENTINEL).ok();
        (debug, crate::devtrig::ENABLED, trigger, killed, sentinel)
    })
}

/// The interposer's absolute path in this install.
pub fn interposer_path() -> PathBuf {
    crate::paths::in_app_dir(LIBRARY)
}

/// Is `LIBRARY` mapped into this process? `/proc/self/maps` is the answer rather than an
/// environment marker: it says the loader really loaded the file, not that somebody asked it to.
pub fn maps_mention(maps: &str) -> bool {
    maps.lines()
        .any(|line| line.split_whitespace().any(|word| word.ends_with(LIBRARY)))
}

/// [`maps_mention`] over the live process. Empty on a host without `/proc`, which reads as "not
/// mapped" and is the honest answer there.
pub fn interposer_mapped() -> bool {
    std::fs::read_to_string("/proc/self/maps")
        .map(|maps| maps_mention(&maps))
        .unwrap_or(false)
}

/// The `LD_PRELOAD` value for the re-exec: ours first, the caller's existing list preserved, and
/// no duplicate if ours is already there. Pure, so the merge is host-testable.
pub fn merge_preload(existing: Option<&str>, ours: &str) -> String {
    let mut parts: Vec<&str> = existing.unwrap_or("").split_whitespace().collect();
    if !parts.iter().any(|part| *part == ours) {
        parts.insert(0, ours);
    }
    parts.join(" ")
}

/// The re-exec decision as a pure function: `Some(ld_preload)` exactly when the variant is armed,
/// this process has not already re-exec'd, the kill switch is off, and the library is installed.
pub fn reexec_preload(
    armed: bool,
    sentinel: bool,
    killed: bool,
    library: Option<&str>,
    existing_preload: Option<&str>,
) -> Option<String> {
    if !armed || sentinel || killed {
        return None;
    }
    let library = library?;
    Some(merge_preload(existing_preload, library))
}

/// Re-exec this app once with the interposer preloaded, when the variant is armed.
///
/// Called at the very top of `port::nj_run`, i.e. after `main.c` has opened the log but before
/// SDL, ACB or any worker exists. The log is truncated again by the child, so the useful line is
/// the one the child writes (`platformfix: active …`), not an announcement the exec would erase.
pub fn maybe_reexec() {
    #[cfg(all(feature = "devtriggers", unix))]
    maybe_reexec_armed();
}

#[cfg(all(feature = "devtriggers", unix))]
fn maybe_reexec_armed() {
    use std::os::unix::process::CommandExt;

    if !enabled() {
        return;
    }
    if sentinel_active(std::env::var(SENTINEL).ok().as_deref()) {
        crate::eventlog::log(&format!(
            "platformfix: swdec active (sentinel set); interposer {}",
            if interposer_mapped() { "mapped" } else { "NOT mapped" }
        ));
        return;
    }
    let lib = interposer_path();
    let lib_text = lib.to_string_lossy();
    let library = lib.is_file().then_some(lib_text.as_ref());
    let existing = std::env::var(PRELOAD).ok();
    let Some(preload) = reexec_preload(true, false, false, library, existing.as_deref()) else {
        crate::eventlog::log(&format!(
            "platformfix: swdec armed but {} is not installed here; staying stock",
            lib.display()
        ));
        return;
    };
    let exe = match std::env::current_exe() {
        Ok(exe) => exe,
        Err(error) => {
            crate::eventlog::log(&format!("platformfix: cannot resolve own executable: {error}"));
            return;
        }
    };
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let error = std::process::Command::new(&exe)
        .args(&args)
        .env(PRELOAD, &preload)
        .env(SENTINEL, SENTINEL_VALUE)
        .exec();
    // `exec` only returns on failure; the process that logs this is still the stock one.
    crate::eventlog::log(&format!("platformfix: re-exec with LD_PRELOAD failed: {error}"));
}

#[cfg(test)]
mod tests {
    use super::{
        boot_armed, enabled_latched, kill_switched, maps_mention, merge_preload, reexec_preload,
        sentinel_active, trigger_arms,
    };
    use std::sync::OnceLock;

    #[test]
    fn absent_trigger_is_off_and_presence_is_on() {
        assert!(!trigger_arms(None));
        assert!(trigger_arms(Some("")));
        assert!(trigger_arms(Some("true")));
        assert!(trigger_arms(Some("1")));
        assert!(trigger_arms(Some(" DTS ")));
    }

    #[test]
    fn explicit_false_values_are_off() {
        for value in ["false", "FALSE", "0", "no", "off", " Off "] {
            assert!(!trigger_arms(Some(value)), "{value:?} must read as off");
        }
    }

    #[test]
    fn kill_switch_is_the_exact_zero() {
        assert!(kill_switched(Some("0")));
        assert!(!kill_switched(Some("")));
        assert!(!kill_switched(Some("1")));
        assert!(!kill_switched(None));
    }

    #[test]
    fn sentinel_is_active_only_for_the_exact_value_one() {
        assert!(!sentinel_active(None));
        assert!(!sentinel_active(Some("")));
        assert!(!sentinel_active(Some("0")));
        assert!(!sentinel_active(Some("true")));
        assert!(sentinel_active(Some("1")));
    }

    #[test]
    fn boot_decision_is_latched_across_trigger_changes_in_both_directions() {
        let armed = OnceLock::new();
        let mut trigger = Some(String::new());
        assert!(enabled_latched(&armed, || (true, true, trigger.clone(), false, None)));
        trigger = Some("false".into());
        assert!(
            enabled_latched(&armed, || (true, true, trigger.clone(), false, None)),
            "turning the trigger off after boot must not leave a half-armed process"
        );
        trigger = None;
        assert!(enabled_latched(&armed, || (true, true, trigger.clone(), false, None)));

        let unarmed = OnceLock::new();
        let mut trigger = None;
        assert!(!enabled_latched(&unarmed, || (true, true, trigger.clone(), false, None)));
        trigger = Some(String::new());
        assert!(
            !enabled_latched(&unarmed, || (true, true, trigger.clone(), false, None)),
            "creating the trigger after boot cannot add a preload to this process"
        );
    }

    #[test]
    fn exact_inherited_sentinel_preserves_the_parent_decision_but_not_the_kill_switch() {
        for sentinel in [None, Some(""), Some("0"), Some("true"), Some("1")] {
            assert_eq!(
                boot_armed(true, true, Some("false"), false, sentinel),
                sentinel_active(sentinel),
                "sentinel={sentinel:?}"
            );
            assert!(
                !boot_armed(true, true, Some("false"), true, sentinel),
                "sentinel={sentinel:?}"
            );
            assert!(
                !boot_armed(false, true, Some(""), false, sentinel),
                "sentinel={sentinel:?}"
            );
            assert!(
                !boot_armed(true, false, Some(""), false, sentinel),
                "a release build must ignore even an inherited sentinel"
            );
        }
    }

    #[test]
    fn preload_merge_keeps_other_listeners_and_is_idempotent() {
        let ours = "/apps/nj/libplatformfix.so";
        assert_eq!(merge_preload(None, ours), ours);
        assert_eq!(
            merge_preload(Some("/usr/lib/libfoo.so"), ours),
            format!("{ours} /usr/lib/libfoo.so")
        );
        assert_eq!(
            merge_preload(Some(ours), ours),
            ours,
            "a second pass must not duplicate the library"
        );
    }

    #[test]
    fn reexec_needs_armed_unset_sentinel_live_switch_and_an_installed_library() {
        let lib = "/apps/nj/libplatformfix.so";
        assert_eq!(reexec_preload(false, false, false, Some(lib), None), None);
        assert_eq!(reexec_preload(true, true, false, Some(lib), None), None);
        assert_eq!(reexec_preload(true, false, true, Some(lib), None), None);
        assert_eq!(reexec_preload(true, false, false, None, None), None);
        assert_eq!(
            reexec_preload(true, false, false, Some(lib), None).as_deref(),
            Some(lib)
        );
    }

    #[test]
    fn maps_match_the_library_by_path_suffix() {
        let maps = "\
7f000000-7f001000 r-xp 00000000 00:01 42 /usr/lib/libc.so.6
7f001000-7f002000 r-xp 00000000 00:01 43 /apps/nj/libplatformfix.so
";
        assert!(maps_mention(maps));
        assert!(!maps_mention("/usr/lib/libc.so.6\n"));
        assert!(
            !maps_mention("/apps/nj/libplatformfix.so.bak\n"),
            "a sibling file must not read as the interposer"
        );
    }
}
