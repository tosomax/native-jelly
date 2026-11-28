# interposer/ — the o24n software-audio whitelist shim

`platformfix.cpp` is a diagnostic `LD_PRELOAD` object, built only into the debug flavour
(`pkg/libplatformfix.so`) and only when `RELEASE` is empty. It makes
`smp::util::isPlatformSupportSoftwareAudioDecoder("o24n")` answer `true` inside this app's process,
so `option.useSoftwareAudioDecoder` from the Load can take effect on the G5.

It is not installed system-wide and `/etc/ld.so.preload` is never touched. The app re-execs itself
once with `LD_PRELOAD` when the `nativejelly-swdec` trigger is armed at startup. The override requires
the exact sentinel `NJ_PLATFORMFIX_ACTIVE=1`; absent, empty, `0` and all other values stay inert in
both Rust and C++. Trigger edits require an app restart, and `NJ_PLATFORMFIX=0` is the kill switch.
See [`docs/swdec-o24n-experiment.md`](../docs/swdec-o24n-experiment.md) for the trigger, payload,
gates and limits.

Build through the repository Makefile (it uses the webOS NDK and produces the link receipt the
packaging gates require):

```sh
make FLAVOR=debug platformfix     # -> pkg/libplatformfix.so; stable/nightly/RELEASE are refused
```

The Makefile sets `NJ_ARM_CXX=1` for this one C++ link. The wrapper accepts only that exact value;
`0`, `false`, an empty string or unset all keep the ordinary C driver.

Host mechanism test (x86_64, needs `g++`; it does not prove the ARM build):

```sh
interposer/hosttest/run.sh
```
