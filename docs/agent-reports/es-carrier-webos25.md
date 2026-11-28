# Percorso di scrittura ES audio su webOS 25 e carrier DTS-HD/X senza TV

Data: 28/11/2026. Analisi statica, sola lettura. Nessun file o binario modificato, nessuna TV usata.

> **Nota di implementazione (28/11/2026):** la proposta §5.3 è storica. Il prototipo corretto in
> `src/pcmhd_sink.c` usa API pubbliche GStreamer/GObject, non legge più offset delle strutture e
> richiede un handshake CAPS accettate prima del feed. Vedere `pcmhd-carrier.md` e
> `pcmhd-carrier-review-fixes.md`.

Etichette:

- **PROVA**: letto nel disassemblaggio, nel DWARF o nel sorgente citato.
- **MISURA**: riga di log di una sessione precedente, non riprodotta qui.
- **IPOTESI**: spiegazione coerente ma non falsificata.
- **NON DISPONIBILE**: binario assente dal corpus; nessuna inferenza sui suoi simboli.

---

## 0. Risposta breve

1. **Su webOS 25 la scrittura ES non passa da `ioctl(fd, 0x40285207, arg)`.** Quella richiesta non compare in `libkadaptor.so.2.0.1`, `libpf.so` né `libplayerAPIs.so.1.0.0` (ricerca del valore little-endian `07 52 28 40`: zero occorrenze). **PROVA**
2. **Il percorso ES è il sink GStreamer `audiosink` (`libgstlgaudiosink.so`)**: caps `audio/x-raw` con `es=true`, `clock-x4`, `codec-str`, `data-rate/channels/bps`, e per il buffer il flag `1<<21` in `GstMiniObject.flags` (offset 12 su ARM32). Il sink traduce il flag in `LGAO_DATA_ES` e chiama `lgao_feed()`, che passa a un modulo AIMC (`module_appsrc_push_data`). Il renderer è aperto su `/dev/lg/aren0` con gli ioctl `0x40045201` / `0x40045228`; **non è provato** che la scrittura dei dati usi quegli stessi ioctl (il codice sotto `lgao_feed` è in libaa/liblxa, non disponibile). **PROVA** (disassemblaggio + DWARF) per la catena fino a `lgao_feed`; **NON DISPONIBILE** per il fondo.
3. **Il carrier DD+ del C1 non arriva a quel punto** perché `libpf` decodifica l'E-AC3 e consegna PCM al sink: il flag resta 0 e `outType` è `LGAO_DATA_PCM`, non `LGAO_DATA_ES`. **PROVA** statica + **MISURA** `play-session2.log:610` (zero scritture ES).
4. **Il nuovo indizio PCM è corretto e sfruttabile**: `audio/x-raw` → maschera 4 → chiave `"pcm"`; la tabella del calcolatore **non ha** `"pcm"`, `lookup()` restituisce null e `concatResourceList()` non aggiunge nulla. La lista audio è **vuota, 0 ADEC**. **PROVA**
5. Ciò non è teorico: il fallback PCM 48 kHz stereo di native-jelly funziona già senza ADEC (`load: a="PCM"`, `smp_cb type=8 audio/x-raw 48.000000 2`). **MISURA** `play-session.log:198,212,720,732`.
6. **La via minima è quindi**: dichiarare `PCM` 192 kHz 8 canali (nessun ADEC, nessun 601) e far arrivare al sink i chunk carrier da 98304 byte ogni 32 ms, aggiungendo con un hook minimo i campi caps che il Load non può esprimere (`es=true`, `clock-x4`, `codec-str`, `data-*`, `acmod`, `channel-mask`) e la proprietà `pcm-multi-ch`. Per il G5 usare la variante **PCM carrier** (flag `1<<21` **non** impostato), non la variante native ES. **PROVA** statica + sorgente dts_restore_plus; **IPOTESI** per native-jelly end-to-end.
7. **Manca `liblxa`** e mancano `libaa`/`libaabase`/`libaudcommon`: il punto esatto sotto `lgao_feed` (syscall finale, condivisione DSP) **non è stato analizzato**. Non lo si inventa.

---

## 1. Corpus e provenienza

