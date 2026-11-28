# Correzioni review PCM carrier — report di lane

- **Worktree:** `/tmp/opencode/nj-pcm-carrier`
- **Branch:** `dts-pcm-carrier-spike`
- **Base:** `55d15b6b22ea1565582f2e45a72aa2be34ae2743`
- **Review:** `/tmp/opencode/pcm-review-agent/report.md`
- **Vincoli rispettati:** nessuna TV, nessun Docker, nessun `cargo`/`make`/build, nessun pull di
  dipendenze. Solo sorgenti, test statici/leggeri e documentazione.

## Esito

Il prototipo è stato reso **fail-closed** lungo tutti i percorsi individuati dalla review. Il carrier
non può essere alimentato finché:

1. il route non prova una sorgente DTS a 48000 Hz;
2. il core normalizzato non prova 16 blocchi × 32 campioni = 512 campioni;
3. il payload non diventa esattamente PCM 192 kHz/8ch;
4. la probe C non accetta le caps carrier;
5. l'armamento non è ancora associato a una sessione Engine installata.

Un'interposizione mancante, un caps errata, un payload divergente o un timing non supportato
producono un rifiuto, non un fallback PCM rumoroso.

## Fix applicati

### P1

- **P1-1 — interposer disarmato e risoluzione separata.** `resolve_make_locked()` è indipendente dai
  simboli della probe. I factory non `audiosink` e il modo disarmato risolvono solo il real
  `gst_element_factory_make`; un simbolo accessorio mancante non può più far restituire `NULL` a un
  factory non PCM. Il fallimento del probe non è latchato come fallimento del factory.
- **P1-2 — caps fail-closed.** `es` viene letto con `gst_structure_get_boolean`. Assente, false, non
  booleano, o true ma incompleto richiedono la riscrittura. Solo `es=true` con layout, rate, channels,
  `clock-x4`, `codec-str`, `data-*`, `acmod` e `channel-mask` esatti bypassa la riscrittura.
- **P1-3 — CAPS rifiutata.** `GST_PAD_PROBE_DROP` è restituito solo dopo `send_event != 0`. Su
  rifiuto la probe restituisce `OK`, preserva l'evento originale e chiama
  `nj_pcmhd_sink_refused()`; il demuxer si ferma prima di ulteriore feed.
- **P1-4 — rollback armamento.** `PcmhdStartGuard` azzera lo stato a ogni `start_bufferfeed_inner` e
  lo disarm a su ogni early return/panic; commit solo dopo `pa.install(eng)`. I rami `no_audio` e
  sample statici partono quindi sempre disarmati.
- **P1-5 — timing.** Il sample rate sorgente è conservato da Jellyfin/Plex fino a `CarriedAudio` e
  deve essere noto e 48000. `dts_core_timing()` legge SFCODE e NBLKS dal core normalizzato; il
  packer accetta solo 48000/16 blocchi. Se presente, `AVPacket.duration` deve corrispondere a 512
  campioni a 48 kHz. Ogni altra shape è rifiutata.
- **P1-6 — ABI pubblico.** Rimossi gli offset `info+8` e `event+36` e il valore enum numerico. Usate
  `gst_pad_probe_info_get_event`, `gst_event_get_type`, `gst_event_type_get_name` e le funzioni
  esportate `g_object_set`/`g_object_get` per verificare `pcm-multi-ch`. La macro GLib
  `g_type_instance_get_class` non viene risolta. Il comportamento del sink LG resta non provato, ma
  il codice non dipende più dal layout donor né da simboli non esportati.

### P2

- **P2-1 — discontinuità.** La soglia di smoothing passa da 100 ms a 1 ms. Gap reali da 5 ms in su
  riancorano immediatamente; il jitter non si accumula.
- **P2-2 — payload.** `with_pcmhd_info()` restituisce `Result`, richiede esattamente un nodo stereo,
  nessun nodo wide, JSON valido e il nodo finale esatto. Nessun `debug_assert` come unica difesa.
- **P2-3 — refcount pad.** Rimosso il secondo `gst_object_ref`. La reference transfer-full di
  `gst_element_get_static_pad` è rilasciata una sola volta su sostituzione, add_probe fallito o
  disarmo. `nj_pcmhd_disarm()` rimuove probe/ref a ogni start/stop.
- **P2-4 — EOF.** Aggiunto `Pcmhd::finish()` e flush nel demuxer solo su EOF naturale. Gli ultimi
  uno/due frame sono emessi in un chunk da 32 ms con slot burst inutilizzati a zero; seek/errori di
  trasporto/teardown non fanno flush.
