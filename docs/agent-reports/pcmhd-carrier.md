# PCM carrier DTS-HD/X su webOS 25 — prototipo diagnostico (branch `dts-pcm-carrier-spike`)

Data: 28/11/2026. Worktree `/tmp/opencode/nj-pcm-carrier`, base `v011-debug`. Nessuna TV usata,
nessun push. Il codice è un **esperimento dev**, non un default di produzione.

Documento di riferimento per l'analisi statica che questo codice consuma:
`docs/agent-reports/es-carrier-webos25.md` (percorso ES, caps del sink, tabella risorse).

---

## 1. Cosa fa il prototipo

Trigger: `<runtime>/nativejelly-pcmhd` (solo build `devtriggers`; per la debug app
`/tmp/com.sostk.nativejelly.debug/nativejelly-pcmhd`). Se è presente **e** il firmware è
webOS 25 **e** la traccia è DTS **e** l'uscita audio è un receiver (la stessa `receiver_ready` che
governa il passthrough DTS):

1. **Load** — `audio: "PCM"`, `pcmInfo` S16LE / `8-channel` / interleaved / 192 kHz / 16 bit
   (`engine::with_pcmhd_info`, applicato dopo il nodo stereo 48k di `with_iec_pcm_info`).
2. **Feed** — il demuxer impacchetta ogni frame DTS-HD in un burst IEC 61937 type IV da 32768 B
   (subtype 4, big-endian) e ne consegna **tre per AU** = 98304 B, PTS contigui ogni 32 ms
   (`es_carrier::Pcmhd`). È lo stesso chunk che il percorso C1 accoda per lo swap; qui viene
   *feedato* come PCM, perché su webOS 25 non esiste la scrittura ES da intercettare.
3. **Caps** — `src/pcmhd_sink.c` esporta `gst_element_factory_make`. Quando libpf crea
   `audiosink` con il modo armato, mette una probe `EVENT_DOWNSTREAM|EVENT_FLUSH` sul pad `sink`,
   chiede `pcm-multi-ch=true` e sostituisce l'evento CAPS `audio/x-raw S16LE 192 kHz 8 ch` con:

   ```
   audio/x-raw, format=(string)S16LE, layout=(string)interleaved, rate=(int)192000,
   channels=(int)8, channel-mask=(bitmask)0xc3f, acmod=(uint)0, es=(boolean)true,
   codec-str=(string)mat, clock-x4=(boolean)true, data-rate=(int)192000,
   data-channels=(int)8, data-bps=(int)16
   ```

   È la variante che dts_restore_plus chiama **PCM carrier**: il buffer **non** porta il flag
   `1<<21` (quello è la variante native ES), quindi il sink resta su `LGAO_DATA_PCM`.
   `pcm-bypass` non viene toccato: `set_pcm` di dts_restore_plus imposta solo `pcm-multi-ch` per il
   PCM carrier, mentre `pcm-bypass` è impostata da libpf in `DirectMediaPlayer::createAudioPath`.
4. **Teardown** — `set_pcmhd(false)` accanto a `set_on(false)`: la probe resta installata ma passa
   tutto finché `nj_pcmhd_armed` è 0, e il Load successivo riparte dalla decisione.

Nessun ADEC, nessun `601`, nessun `dtsInfo`/`ac3PlusInfo`, nessun lease `earc_route`, nessun ioctl.

**Fuori scope**: TrueHD/MAT. Il percorso MAT esistente resta quello C1 (chunk da 98304 B accodati per
lo swap); le caps sarebbero le stesse, ma il feed denso dei chunk MAT in questa modalità non è
implementato né provato. Il trigger riguarda solo DTS.

---

## 2. Evidenza ABI usata (nessun numero a memoria)