| File | Ruolo | Stato |
|---|---|---|
| `/tmp/opencode/lg/libkadaptor.so.2.0.1` | HAL/kadaptor webOS 25 | ELF32 ARM, stripped, BuildID `5eecbd07…` |
| `/tmp/opencode/lg/libpf.so` | pipeline media in-process | ELF32 ARM, stripped, BuildID `f9577495…` |
| `/tmp/opencode/lg/libplayerAPIs.so.1.0.0` | StarfishMediaAPIs / ResourceManager client | ELF32 ARM, stripped, BuildID `b2d47e04…` |
| `/tmp/opencode/lg/libmedia-resource-calculator.so.1` | calcolatore risorse | ELF32 ARM, stripped |
| `/home/massimo/Documents/Codex/2026-10-19/ro/work/dts-native-bridge/donor-g5/usr/lib/gstreamer-1.0/libgstlgaudiosink.so` | sink audio LG | ELF32 ARM, **non stripped, con DWARF**, SHA-256 `90dba3ef32704ce4096bf1e6045d2879d0c2192d507bc04d1e8e98437f420a43` |

Il sink non è in `/tmp/opencode/lg`; è una copia donor-G5 presente sull'host, **non verificata come il file esatto della TV in esame**. La sua struttura e le stringhe coincidono con quanto riportato da dts_restore_plus per i sink webOS 25 verificati; i risultati vanno trattati come **PROVA sul build donor**, non come prova sul firmware 33.31.68 della G5.

**NON DISPONIBILE**: `liblxa`, `libaa.so.2`, `libaabase.so.1`, `libaudcommon.so.1`, `libgstlxbase.so.0`. `libgstlgaudiosink.so` li dichiara come `NEEDED`; senza di essi non si può dire con certezza quale syscall/condivisione DSP avvenga sotto `lgao_feed`.

`libkadaptor` importa `write`, `ioctl`, `send`, `sendto`, `socket`, ma **non** `sendmsg`/`writev`; non contiene `/dev/lg/aren0` né stringhe audio-out. È linkato dal sink solo per `KADP_InitSystem`. **PROVA**

---

## 2. Il percorso ES su webOS 25

### 2.1 Catena delle chiamate

```
app Feed()  (libpf)
  -> DirectMediaPlayer::feed @ 0x139ccd   oppure   CustomPipeline::FeedStream @ 0x1fdd5/0xaaed5
  -> appsrc -> (decoder) -> audiosink
  -> gst_lg_audio_render_chain @ 0xbbf4
       legge GstBuffer flags @ [buf+0xc]
       test bit 21:  lsls r1,r3,#0xa   @ 0xbe92
       outType = 1 (LGAO_DATA_ES)      @ 0xbe96-0xbe98
       test bit 20:  lsls r2,r3,#0xb   @ 0xc79c  -> outType = 2 (METADATA)
       altrimenti outType = 0 (PCM)
  -> lgao_feed @ 0x18828  (call site 0xc696)
       -> module_appsrc_push_data @ 0x18948 (PLT 0x3bcc)     [percorso normale]
       oppure, se pcm_bypass [sink+0x1d0] != 0:
            aud_object_get @ 0x18ab2 + module_appsrc_push_data
  -> AIMC / libaa  (NON DISPONIBILE: qui avviene la scrittura dati)
  -> renderer aperto da lgao_open_renderer @ 0x14924 su /dev/lg/aren0
       ioctl 0x40045201 @ 0x14956   (setup renderer)
       ioctl 0x40045228 @ 0x14976   (processor id)
  -> DSP/HAL -> eARC -> AVR
```

`lgao_feed` ha firma (DWARF, file `lgao.c`): `int lgao_feed(a, buff, buffLength, int64 timestamp, LGAO_DATA_TYPE_T outType)`. **PROVA** (DWARF `lgao_feed` @ 0x1a97d; il call site passa il timestamp a 64 bit sullo stack e `outType` a `[sp+8]`).

### 2.2 Caps e flag

Il parser delle caps è in `gst_lg_audio_render_event` @ 0x8240 (14772 byte). Stringhe e punti:

| Campo caps | Stringa | Uso |
|---|---|---|
| `es` | chiave `0x1bd14`; log `0x1bd18` / `0x1bd30` | booleano; log `es = %d` @ 0x97f4/0x9816 |
| `clock-x4` | `0x1b9f8` | booleano; `gst_structure_get_boolean` @ 0x9470 |
| `codec-str` | `0x1bdec` | stringa; log `codec-str = %s -> %s` @ 0x9928 |
| `data-channels` | `0x1bd38` | int |
| `data-rate` | `0x1bd7c` | int |
| `data-bps` | `0x1bdb4` | int |
| `acmod` | `0x1b764` | int |
| `channel-mask` | `0x1b7c8` | int |