- **P2-5 — ABI remove_probe.** Il typedef è ora `void (*)(GstPad *, unsigned long)`.

### P3

- **P3-1 — atomici.** I contatori C sono `_Atomic`; Rust espone funzioni C load/store invece di
  condividere il tipo `AtomicI32` con `volatile int`. Le callback sono limitate alla generazione
  della sessione.
- **P3-2 — diagnostica.** Log CAPS campo per campo all'ingresso, esito `send_event`, distinzione tra
  caps già complete e sostituzione accettata. I contatori once sono azzerati a install/disarmo. La
  documentazione distingue "downstream accepted" da "LG parsed correctly".
- **P3-3 — artefatto/callsite.** Makefile e check-elf sono feature-aware; il job CI dev passa
  `PCMHD_HOOK=1`, il release richiede l'assenza del simbolo. Aggiunto un harness C host. Il callsite
  effettivo di libpf resta non dimostrato: l'handshake con timeout rende quel caso non alimentabile.

## File modificati

### Sorgente Rust

- `rust-modules/src/player/es_carrier.rs`
- `rust-modules/src/player/engine.rs`
- `rust-modules/src/player/ff.rs`
- `rust-modules/src/player/playurl.rs`
- `rust-modules/src/route/plan.rs`
- `rust-modules/src/route/decision.rs`
- `rust-modules/src/catalog/models.rs`
- `rust-modules/src/jf/convert.rs`
- `rust-modules/src/metadata.rs`

### Sorgente C / build / CI

- `src/pcmhd_sink.c`
- `Makefile`
- `ci/check-elf.sh`
- `.github/workflows/ci.yml`
- `ci/test_check_elf.py`
- `ci/test-compat-auxv.c` (guard `_GNU_SOURCE` preventivo)
- `ci/pcmhd-sink-test.c` (nuovo)
- `ci/test-pcmhd-sink.sh` (nuovo)

### Test fixture Rust (solo il nuovo campo sample_rate)

- `rust-modules/src/route/carried_audio_tests.rs`
- `rust-modules/src/route/decision_direct_play_mode_tests.rs`
- `rust-modules/src/route/decision_plan_tests.rs`
- `rust-modules/src/route/decision_quality_recovery_tests.rs`
- `rust-modules/src/route/decision_resolve_route_tests.rs`
- `rust-modules/src/route/decision_retranscode_claim_tests.rs`
- `rust-modules/src/route/decision_test_support.rs`
- `rust-modules/src/appkit/track_menu.rs`

### Documentazione

- `docs/agent-reports/pcmhd-carrier.md`
- `docs/agent-reports/es-carrier-webos25.md`
- `docs/agent-reports/verify-pcmhd-carrier.py`
- `docs/agent-reports/pcmhd-carrier-review-fixes.md` (questo report)

## Test aggiunti (non eseguiti)

### Rust

- rifiuto timing 44.1/48/96/192 kHz e blocchi diversi;
- parsing SFCODE/NBLKS, DEFSIC e codice riservato;
- gap 0.5/1/1.001/5/50/100/101 ms e gap negativi;
- EOF dopo 0..5 frame, padding e idempotenza di `finish`;
- handshake CAPS per generazione, ready/refused staleness;
- rollback/commit di `PcmhdStartGuard`;
- payload mancante, duplicato, malformed o già wide;
- sample rate nel route e conversione Jellyfin.

### C host

`ci/pcmhd-sink-test.c` include il vero `src/pcmhd_sink.c` e link-wrapper `dlopen/dlsym`:

- forwarding disarmato e factory non-audio con simboli probe mancanti;
- factory audio armato con probe mancante: factory forwarded, feed rifiutato;
- refcount su install/sostituzione/add_probe fallito/disarmo;
- proprietà `pcm-multi-ch` mancante e caps inattese;
- caps `es` assente/false/non booleano/incompleto/completo;
- `send_event` accettato/rifiutato e preservazione dell'originale;
- info probe con layout volutamente diverso dal vecchio offset.

Comando registrato in `ci/test-pcmhd-sink.sh`; **non eseguito**.

### Python/statico

- `ci/test_check_elf.py`: gate feature-aware, export/assenza/import, contratti sorgente C/Rust;
- `docs/agent-reports/verify-pcmhd-carrier.py --tree-only`: contratti del tree senza corpus firmware.

