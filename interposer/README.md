# interposer/ — the o24n software-audio whitelist shim

`platformfix.cpp` is a diagnostic `LD_PRELOAD` object, built only into the debug flavour
(`pkg/libplatformfix.so`) and only when `RELEASE` is empty. It makes
`smp::util::isPlatformSupportSoftwareAudioDecoder("o24n")` answer `true` inside this app's process,
so `option.useSoftwareAudioDecoder` from the Load can take effect on the G5.

It is not installed system-wide and `/etc/ld.so.preload` is never touched. The app re-execs itself
once with `LD_PRELOAD` when the `nativejelly-swdec` trigger is armed; see
[`docs/swdec-o24n-experiment.md`](../docs/swdec-o24n-experiment.md) for the trigger, the payload
change, the host tests and the limits.

Build through the repository Makefile (it uses the webOS NDK and produces the link receipt the
packaging gates require):

```sh
make FLAVOR=debug platformfix     # -> pkg/libplatformfix.so
```

Host mechanism test (x86_64, needs `g++`; it does not prove the ARM build):

```sh
interposer/hosttest/run.sh
```