| Fatto | Valore | Prova |
|---|---|---|
| `_GstEvent.type` offset | 36 | DWARF `libgstlgaudiosink.so` donor G5: `_GstEvent` byte_size 56, `type` @36 |
| `GST_EVENT_CAPS` | 12814 | stesso DWARF, enumeratore `GstEventType` |
| `GstMiniObject` size | 36 | stesso DWARF (i flag buffer, se servissero, sono a @12) |
| `GstPadProbeInfo.data` offset | 8 | `event_probe` compilato in dts_restore_plus `out/libgstdtsdec.so`: `ldr r5,[r1,#8]`, poi `ldr r0,[r5,#0x24]` |
| maschera probe | 0x140 | `gst_pad_add_probe` in `lgroute_arm`: `mov r1,#0x140` (EVENT_DOWNSTREAM\|EVENT_FLUSH) |
| `GST_PAD_PROBE_OK` / `DROP` / `REMOVE` | 0 / 1 / 2 | ritorni compilati di `event_probe`/`buffer_probe` confrontati col sorgente |
| `_GTypeInstance.g_class` @0, `_GObject.g_type_instance` @0 | GObject 12 B | DWARF dello stesso sink donor |
| soname | `libgstreamer-1.0.so.0`, `libgobject-2.0.so.0` | `readelf -d libpf.so` NEEDED |
| simboli risolti | vedi `src/pcmhd_sink.c` | tutti import dinamici di `libpf.so`/`libgstdecproxy.so` (`readelf --dyn-syms`), quindi esistono su questo firmware |

`gst_caps_from_string` è usato al posto di `gst_caps_new_simple` (varargs con `GST_TYPE_BITMASK`)
per non dipendere dal valore di un GType non disponibile nel corpus.

---

## 3. Cosa è provato e cosa no

**Provato staticamente**

- Il percorso ES webOS 25 è il sink `audiosink`, non `ioctl(0x40285207)` (report §2-3).
- Il Load PCM 192k/8ch è esprimibile: `setPCMinfo` accetta `N-channel` (`sscanf("%2d-channel")`) e
  `sampleRate` in kHz; la maschera risorse per `audio/x-raw` è 4 → chiave `"pcm"` assente → lista
  ADEC vuota (report §4).
- Il sink accetta `audio/x-raw` fino a 384 kHz / 10 canali e i campi caps extra (template @0x1fab4).
- Il chunk 98304 B = 192000 × 8 × 2 × 32 ms e = 3 × 32768 B (test host + verifier).
- `codec-str` noto = `"mat"`; `LGAO_CODEC_T` non ha DTS (report §2.2).

**Non provato (da falsificare sul device)**

- Che libpf accetti/allocchi un Load PCM 192k/8ch senza ADEC a questa larghezza (a 48k/2ch funziona,
  misurato).
- Che la negoziazione arrivi al sink come `audio/x-raw 192000 8` e non venga normalizzata da un
  `audioconvert` a 48k/2ch. La probe logga le caps realmente viste e non riscrive se non combaciano.
- **Il passo meno solido**: sostituire le caps inviando un evento CAPS *direttamente sul pad* del sink
  dentro la sua probe e scartando l'originale. `gst_pad_send_event` su quel pad è ciò che
  dts_restore_plus fa per `STREAM_START`, e i suoi commenti dicono che caps *diverse* riaprono il
  renderer LG; ma il loro carrier arriva normalmente come caps del srcpad di un elemento, cosa che
  qui non è possibile. Va confermato con `pcmhd-sink.log` + AVR.
- Che il sink di questo esemplare (33.31.68) sia il build donor verificato: il file esatto non è nel
  corpus. La probe logga il percorso e non fa hash-check (dts_restore_plus lo fa nel boot script).
- Che l'interposizione di `gst_element_factory_make` avvenga: provata dal meccanismo `ioctl` già in
  uso, ma va vista nei log (`audiosink found`).
- eARC/policy: senza `Digital Sound Output = Pass Through` la TV può mixare. Fuori scope qui.

---

## 4. Come si esegue (quando c'è una TV)

```
# build debug (FLAVOR=debug è il default tracciato)
make
make deploy
# trigger, poi play di un titolo DTS-HD MA / DTS:X con uscita su receiver
printf '' > /tmp/com.sostk.nativejelly.debug/nativejelly-pcmhd
```

Log attesi:

- `/tmp/nativejelly-events.log`
  - `load: ... a="PCM" ...`
  - `start_bufferfeed: PCM carrier armed by /tmp/nativejelly-pcmhd: ...`
  - `ff: PCM carrier: DTS-HD frames as three 32768-byte ...`
  - `ff: PCM carrier N chunks of 98304 B (3N DTS frames, E with an extension substream, ...)`
  - **nessun** `smp_cb type=18 num=601`
- `/tmp/nativejelly-pcmhd-sink.log`
  - `symbols resolved`
  - `audiosink found: probe ... pcm-multi-ch requested`
  - `caps rewritten to the carrier ...`
  - oppure `sink got audio/x-raw 48000 2 ... caps NOT rewritten` (negoziazione diversa: il test
    fallisce in modo diagnosticabile, non silenzioso)

