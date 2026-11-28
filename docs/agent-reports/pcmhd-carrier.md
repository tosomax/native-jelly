# PCM carrier DTS-HD/X su webOS 25 — prototipo diagnostico fail-closed

Data: 28/11/2026. Branch `dts-pcm-carrier-spike`, base `55d15b6b`.
Nessuna TV usata, nessun push, nessuna build nella revisione correttiva. Il codice resta un
**esperimento dev**, non un default di produzione e non una capacità dichiarata verificata.

Documento di riferimento per l'analisi statica: `docs/agent-reports/es-carrier-webos25.md`.
Correzioni della review indipendente: `docs/agent-reports/pcmhd-carrier-review-fixes.md`.

---

## 1. Contratto attuale

Trigger: `<runtime>/nativejelly-pcmhd` (solo build `devtriggers`; debug app:
`/tmp/com.sostk.nativejelly.debug/nativejelly-pcmhd`). Il carrier può armarsi solo se:

1. firmware webOS 25;
2. traccia DTS e uscita audio su receiver;
3. il route porta un sample rate **noto e pari a 48000 Hz**;
4. il payload PCM 192 kHz/8ch viene trasformato esattamente;
5. il demuxer legge dal core DTS normalizzato **16 blocchi × 32 campioni = 512 campioni**;
6. la probe C accetta le caps carrier prima del primo chunk audio.

Ogni condizione mancante rifiuta il percorso. In particolare, un sample rate sconosciuto non viene
assunto, un header non 48k/512 non viene ritemporizzato, e un hook mai intercettato non può far
passare rumore PCM.

### Trigger diagnostico `pcmhd-force` (build app-scoped per webOS 10)

Il test TV DTS-X di `9607c2e3` (28/11/2026, evidenza conservata nel workspace principale) è fallito **prima dell'arming**:
la app legge `release=10.3.1 major=10`, mentre il gate firmware richiedeva `major == 25`. Per
poter diagnosticare i gate successivi su quella TV, questa revisione aggiunge un secondo trigger
**, solo nelle build con `devtriggers`**:

- file `<runtime>/nativejelly-pcmhd-force` (debug app:
  `/tmp/com.sostk.nativejelly.debug/nativejelly-pcmhd-force`), letto con
  `nj_base::devtrig::flag("pcmhd-force")`;
- funzione pura `pcmhd_firmware_allowed(major, forced) = pcmhd_measured(major) || forced`: il
  valore misurato `25` resta il gate normale, `forced` aggira **solo** il gate del major;
- `engine.rs` legge il major reale e il force una sola volta e chiama
  `pcmhd_firmware_allowed(major, force)`, così decisione e log non possono divergere.

Il force **non abilita da sé alcun feed**: per armare servono ancora il trigger `pcmhd`, un
receiver DTS (`dts_iec`), i caps `dts_passthrough`, un sample rate noto pari a 48000 Hz e — soprattutto
— l'handshake CAPS accettato dalla probe C prima del primo chunk. Nessuna patch di sistema, nessuna
modifica alle impostazioni audio della TV, nessun behavior change senza i trigger: in assenza di
`pcmhd` e `pcmhd-force` il percorso è esattamente quello di prima. Il trigger resta un esperimento
dev, non un default di produzione; il fatto che il sink accetti le caps carrier resta da provare.

Il gate finale in `engine.rs` è calcolato una sola volta per start:

```
pcmhd = dts_iec && pcmhd_firmware_ok && pcmhd_requested && sample_rate == 48_000
```

### Load

`audio: "PCM"`, `pcmInfo` S16LE / `8-channel` / interleaved / 192 kHz / 16 bit.
`with_pcmhd_info()` ora restituisce `Result`: richiede esattamente un nodo stereo, nessun nodo già
wide, JSON valido e il nodo finale esatto. Un fallimento interrompe lo start.

### Feed

Il demuxer impacchetta ogni frame DTS-HD in un burst IEC 61937 type IV da 32768 B (subtype 4,
big-endian) e consegna tre burst per AU = 98304 B, PTS contigui ogni 32 ms. La durata è derivata dal
core DTS, non dal nome codec. Se `AVPacket.duration` è presente, deve anch'esso corrispondere a
512 campioni a 48 kHz (tolleranza di rounding 1 ms).

A EOF naturale, `Pcmhd::finish()` emette anche l'ultimo gruppo parziale da uno o due frame. L'AU
resta un chunk PCM fisso da 32 ms; gli slot burst inutilizzati sono zeri. Su seek il packer viene
resettato; su errore di trasporto o teardown non viene fatto il flush.