Pad template del sink @ `0x1fab4`:

```
audio/x-raw, format=(string){ S8, U8, S16LE, S16BE, U16LE, U16BE,
  S24LE, S24BE, U24LE, U24BE, S32LE, S32BE, U32LE, U32BE },
  layout=(string)interleaved, rate=(int)[1,384000], channels=(int)[1,10]
```

Quindi il sink **accetta solo `audio/x-raw`**, non `audio/x-dts`, `audio/x-eac3` o `audio/x-true-hd`; i campi `es`, `clock-x4`, `codec-str`, `data-*` sono campi extra sullo stesso media type. **PROVA**

`GstMiniObject` (DWARF @ 0x16bb, byte size 36):

| offset | campo |
|---|---|
| 0 | `type` |
| 4 | `refcount` |
| 8 | `lockstate` |
| **12** | **`flags`** |
| 16 | `copy` |
| 20 | `dispose` |
| … | … |

`GstBuffer` (DWARF @ 0x1b99, byte size 80) inizia con `mini_object` a offset 0; `pts` a 40, `dts` a 48, `duration` a 56. Il sink legge `[buf+0xc]`, cioè `mini_object.flags`, coerente. **PROVA**

`LGAO_DATA_TYPE_T` (DWARF @ 0x3ce0): `PCM=0`, `ES=1`, `METADATA=2`, `EVENT=3`, `METADATA2=4`. **PROVA**

**`LGAO_BUFFER_FLAG_ES` non esiste come simbolo o stringa nei binari analizzati.** Il flag ES è il bit 21 di `GstMiniObject.flags` (offset 12), non una costante nominata di lgaudiosink; `LGAO_DATA_ES` è invece il valore dell'enum `LGAO_DATA_TYPE_T` che il sink sceglie quando il bit 21 è impostato. Non inventare un simbolo `LGAO_BUFFER_FLAG_ES`: se esiste, è in una libreria non disponibile (libaa/liblxa).

`LGAO_CODEC_T` (DWARF @ 0x40ad): `UNKNOWN=0`, `PCM=1`, `AC3=2`, `EAC3=3`, `AAC=4`, …, `MAT=21`, …, **nessun DTS**. Il `codec-str` del carrier non può essere "dts": dts_restore_plus usa `"mat"` anche per DTS-HD/X. **PROVA**

### 2.3 `/dev/lg/aren0`

`_lgao_open_renderer` @ 0x14924 apre `/dev/lg/aren0` (stringa @ 0x200a8) con flag `0x80002` (O_RDWR|O_CLOEXEC) e chiama:

- `ioctl(fd, 0x40045201, &param)` @ 0x14956
- `ioctl(fd, 0x40045228, processor_id)` @ 0x14976, se esiste un modulo AIMC

**PROVA**. Questo è il sostituto webOS 25 del vecchio renderer ES; il numero `0x40285207` non esiste in questi binari.

### 2.4 Cosa manca sotto

`lgao_feed` spinge i dati in un modulo AIMC (`module_appsrc_push_data`); il modulo è creato da `aimc_create` / `ModuleRegistry_Init` / `Connector_Create` in `_lgao_open_renderer`. La scrittura finale (DSP, shared memory, eventuale ioctl) è in `libaa`/`liblxa`, **non disponibili**. Non si può quindi affermare quale syscall scriva davvero; si può solo dire che il percorso ES webOS 25 è quello sopra e che il vecchio `ioctl(0x40285207)` non è sulla strada.

---

## 3. Perché il carrier C1 non scrive su webOS 25

`src/es_swap.c` intercetta `ioctl(fd, 0x40285207, arg)` e scambia le parole 0/2 del blocco a 40 byte. Su webOS 6 il chiamante è `liblxa`, che impacchetta l'E-AC3 in IEC 61937 e scrive nel renderer ES. **PROVA** dal sorgente e dalla documentazione del progetto.

Su webOS 25:

- la costante non c'è in `libkadaptor`, `libpf`, `libplayerAPIs`;
- il sink `audiosink` riceve PCM decodificato, non ES;
- `gst_lg_audio_render_chain` calcola `outType` dal flag del buffer; con flag 0 usa `LGAO_DATA_PCM`;
- `lgao_feed` non passa dal vecchio ioctl.