Se compare `cannot find ADEC or PCMMC` la via PCM non passa: tornare alla patch ADEC o alla
pipeline gestita (report §6.7).

---

## 5. Rischi

1. **Rumore a pieno volume** se la caps non vengono riscritte: la TV decodifica 98304 B di carrier
   come PCM. Il trigger è esplicito e il log dice se la riscrittura è avvenuta; non armarlo su un
   impianto a volume alto.
2. **Read-ahead audio**: il carrier è 3 MB/s, quindi la coda audio (cap in BYTE, 1 MiB) terrebbe
   ~0,34 s. In questo modo dev la coda è creata a 4 MiB (`engine::start_bufferfeed_inner`, solo quando
   `es_carrier::pcmhd()` è vero) per tornare agli stessi secondi del percorso C1. `abr/sim.rs`
   continua a modellare 1 MiB: coerente perché il carrier è solo direct play, ma è una divergenza
   consapevole.
3. **Interposizione globale** di `gst_element_factory_make`: con il modo spento è un forward con una
   load atomica; con il modo acceso tocca solo il factory `audiosink`. Se una build futura creasse il
   sink con `gst_element_factory_create` (anch'esso importato da libpf), l'hook non lo vedrebbe e il
   log direbbe solo che nessun `audiosink` è stato visto.
4. **Refcount/probe**: la probe è rimossa e il pad dereferenziato quando un nuovo `audiosink` viene
   creato; tra due playback resta un ref al pad dell'ultimo sink. Il pad è posseduto dall'elemento,
   che libpf distrugge all'Unload: il ref tiene vivo il pad, non l'elemento. Accettabile per un
   prototipo, da rivedere se il modo diventasse permanente.
5. **`with_pcmhd_info` è una sostituzione testuale**: un cambio di `with_iec_pcm_info` la fa fallire
   in silenzio. Il `debug_assert_ne!` la fa esplodere nei test, non sul device.
6. **Il carrier non è "lossless" di per sé**: il trigger non ispeziona il profilo (la route non lo
   espone). I frame senza estensione sono DTS core, conteggiati nel log; il carrier funziona, ma il
   test DTS-HD/X va fatto con un file che ha l'estensione.
7. **Nessun test end-to-end senza TV**: l'unica verifica eseguibile qui è statica/host.

---

## 6. File

- `rust-modules/src/player/es_carrier.rs` — `PCMHD_CHUNK`, `Pcmhd`, `set_pcmhd`/`pcmhd`,
  `nj_pcmhd_armed`, gating webOS 25, test host (dimensione/timing/burst/rifiuti).
- `rust-modules/src/player/engine.rs` — decisione dev, `with_pcmhd_info`, disarmo al teardown, test
  del payload.
- `rust-modules/src/ff.rs` — feed dei chunk PCMHD, conteggi di log, reset al seek.
- `src/pcmhd_sink.c` — hook C (factory → probe → caps + `pcm-multi-ch`).
- `Makefile` — export di `gst_element_factory_make`.
- `ci/check-elf.sh` — asserzione che l'export sia nell'artefatto.
- `docs/agent-reports/verify-pcmhd-carrier.py` — verifica statica riproducibile.

## 7. Verifiche eseguite successivamente in Docker

Il worker non aveva toolchain Rust/C sull'host. La verifica è stata ripresa in un container
`nativejelly-test:nightly` con `rust-src`, SDL2, SDL2_ttf e GL:

```text
cargo test --lib -p nativejelly-modules player::es_carrier
test result: ok. 13 passed; 0 failed; 1 ignored; 5421 filtered out

cargo test --lib -p nativejelly-modules pcmhd
test result: ok. 4 passed; 0 failed; 0 ignored; 5431 filtered out

cargo test --lib -p nativejelly-modules dts
test result: ok. 15 passed; 0 failed; 0 ignored; 5420 filtered out

gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only src/pcmhd_sink.c
pcmhd_sink.c: syntax/warnings OK
```

La compilazione C ha trovato e corretto un simbolo mancante: ora
`gst_element_factory_make` ha una dichiarazione forward prima del controllo anti-ricorsione in
`resolve()`. Restano non eseguiti il build ARM/IPK e il test end-to-end sulla TV.
