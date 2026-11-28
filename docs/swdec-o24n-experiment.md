# SWDEC / o24n experiment (`dts-swdec-spike`)

Diagnostic variant of `native-jelly`, not a shipping feature. Goal: send
`option.useSoftwareAudioDecoder=true` in the Starfish `Load` together with the existing
`acodec=DTS` experiment, to test whether the software-audio path changes the `601 / ADEC`
refusal on the G5 (`O24N_DVB_EU`, board `o24n`).

Two pieces are required, and each is useless without the other:

1. **The flag in the Load.** The request in `option.useSoftwareAudioDecoder` only matters if the
   pipeline is allowed to emit/convert it.
2. **The platform whitelist.** `libplayerAPIs`'s compiled whitelist does not contain `o24n`, so
   `smp::util::isPlatformSupportSoftwareAudioDecoder("o24n")` answers `false` and the flag has no
   effect. `interposer/platformfix.cpp` answers `true` for `o24n` inside this app's process only.

## Trigger

```sh
# debug install (com.sostk.nativejelly.debug) only
printf 'DTS\n'  > /tmp/com.sostk.nativejelly.debug/nativejelly-acodec
: >                 /tmp/com.sostk.nativejelly.debug/nativejelly-swdec
```

* `nativejelly-swdec` arms both halves. An empty file, or any value except
  `false`/`0`/`no`/`off` (any case), means **on**; the file's presence is enough.
* `NJ_PLATFORMFIX=0` in the app's environment is the kill switch for both halves.
* The variant is compiled out of release builds (`devtriggers` is dropped by `RELEASE=1`) and is
  refused at runtime on any install whose flavour is not `debug`, so `/tmp` cannot arm it on the
  app users install.
* The `acodec` trigger must be present: the payload key is added only to a Load that actually
  declares a DTS-family audio (`DTS`, `DTSX`, `DTSE`). The AC3 fallback is unchanged.

## What the payload gets

At the single point every variant passes through, `with_software_audio_decoder` inserts, after
`appId` and before `externalStreamingInfo`:

```json
"option":{"appId":"…","useSoftwareAudioDecoder":true,"externalStreamingInfo":{…}}
```

It is idempotent (an existing key is never rewritten, and no key is duplicated), it never touches
`contents`, and it needs exactly one `externalStreamingInfo` anchor or it leaves the payload alone.
The existing `load:` line still names the declared codecs; this experiment adds one line:

```
load: swdec ON — option.useSoftwareAudioDecoder=true for a=DTS (nativejelly-swdec; interposer mapped)
```

`interposer mapped` is read from `/proc/self/maps`, i.e. it says the library is really loaded, not
that the trigger was seen. `NOT mapped` means the flag will arrive but the whitelist gate will
still refuse it.

## The interposer and the one re-exec

`libplatformfix.so` is built only for `FLAVOR=debug` and without `RELEASE=1`, staged beside the
binary, and never installed system-wide. There is no `/etc/ld.so.preload` write anywhere.

At the top of `nj_run`, `nj_base::platformfix::maybe_reexec` examines the same trigger. If armed:

1. it requires `libplatformfix.so` beside the executable (a package without it stays stock);
2. it re-execs the app once with `LD_PRELOAD=<app dir>/libplatformfix.so` and
   `NJ_PLATFORMFIX_ACTIVE=1` (the sentinel — one re-exec, never a loop);
3. the new process loads the interposer before `libplayerAPIs`, whose PLT slots are resolved
   `BIND_NOW`; a later `dlopen` could not replace them.

The interposer itself additionally refuses to override unless `NJ_PLATFORMFIX_ACTIVE` is present,
so an accidental preload into another process is inert, and honours `NJ_PLATFORMFIX=0`.

Build/packaging (on a machine with the webOS NDK and the Rust toolchain):

```sh
make FLAVOR=debug platformfix     # pkg/libplatformfix.so
make FLAVOR=debug deploy          # scp's it beside the binary (debug package/deploy only)
make FLAVOR=debug ipk             # or package it; stable/nightly never carry it
```

## Host tests

```sh
# payload splice: off, active, idempotent, valid JSON, inside option, other keys intact
cd rust-modules && CARGO_INCREMENTAL=0 cargo +nightly test --lib \
  -p nativejelly-modules player::engine::payload_tests

# trigger/reexec gate: presence, explicit false, kill switch, preload merge, sentinel, maps
cd rust-modules && CARGO_INCREMENTAL=0 cargo +nightly test --lib -p nj_base platformfix

# interposer mechanism (x86_64, needs g++): stock, no marker, marker, kill switch, PLT + symbol
interposer/hosttest/run.sh
```

Full host suite: `make check` (queues on the machine-wide check lock).

## Limits

* Not run on a television in this spike: no deploy, no playback, no `601` re-test.
* The interposer's ARM build and the TV's `libstdc++` ABI are not proven here. The source is
  compiled with `_GLIBCXX_USE_CXX11_ABI=1` (the TV's cxx11 ABI); with a mismatched ABI the mangled
  symbol differs and the override is simply never called (safe failure, no crash).
* Overriding the symbol affects all four call sites in `libplayerAPIs` (flag emission and
  ADEC→PCMMC conversion). That is intended for this experiment; it is not a general software
  decode switch.
* The key alone is not evidence of anything: without the interposer the whitelist still answers
  `false`. Read the `interposer mapped` / `NOT mapped` field on the `load: swdec` line first.
* `libplayerAPIs` is linked `BIND_NOW`; re-exec is the only supported injection point. Do not
  replace it with a `dlopen` after startup and expect the GOT to change.