### Caps C

`src/pcmhd_sink.c` esporta `gst_element_factory_make` solo nelle build `devtriggers`. Quando libpf
crea `audiosink` con il modo armato:

1. ottiene il pad `sink` con `gst_element_get_static_pad` (una reference transfer-full);
2. installa una probe `EVENT_DOWNSTREAM|EVENT_FLUSH`;
3. imposta `pcm-multi-ch=true`;
4. valida le caps in ingresso campo per campo;
5. sostituisce solo `audio/x-raw S16LE 192000 8ch` con:

```
audio/x-raw, format=(string)S16LE, layout=(string)interleaved, rate=(int)192000,
channels=(int)8, channel-mask=(bitmask)0xc3f, acmod=(uint)0, es=(boolean)true,
codec-str=(string)mat, clock-x4=(boolean)true, data-rate=(int)192000,
data-channels=(int)8, data-bps=(int)16
```

`es` assente, `es=false`, `es` non booleano oppure `es=true` con campi carrier incompleti/sbagliati
richiedono tutti la riscrittura. Solo `es=true` con **tutti** i valori attesi bypassa la riscrittura.

Se `gst_pad_send_event()` rifiuta la caps sostitutiva, la probe restituisce `GST_PAD_PROBE_OK`:
l'evento originale non viene droppato. Rust viene disarmato e il demuxer non alimenta altro carrier.
Se l'invio riesce, la probe droppa l'originale e segnala a Rust che il downstream ha accettato le
caps carrier.

### Handshake e rollback

- `PcmhdStartGuard` azzera lo stato all'ingresso di ogni start nativo e lo disarm a su ogni uscita
  d'errore; il commit avviene solo dopo l'installazione dell'Engine.
- Il demuxer attende fino a 10 s il conferma `PCMHD_SINK_READY`. Mancata interposizione, caps mai
  negoziate, proprietà mancante o rifiuto producono un errore prima del primo chunk.
- Le callback C portano una generazione: una callback vecchia non può armare/disarmare una nuova
  sessione.
- `nj_pcmhd_disarm()` rimuove la probe e rilascia il reference del pad a ogni stop/start.

---

## 2. ABI e API pubbliche

La revisione non legge più `GstPadProbeInfo.data` o `GstEvent.type` a offset hard-coded. Usa:

- `gst_pad_probe_info_get_event()`;
- `gst_event_get_type()` + `gst_event_type_get_name()` (nessun valore enum numerico);
- `g_object_set()` + `g_object_get()` per impostare e verificare `pcm-multi-ch`; le macro
  `G_OBJECT_GET_CLASS`/`G_TYPE_INSTANCE_GET_CLASS` non sono simboli esportati e non vengono risolte;
- `gst_structure_get_boolean/int/uint/uint64/string()` per validare i campi;
- `gst_pad_remove_probe()` con il prototipo pubblico `void`.

Restano numeriche solo le costanti pubbliche `GstPadProbeType` e `GstPadProbeReturn` usate dalla
probe (0x140, OK=0, DROP=1). La semantica del sink LG e il suo parsing delle caps restano non
provati sul firmware 33.31.68; l'API pubblica elimina il rischio di leggere strutture con il layout
donor, non dimostra il comportamento del renderer.

---

## 3. Interposizione e artefatto

- `Makefile` compila `src/pcmhd_sink.c` ed esporta `gst_element_factory_make` solo se
  `devtriggers` è nel set di feature effettivo.
- `ci/check-elf.sh` riceve `PCMHD_HOOK=1` per il job dev e richiede l'export esattamente una volta;
  con `PCMHD_HOOK=0` richiede che il simbolo sia assente.
- Il gate ELF prova l'export, **non** il callsite di libpf. Se libpf usasse
  `gst_element_factory_create` o una risoluzione non interponibile, l'handshake non diventerebbe
  pronto e il carrier non verrebbe alimentato.
- `ci/pcmhd-sink-test.c` è un harness host con `dlopen/dlsym` link-wrapped: prova il forwarding
  disarmato, la separazione dei simboli, caps `es` false/incomplete, rifiuto CAPS, refcount del pad e
  disarmo. `ci/test-pcmhd-sink.sh` contiene il comando di build/run; **non è stato eseguito** in
  questa lane.

---

## 4. Log attesi

`/tmp/nativejelly-events.log`:

- `load: ... a="PCM" ...`;
- `start_bufferfeed: PCM carrier armed ... waiting for accepted sink CAPS`;
- `ff: PCM carrier ...`;
- in caso di rifiuto: `PCM carrier refused ...` e nessun feed carrier.