## Verifiche effettuate

- La CI pre-fix PCM run `46155805610` ha rilevato `_GNU_SOURCE` ridefinito perché il Makefile passa
  già `-D_GNU_SOURCE`. Corretto con guard `#ifndef`; la scansione ha trovato lo stesso define non
  guardato in `ci/test-compat-auxv.c` e lo ha anch'esso reso idempotente. Scansione statica delle
  macro `-D` del Makefile (`_GNU_SOURCE`, `NJ_DEBUG`) contro tutti i C: **nessun conflitto residuo**.
- `python3 ci/test_check_elf.py` — **22 test OK**.
- `python3 docs/agent-reports/verify-pcmhd-carrier.py --tree-only` — **0 fail**.
- Controllo statico una-tantum dei 22 simboli risolti (21 fake + il real `make`): parità completa,
  nessun fake per macro GLib non esportate.
- `git diff --check` — **OK**.
- Verifica dei letterali `CarriedAudio`: nessuno privo del nuovo `sample_rate` o di update.

## Verifiche NON eseguite / limiti

- Nessun test Rust, nessun `cargo check/test`, nessun `rustfmt`/lint Rust.
- Nessuna compilazione C e nessun harness C eseguito.
- Nessun `make`, build ARM, link, IPK, deploy, TV o Docker.
- Nessun pull/aggiornamento dipendenze.
- Il corpus `/tmp/opencode/lg/libpf.so` non è presente in questa lane: la verifica firmware completa
  fallisce per input mancante; è stata usata la modalità `--tree-only`.
- Restano non provati: callsite effettivo di libpf, hash/build del sink 33.31.68, parsing LG delle
  caps, negoziazione 192k/8ch e riproduzione AVR. Il percorso non è dichiarato verificato o
  deployabile; in loro assenza l'handshake impedisce il feed.

## Follow-up CI host — run 46156913622 / 46157136447

Base del follow-up: `dc6dda96`.

### Due regressioni Rust

- **NBLKS:** il fixture scriveva `frame[5]=17`, che codifica 5 blocchi perché il bit alto di NBLKS
  sta nel bit 0 di `frame[4]`. La formula del parser era corretta. Resa esplicita la lettura
  `byte4 bit0 + byte5 bits7..2` e aggiunto `set_blocks()`, che codifica correttamente 1/16/18/128.
- **PTS:** il fixture chiamava "jitter" un gruppo posto ~10 ms dopo la lattice; il re-anchor a
  `72_000_500` era quindi corretto. Ricostruiti i timestamp da `FRAME_NS`: +0.5 ms viene smoothed,
  +50 ms riancora immediatamente.

Nessuna soglia fail-closed è stata allentata.

### Simbolo GLib non esportato

La check indipendente `/tmp/opencode/pcm-fix-check/report.md` ha trovato
`dlsym("g_type_instance_get_class")`, che è una macro e non un simbolo. Rimosso insieme a
`g_type_from_name`/`g_object_class_find_property`. La proprietà è ora impostata con
`g_object_set()` e verificata con `g_object_get()`; sentinel `-1` e round-trip diverso da `1`
rifiutano il carrier. L'harness non finge più quei simboli e restituisce esplicitamente `NULL`
per la macro. Aggiunto un gate statico di parità tra simboli risolti e fake.

### Harness C runtime

La run `46157136447` falliva perché l'evento originale del fixture aveva tipo `12345`, mentre il
fake restituiva il nome `"caps"` solo per `777`; la probe passava correttamente un non-CAPS.
Unificato `FAKE_CAPS_EVENT_TYPE` per originale e sostitutivo e aggiunto `expect_caps()` con stampa
di got/expected, armed/ready/refused/sends. Il contratto della probe non è stato modificato.

### Verifiche del follow-up

- Modello statico dei fixture NBLKS: round-trip 1/16/18/128.
- Modello statico del packer: output `[10, 32000010, 114000010]`.
- `nm -D` sulla GLib host: `g_object_get` e `g_object_set` esportate;
  `g_type_instance_get_class` assente (coerente con la macro GLib).
- Gate Python dei contratti C/Rust e parità simboli/fake.
- `git diff --check`.

Non eseguiti localmente: test Rust, compilazione/esecuzione harness C, cargo/make/build, ARM, TV o
Docker. La CI ha eseguito le versioni precedenti di quei test e ha prodotto i log citati; la
presente patch non è stata rieseguita in questa lane.
