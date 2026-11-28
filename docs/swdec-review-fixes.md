# Report correzioni SWDEC

- **Worktree:** `/tmp/opencode/nj-swdec`
- **Branch:** `dts-swdec-spike`
- **Base revisionata:** `5c9589ac0179358b6cbf8a4eb0254309277a5105`
- **Perimetro:** solo sorgenti, test leggeri e documentazione; nessuna modifica live.

## Esito

Tutti i finding richiesti **F1–F10** hanno una correzione e una regressione. È corretto anche il
manifesto `all` segnalato dalla CI pre-fix SWDEC run `46155696814`, che aveva compilato
`nativejelly` senza produrre la DSO. Il gate host di meccanismo e il gate dell'artefatto ARM sono
cablati nei workflow, ma non è stato eseguito un build o
un test C++ in questo lane. La validazione locale è quindi statica/fittizia, non una prova del DSO
realmente prodotto o del comportamento TV.

## Fix

| ID | Correzione | File principali |
|---|---|---|
| F1 | `platformfix` registrato nel layer `base`. | `ci/module-layers.ini` |
| F2 | DSO esclusa da `DEPLOY_FILES`, copiata su `libplatformfix.so.new`, `chmod 644` + `mv` atomico e inclusa in `VERIFY_FILES`. | `Makefile`, `ci/test_deploy_manifest.py` |
| F3 | Il predicato payload richiede esplicitamente `dts_native`, dichiarazione DTS-family e gate boot-scoped. DTS ordinario senza `acodec` non riceve il key. | `rust-modules/src/player/engine.rs` |
| F4 | Decisione `nativejelly-swdec` latchata con `OnceLock`; il figlio eredita la decisione tramite sentinella esatta. Cambi runtime ignorati fino al restart. | `rust-modules/base/src/platformfix.rs`, documentazione |
| F5 | `interposer/` aggiunto al source bundle; il validatore rifiuta un Makefile che referenzia `PLATFORMFIX_SRC` se il sorgente manca. | `ci/source_bundle.py`, `ci/test_source_bundle.py` |
| F6 | Push del workflow limitato a `dts-swdec-spike`, checkout e nome artefatto fissati a `github.sha`; rimosso il vecchio branch hardcoded. | `.github/workflows/dts-native-build.yml` |
| F7 | Host mechanism test senza `|| true`, gate Python per ELF/simbolo/`DT_NEEDED`/versioni e DSO aggiunta alla matrice firmware. Gate obbligatori in CI e workflow DTS. | `interposer/hosttest/run.sh`, `ci/check-platformfix-elf.py`, workflow |
| F8 | `NJ_ARM_CXX` seleziona `g++` solo se uguale esattamente a `1`. | `ci/arm-cc.py`, `ci/test_arm_cc.py` |
| F9 | Target `platformfix` phony e condizionale: rifiuta `FLAVOR != debug` e ogni `RELEASE` non vuoto. | `Makefile`, `ci/test_platformfix.py` |
| F10 | Sentinella attiva solo per `NJ_PLATFORMFIX_ACTIVE=1` in Rust e C++; assente, vuota, `0` e altri valori inerti. | `platformfix.rs`, `platformfix.cpp`, host test |
| CI pre-fix | Intero blocco `platformfix` spostato prima di `all`: le prerequisite sono espanse immediatamente, quindi `PLATFORMFIX_FILES` deve essere già definita. Il default `FLAVOR=debug` include di nuovo `pkg/libplatformfix.so`. | `Makefile`, `ci/test_platformfix.py` |

## Test aggiunti

- Rust `nj_base::platformfix`: latch bidirezionale, sentinella esatta, ereditarietà della decisione,
  kill switch e rifiuto anche con sentinel in una build senza `devtriggers`.
- Rust `player::engine::payload_tests`: matrice DTS/DTSX/DTSE, gate spento, PCM/AC3 e caso
  “DTS + nessun `acodec` + nessun receiver”.
- Python: layer/target/workflow/contratti runtime statici, ordine delle definizioni prima di `all`
  e dipendenza DSO nel manifest debug, selettore driver ARM, gate artefatto con output `readelf`
  fittizio e controlli negativi, deploy atomico, allowlist e omissione source bundle.
- Host C++ runner: marker assente/vuoto/`0`/`1`, kill switch, simbolo chiamato da `mock_query` UND
  e default-visibility in `.dynsym` (parsing dalle colonne destre, tipo `FUNC`/`IFUNC`/`NOTYPE`)
  con relocation `JUMP_SLOT` (X86_64/AArch64) e simbolo esatto definito `GLOBAL/DEFAULT`; riga
  `readelf` stampata in caso di fallimento, output in directory temporanea.

## Verifiche eseguite

Tutte senza TV, Docker, cargo, make, compilazione o build:

```text
PYTHONDONTWRITEBYTECODE=1 python3 ci/check-module-layers.py
  -> green — 14 layer, 1125 moduli, 0 violazioni

PYTHONDONTWRITEBYTECODE=1 python3 ci/test_arm_cc.py
  -> 1 test OK

PYTHONDONTWRITEBYTECODE=1 python3 ci/test_platformfix.py
  -> 8 test OK

PYTHONDONTWRITEBYTECODE=1 python3 ci/test_platformfix_elf.py
  -> 8 test OK (readelf fittizio)

PYTHONDONTWRITEBYTECODE=1 python3 ci/test_deploy_manifest.py -k platformfix_dso
  -> 1 test OK (solo il caso statico; il resto invocherebbe make)

PYTHONDONTWRITEBYTECODE=1 python3 ci/test_source_bundle.py
  -> 39 test OK

PYTHONDONTWRITEBYTECODE=1 python3 ci/test_ci_split.py
  -> 9 test OK

PYTHONDONTWRITEBYTECODE=1 python3 ci/test_ci_timeouts.py
  -> 9 test OK

PYTHONDONTWRITEBYTECODE=1 python3 ci/test_module_graph.py
  -> 33 test OK

PYTHONDONTWRITEBYTECODE=1 python3 ci/check-module-cycle.py
  -> ok — 8 moduli sul ciclo, baseline 8

sh -n interposer/hosttest/run.sh
  -> PASS

AST parse di tutti i file Python aggiunti/modificati
  -> PASS

git diff --check
  -> PASS
```

Prima delle correzioni, le regressioni statiche F1/F2/F5/F6/F8/F9 sono state osservate fallire.

## Verifiche non eseguite e limiti

- Nessun TV: deploy atomico, loader webOS, simbolo reale, `601/ADEC`, playback e A/B restano non
  provati.
- Nessun Docker, cargo, make o build, come richiesto. Di conseguenza non sono stati eseguiti i test
  Rust, il test C++ host, il cross-build ARM, `make check`/`check-python` completi e l'intero
  `test_deploy_manifest.py` (che invoca query `make`).
- `pkg/libplatformfix.so` non è disponibile in questo worktree: il gate ELF/versioni è stato provato
  solo contro output `readelf` sintetico. Il ceiling applicato è quello documentato
  `GLIBCXX_3.4.21`; CXXABI/GCC vengono raccolti e riportati, senza inventare un massimo non
  documentato. La matrice firmware e il gate sull'artefatto reale sono demandati alla CI.
- I workflow non sono stati eseguiti. La loro sintassi/struttura è coperta dai test testuali leggeri,
  non da un run GitHub Actions.
- Un gate verde non prova il comportamento del decoder software né l'ABI della libreria TV; la
  sentinella resta un anti-loop falsificabile, non un controllo di autenticazione.