Risultato misurato: `load: a="AC3 PLUS"`, `smp_cb type=8 audio/x-eac3`, poi `es_carrier: no ES write after the first pictures` (`play-session2.log:269,282,610`). **MISURA**

Quindi il gate `MEASURED_MAJOR == 6` non è il problema: anche armando il carrier su webOS 25, l'hook ioctl non ha nulla da intercettare.

---

## 4. Il nuovo indizio ADEC/PCM

### 4.1 Calcolatore: `audio/x-raw` → lista vuota

`mrc::ResourceCalculator::calcAdecResources(int,int,int)` @ 0x58d8 (`libmedia-resource-calculator.so.1`):

- `lsls r6,r4,#0x1d` @ 0x5900 → test bit 2 della maschera (valore 4) → ramo 0x5be6.
- Il ramo costruisce la chiave di 3 byte `"pcm"` e chiama `ResourceTable::lookup` @ 0x5c0c, poi `concatResourceList` @ 0x5c14.
- `ResourceTable::lookup` @ 0x4c04 è una ricerca nell'albero; se la chiave non c'è restituisce `0` (nessun fallback).
- `concatResourceList` @ 0x3c78: `cmp r1,#0; beq 0x3d5e` @ 0x3c84 → con input null non aggiunge nulla.
- La tabella embedded @ 0x7ebc contiene: `aac6`, `dts`, `dtse`, `mpeg-h`, `ac4`, `atmos`, `description`, `max`, `iamf`, `default`. **Nessun `"pcm"`**.

Quindi una Load con `audio/x-raw` produce una lista audio **vuota**: 0 ADEC, 0 ADEC_BANDWIDTH. **PROVA** statica.

Nota importante: la maschera del client è costruita dalla funzione a **0x89f24** (`libplayerAPIs`), che restituisce `2` per `audio/mpeg`, `4` per `audio/x-raw`, `8` per `audio/x-dts` e `0x10` per il prefisso `audio/x-dts*` (quindi `dtsx`/`dtse`). Il vecchio report diceva "bit 2/3/4"; la corrispondenza reale è: 4=`audio/x-raw`→`pcm` (vuoto), 8=`audio/x-dts`→`dts` (2 ADEC), 16=`audio/x-dtsx`/`audio/x-dtse`→`dtse` (2 ADEC). **PROVA** della struttura e dei valori di ritorno; l'associazione delle stringhe è quella già stabilita nel report precedente.

### 4.2 Client: cosa cerca dopo

`libplayerAPIs` contiene:

- `usePCMMC[%d]` @ 0xa9288
- `convert ADEC to PCMMC` @ 0xa940c
- `ADEC_BANDWIDTH` @ 0xa9424
- `remove ADEC_BANDWIDTH` @ 0xa9434
- `cannot find ADEC or PCMMC from response[%s]` @ 0xa95cc

In `smp::ResourceManager::acquireResources` @ 0x90fbc c'è un ramo che converte una risorsa `ADEC` in `PCMMC` (log @ 0x91298/0x9129a). Se la lista audio è vuota non c'è alcun `ADEC` da convertire. Il parser della risposta ha però un errore esplicito se non trova né `ADEC` né `PCMMC`. **PROVA** del testo e del ramo; **non provato** che una lista vuota porti davvero a quell'errore, né che il RM non aggiunga `PCMMC` da solo.

Controprova empirica: il fallback PCM 48 kHz stereo di native-jelly **funziona** con `audio/x-raw` e senza ADEC (`play-session.log:198,212`). Quindi il percorso raw audio del client accetta una Load PCM senza ADEC, almeno a 48 kHz/2 canali. La differenza a 192 kHz/8 canali è solo la caps, non la maschera: il calcolatore non legge rate né canali. **MISURA + PROVA**

### 4.3 Conseguenza per il carrier

Un Load `PCM` 192 kHz 8 canali:

```json
"audio": "PCM",
"pcmInfo": {
  "format": "S16LE",
  "channelMode": "8-channel",
  "layout": "interleaved",
  "sampleRate": 192,
  "bitsPerSample": 16
}
```