Con `pcmhd` o `pcmhd-force` armati, ogni start registra una riga
`start_bufferfeed: PCM carrier gates:` con: major webOS reale, major richiesto (25), force
attivo, trigger `pcmhd` attivo, `caps.dts_passthrough`, `receiver_ready`, `dts_iec` e sample rate.
Lo stesso insieme di valori essenziali è ripetuto su `PCM carrier NOT armed — ...`, che ora viene scritta
anche quando il carrier non si arma prima del receiver: è questa la riga che mancava nel test di
`9607c2e3` e che distingue un gate firmware falso da un caps/receiver falsi. Nessuna riga aggiuntiva
viene scritta per uno start ordinario senza trigger.

`/tmp/nativejelly-pcmhd-sink.log`:

- `symbols resolved through public GStreamer/GObject APIs`;
- `audiosink found: probe ...`;
- una riga `sink CAPS ...` con nome, format, layout, rate, channels, `es`, `clock-x4`, codec e
  campi data;
- `replacement carrier CAPS accepted downstream (send_event=1); original dropped`;
- oppure `REFUSED ...`.

La riga di accettazione dimostra il ritorno del downstream, non che il parser LG abbia decodificato
correttamente. La conferma finale resta AVR + log device.

---

## 5. Rischi residui

1. **Rumore/full-scale**: resta un rischio device finché caps e renderer LG non sono provati. Il
   trigger è esplicito e il feed ora è bloccato prima dell'accettazione CAPS.
2. **Sink target**: il file esatto `libgstlgaudiosink.so` 33.31.68 non è nel corpus. Nessun hash
   allowlist è stato inventato.
3. **Callsite**: l'interposizione effettiva di libpf non è dimostrata staticamente; il timeout
   fail-closed impedisce il feed ma non prova il percorso.
4. **EOF**: l'ultimo chunk parziale è padded a 32 ms; può aggiungere silenzio finale, ma non perde
   frame DTS.
5. **Read-ahead**: il carrier è 3 MB/s; la coda audio passa a 4 MiB solo in questa modalità diretta.
6. **TrueHD/MAT**: fuori scope. Il trigger riguarda solo DTS.
7. **Verifiche**: nessuna build Rust/C/ARM, nessun test host eseguito, nessun IPK, nessuna TV.

---

## 6. File principali

- `rust-modules/src/player/es_carrier.rs` — atomici C, handshake, timing supportato, packer, EOF.
- `rust-modules/src/player/engine.rs` — guard di rollback, gate sample rate, payload fail-closed.
- `rust-modules/src/player/ff.rs` — validazione container/core/packet, attesa caps, flush EOF.
- `rust-modules/src/{catalog/models.rs,jf/convert.rs,metadata.rs,route/*.rs}` — sample rate sorgente
  conservato nel route e nella snapshot della traccia.
- `src/pcmhd_sink.c` — hook C pubblico, caps validation, refcount, disarmo.
- `Makefile`, `ci/check-elf.sh`, `.github/workflows/ci.yml` — gate feature-aware.
- `ci/pcmhd-sink-test.c`, `ci/test-pcmhd-sink.sh`, `ci/test_check_elf.py` — test aggiunti.
- `docs/agent-reports/verify-pcmhd-carrier.py` — verifica statica dei nuovi contratti.

---

## 7. Verifiche nella revisione correttiva

Eseguite: solo controlli statici/leggeri documentati nel report dei fix (`git diff --check`, test
Python del gate ELF, verifica statica del carrier quando il corpus locale è disponibile).

Non eseguite: `cargo`, `make`, compilazione C, harness C, build ARM/IPK, deploy, TV, Docker, pull di
dipendenze. I vecchi risultati Docker riportati nel commit `55d15b6b` riguardano il prototipo
precedente e non provano questa revisione.

### Aggiornamento alla revisione diagnostica `dts-pcm-major10-diag`

Aggiunti `pcmhd-force`, la funzione pura `pcmhd_firmware_allowed` e i log dei gate. Il verificatore
statico `docs/agent-reports/verify-pcmhd-carrier.py` è stato aggiornato in modo minimale e **strettamente
necessario**: i valori dei gate in `engine.rs` sono ora variabili locali lette una volta sola (non più
chiamate inline), quindi il controllo sul sample rate segue `let sample_rate = ...;` +
`sample_rate == 48_000` invece della vecchia espressione inline; sono state aggiunte due asserzioni
testuali per il trigger force e per il log dei gate. Nessun gate è stato rilassato.