- `setPCMinfo` @ 0xc48e8 accetta `channelMode` come nome (`mono`/`dual`/`stereo`/`joint stereo`, LUT @ 0x18218c) **oppure** come stringa `N-channel` via `sscanf("%2d-channel")` (formato @ 0x14bb58). `"8-channel"` è quindi esprimibile. **PROVA** statica.
- La maschera resta 4; la lista ADEC resta vuota; il 601 non nasce dalla richiesta DTS.
- Il sink accetta 192 kHz e 8 canali (template rate 1..384000, channels 1..10). **PROVA**
- I chunk carrier da 98304 byte (192 kHz × 8 ch × 2 B × 32 ms) sono esattamente quelli che `es_carrier` produce in HBR (`CHUNK * HBR = 24576 * 4`). **PROVA** dal sorgente.

Questa è la variante che dts_restore_plus chiama **PCM carrier**: caps `audio/x-raw` 192 kHz 8 ch, `es=true`, `codec-str="mat"`, `clock-x4=true`, `pcm-multi-ch=true`, buffer **senza** flag ES. La sua documentazione riporta DTS-HD/TrueHD confermati su C2, G2, G3 e G5 (G5 su firmware 33.30.97). **PROVA** dal sorgente e dalla doc del progetto esterno; **non** è una prova sul nostro esemplare 33.31.68.

Per il G5 la variante native ES (flag `1<<21` + lease `earc_route`) è sconsigliata: dts_restore_plus riporta che su un G5 lasciava DTS:X muto, mentre il PCM carrier con restart del renderer funzionava. **PROVA** dal commento in `lgroute.c` e dalla doc `PASSTHROUGH.md`; **IPOTESI** per native-jelly.

### 4.4 Cosa manca oggi nel Feed di native-jelly

| Elemento | Stato attuale | Serve |
|---|---|---|
| Load audio | `a="PCM"` 48 kHz stereo (fallback) o `a="AC3 PLUS"` | `a="PCM"` 192 kHz 8 canali |
| `pcmInfo.channelMode` | `"stereo"` | `"8-channel"` |
| `pcmInfo.sampleRate` | 48 | 192 |
| Dati feed | core DTS in PCM 48k / E-AC3 silenzioso | chunk carrier 98304 B ogni 32 ms |
| Caps sink `es` | assente | `es=true` |
| Caps `clock-x4` | assente | `true` |
| Caps `codec-str` | assente | `"mat"` (unico label bitstream noto; niente DTS nell'enum) |
| Caps `data-rate/channels/bps` | assenti | `192000/8/16` |
| Caps `acmod` / `channel-mask` | assenti | `0` / `0xc3f` |
| Proprietà sink `pcm-multi-ch` | solo se libpf la imposta | `true` |
| Proprietà sink `pcm-bypass` | solo `DirectMediaPlayer` la imposta | `true` se il sink non prende il percorso `es` |
| Flag buffer `1<<21` | mai impostato | **non** per il PCM carrier G5; sì per native ES |
| Hook | `ioctl(0x40285207)` (webOS 6) | pad probe sul sink (webOS 25) |

Il `Feed()` pubblico non ha un parametro per il flag o per i campi caps: `DirectMediaPlayer::feed` @ 0x139ccd prende `(data, size, pts, unsigned int)`; `CustomPipeline::FeedStream` @ 0x1fdd5/0xaaed5 prende `(data, size, pts, ..., channel)`. **PROVA**. Non si può quindi aggiungere `es`/flag dal payload o dal feed: serve un hook nel processo.

---

## 5. Proposta di hook minima

### 5.1 Payload

Aggiungere una modalità carrier che dichiara:

```json
"audio": "PCM",
"pcmInfo": {
  "format": "S16LE",
  "channelMode": "8-channel",
  "layout": "interleaved",
  "sampleRate": 192,
  "bitsPerSample": 16
}
```

Nessun `dtsInfo`, nessun `ac3PlusInfo`, nessun ADEC. La lista risorse audio è vuota; il video resta la Load attuale. **PROVA** statica; **IPOTESI** che il client accetti 192k/8ch come accetta 48k/2ch.

### 5.2 Feed

Il demuxer, in modalità carrier PCM:

- produce i chunk già esistenti (`Packer` DTS-HD: 3 burst da 32768 B = 98304 B; `Mat` TrueHD: `Stream::SIZE = 98304`);
- li consegna come AU audio PCM con PTS ogni 32 ms (lo stesso `FRAME_NS = 32_000_000` già usato);
- in assenza di chunk, consegna 98304 byte di zeri (o droppa, da testare).

Non serve più `nj_es_swap` / `nj_es_swap_done` su webOS 25: il payload è già il carrier. Il codice Rust di packing e la coda restano; cambia il consumatore (Feed invece di swap).

### 5.3 Hook C

Un file C accanto a `src/es_swap.c` (es. `src/es_sink.c`), esportato dall'eseguibile con lo stesso meccanismo di `--export-dynamic-symbol` usato per `ioctl`:

1. Intercetta `gst_element_factory_make` (o `gst_element_factory_create`).
2. Chiama la funzione reale via `dlsym(RTLD_NEXT, ...)`.
   **Revisione:** l'implementazione corrente risolve separatamente il real `make` e i simboli della
   probe; il fast path disarmato non risolve questi ultimi.
3. Se il factory name è `"audiosink"`:
   - ottiene il pad `"sink"` con `gst_element_get_static_pad` (transfer-full, senza ref aggiuntivo);
   - installa un probe `GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM` (e `EVENT_FLUSH` per il seek);
   - nel probe, se `nj_es_armed`:
     - usa `gst_pad_probe_info_get_event`/`gst_event_get_type` (API pubbliche) e sostituisce l'evento
     CAPS con le caps carrier solo se il downstream accetta; altrimenti preserva l'originale:
       ```
       audio/x-raw, format=(string)S16LE, layout=(string)interleaved,
       rate=(int)192000, channels=(int)8, channel-mask=(bitmask)0xc3f,
       acmod=(uint)0, es=(boolean)true, codec-str=(string)"mat",
       clock-x4=(boolean)true, data-rate=(int)192000,
       data-channels=(int)8, data-bps=(int)16
       ```
     - imposta sul sink `pcm-multi-ch=true` (e, se necessario, `pcm-bypass=true`) con `g_object_set`.
   - se non armato, lascia passare.
4. (Opzionale, solo variante native ES) un probe `BUFFER` imposta `GST_BUFFER_FLAG 1<<21` sul buffer; per il G5 **non** farlo.
5. (Fallback se il feed non produce buffer da 98304 B) un probe `BUFFER` sostituisce il buffer con il chunk carrier, come faceva `nj_es_swap`, e imposta la size. In questo caso il flag è opzionale.

Punti di forza: nessun buffer swap nel caso 5.2 (il feed è già carrier), nessuna modifica al renderer, nessun ioctl, nessun ADEC, nessun 601. La caps rewrite è l'unica cosa che il Load non può esprimere.

### 5.4 Variante native ES (solo se il PCM carrier non basta)

- stesse caps, più flag `1<<21` sul buffer;
- lease `/dev/earc_route` (se disponibile nella sandbox) o `earc_route` già caricato;
- su G5 è la variante che dts_restore_plus evita per DTS:X; usarla solo come test.

---

## 6. Rischi ABI e di piattaforma

1. **Interposizione GStreamer**: l'eseguibile deve esportare `gst_element_factory_make` (o `_create`). Se libpf risolve i simboli con `BIND_NOW`/visibilità protetta, l'interposizione non avviene. Va verificato sul device con `LD_DEBUG=bindings` o con un log nel wrapper.
2. **Offset del flag**: `GstMiniObject.flags` a offset 12 è provato dal DWARF del sink donor e dall'istruzione `[buf+0xc]`. La variante PCM carrier non imposta questo flag. L'hook corretto non legge né flags né offset di evento/probe: usa API pubbliche per le caps. Un'eventuale variante native ES richiederebbe header GStreamer o un guard ABI/runtime separato.
3. **Build del sink**: il donor-G5 non è necessariamente il file della TV. dts_restore_plus ha una allowlist di sink verificati (`w25_pt_sink_verified`); la G5 compare, ma con firmware 33.30.97. La nostra TV è 33.31.68: verificare SHA-256 del `libgstlgaudiosink.so` prima di armare. Un sink non previsto può dare rumore o silenzio.
4. **Caps extra**: il template del sink accetta `audio/x-raw`; i campi `es`, `clock-x4`, `codec-str`, `data-*` sono extra. dts_restore_plus lo prova sul suo sink; non è detto che ogni build li interpreti allo stesso modo.
5. **`codec-str`**: l'enum del sink non ha DTS; `"mat"` è il valore usato da dts_restore_plus. Non inventare `"dts"`/`"dtshd"`: non sono nella tabella.
6. **192k/8ch**: il sink li accetta, ma il pipeline libpf può inserire `audioconvert`/capsfilter e negoziare 48k/2ch. Se il caps probe vede caps diverse, il carrier va ridimensionato o il probe deve forzare 192k/8ch a valle del converter. Il PCM 48k esistente non prova 192k/8ch.
7. **Risorsa**: la lista ADEC è vuota; il client ha però il testo `cannot find ADEC or PCMMC from response`. Il fallback PCM 48k dimostra che il percorso raw funziona, ma 192k/8ch non è stato provato. Se compare quell'errore, la via PCM cade e restano solo ADEC patch o pipeline gestita.
8. **eARC / policy**: per l'HD serve `Digital Sound Output = Pass Through`; senza `earc_route` la TV può mixare/re-encodare. Il modulo webOS 25 caricato sulla TV non ha il nodo `/dev/earc_route` nella sandbox dell'app (doc del progetto). dts_restore_plus usa il lease o `setQualityAttribute` via Luna; native-jelly oggi non ha questo ruolo audio. È un rischio di prodotto, non solo di codice.
9. **Renderer restart**: dopo un cambio traccia o una ripresa, LG può tenere il passo della traccia precedente; dts_restore_plus riavvia il renderer con un nuovo `STREAM_START`. native-jelly ricarica la pipeline su seek/track change, il che può bastare; da verificare.
10. **Thread**: il probe gira sul thread della pipeline; la coda carrier è già `Mutex`; non bloccare nel probe (niente lock lunghi, niente allocazioni grandi se possibile). Il `CURRENT` buffer va tenuto vivo fino alla copia del sink.

---

## 7. Test host e device futuro

### Host (senza TV)

- **Statico**: script che verifica nella tabella @ 0x7ebc l'assenza di `"pcm"` e che `lookup` non abbia fallback; già fatto in questa analisi.
- **Unit Rust**: aggiungere test per la modalità carrier PCM: 98304 B per 32 ms a 192k/8ch, PTS contigui, nessun `nj_es_swap`.
- **Hook C**: harness con GStreamer host (se presente) e un `fakesink`/`appsink`: interporre il factory, creare un elemento finto, pushare un evento CAPS e un buffer, verificare che il probe sostituisca le caps e imposti le proprietà. Non prova il sink LG, ma prova l'intercettazione e la gestione dei refcount.
- **`make check`** e `CARGO_INCREMENTAL=0 cargo check --lib --no-default-features`.

### Device (quando disponibile)

1. Installare un build debug con trigger `nativejelly-es25=pcm` (o simile).
2. Play DTS-HD MA/X; log attesi:
   - `load: v=H265 a="PCM" ...` con `pcmInfo` 192k/8ch;
   - `smp_cb type=8 ... audio/x-raw ... 192.000000 8`;
   - **nessun** `type=18 num=601`;
   - `type=17` con PCMMC o senza ADEC;
   - hook: `es_sink: audiosink found`, `caps rewritten`, `N carrier buffers`;
   - AVR: DTS-HD MA / DTS:X; pausa, seek, cambio traccia.
3. Se il sink negozia 48k/2ch: loggare le caps reali e provare il carrier 24576 B a 192k/2ch (DTS-HD core) prima di dichiarare fallita la via.
4. Se compare `cannot find ADEC or PCMMC`: la via PCM non passa; tornare alla patch ADEC o alla pipeline gestita.
5. Verificare SHA-256 di `libgstlgaudiosink.so` contro l'allowlist dts_restore_plus prima del test.

---

## 8. Cosa è provato, cosa è ipotesi, cosa manca

**PROVATO staticamente**

- Il percorso ES webOS 25 è il sink `audiosink`: caps `es=true` + flag buffer `1<<21`; `lgao_feed` → AIMC.
- `libkadaptor`/`libpf`/`libplayerAPIs` non contengono `0x40285207`.
- `audio/x-raw` → maschera 4 → chiave `"pcm"`; tabella senza `"pcm"`; `lookup` null; lista audio vuota.
- Il sink accetta `audio/x-raw` 192k/8ch e i campi caps extra.
- `LGAO_CODEC_T` non ha DTS; `codec-str` noto = `"mat"`.
- Il Feed pubblico non può impostare flag/caps; serve un hook.

**MISURATO in sessioni precedenti**

- PCM 48k/2ch funziona senza ADEC (`play-session.log:198,212`).
- AC3 PLUS su webOS 25 alloca `audio/x-eac3` ma non produce scritture ES (`play-session2.log:269,282,610`).
- DTS nativo con `dtsInfo` corretto resta 601 (ADEC=2) (`play-session3.log:322-324`).

**IPOTESI da falsificare**

- PCM 192k/8ch viene accettato dal client e allocato come PCMMC/senza ADEC.
- Il sink negozia 192k/8ch e il PCM carrier arriva all'AVR come DTS-HD/X.
- Il flag `1<<21` non serve per il G5 (PCM carrier), mentre `es=true` + `pcm-multi-ch` bastano.
- `codec-str="mat"` è accettato anche per DTS-HD/X su questo firmware.

**NON DISPONIBILE / non provato**

- `liblxa`, `libaa`, `libaabase`, `libaudcommon`: la syscall finale e il comportamento DSP sotto `lgao_feed` non sono noti.
- Il file esatto `libgstlgaudiosink.so` della TV 33.31.68 non è nel corpus; il donor-G5 è una copia.
- Nessun test end-to-end senza TV.

---

## 9. Tabella indirizzi rapida

| Simbolo / stringa | File | Indirizzo |
|---|---|---|
| `gst_lg_audio_render_chain` | libgstlgaudiosink | 0xbbf4 (Thumb) |
| lettura flags buffer | libgstlgaudiosink | 0xbe90 |
| test bit 21 (ES) | libgstlgaudiosink | 0xbe92 |
| test bit 20 (METADATA) | libgstlgaudiosink | 0xc79c |
| call `lgao_feed` | libgstlgaudiosink | 0xc696 |
| `lgao_feed` | libgstlgaudiosink | 0x18828 |
| `module_appsrc_push_data` | libgstlgaudiosink | 0x18948 (PLT 0x3bcc) |
| `lgao_open_renderer` | libgstlgaudiosink | 0x14924 |
| `ioctl 0x40045201` | libgstlgaudiosink | 0x14956 |
| `ioctl 0x40045228` | libgstlgaudiosink | 0x14976 |
| `lgao_set_pcm_bypass` | libgstlgaudiosink | 0x17868 |
| flag `pcm_bypass` store | libgstlgaudiosink | 0x1788c (`[+0x1d0]`) |
| caps `es` | libgstlgaudiosink | key 0x1bd14; log 0x1bd18 / 0x1bd30 |
| caps `clock-x4` | libgstlgaudiosink | 0x1b9f8 |
| caps `codec-str` | libgstlgaudiosink | 0x1bdec |
| caps `data-channels` | libgstlgaudiosink | 0x1bd38 |
| caps `data-rate` | libgstlgaudiosink | 0x1bd7c |
| caps `data-bps` | libgstlgaudiosink | 0x1bdb4 |
| caps `acmod` | libgstlgaudiosink | 0x1b764 |
| caps `channel-mask` | libgstlgaudiosink | 0x1b7c8 |
| pad template | libgstlgaudiosink | 0x1fab4 |
| `GstMiniObject.flags` offset | DWARF | 12 |
| `LGAO_DATA_TYPE_T` | DWARF | PCM 0 / ES 1 / METADATA 2 |
| `LGAO_CODEC_T` | DWARF | MAT 21, niente DTS |
| `DirectMediaPlayer::createAudioPath` | libpf | 0x13bf68 |
| set `pcm-bypass` | libpf | 0x13c222 |
| set `pcm-multi-ch` | libpf | 0x13c45e |
| `CustomPipeline::setAdecSinkInfo_ES` | libpf | 0xb5754 |
| `setPCMinfo` | libpf | 0xc48e8 |
| `sscanf("%2d-channel")` | libpf | 0xc4fd2 |
| `LUT_ChannelModeType` | libpf | 0x18218c |
| `calcAdecResources` | libmrc | 0x58d8 |
| test bit 2 (PCM) | libmrc | 0x5900 |
| key `"pcm"` build | libmrc | 0x5be6 |
| `ResourceTable::lookup` | libmrc | 0x4c04 |
| `concatResourceList` | libmrc | 0x3c78 |
| tabella risorse | libmrc | 0x7ebc |
| helper maschera audio (2/4/8/0x10) | libplayerAPIs | 0x89f24 |
| `ResourceManager::acquireResources` | libplayerAPIs | 0x90fbc |
| `allocateResource` | libplayerAPIs | 0x43da0 |
| `cannot find ADEC or PCMMC` | libplayerAPIs | 0xa95cc |
| `convert ADEC to PCMMC` | libplayerAPIs | 0xa940c |
| `ioctl 0x40285207` | libkadaptor/libpf/libplayerAPIs | assente |
