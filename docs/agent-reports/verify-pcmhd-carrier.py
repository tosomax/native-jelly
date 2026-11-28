#!/usr/bin/env python3
"""Verify, without a television and without a rust/c toolchain, the facts the PCM-carrier
prototype in this tree rests on. Read-only. Prints PASS/FAIL per claim; exit 1 on any FAIL.

Two halves, both needed:
  * the SHIPPING/DONOR binaries still say what the design assumed
    (the report docs/agent-reports/es-carrier-webos25.md is the analysis);
  * THIS tree actually wires those facts together (the Rust decision, the feed, the C hook, the
    link export), so a later edit cannot leave, say, a 24576-byte chunk under an 8-channel Load.

Run: python3 docs/agent-reports/verify-pcmhd-carrier.py
Tree-only (when the local firmware corpus is absent):
     python3 docs/agent-reports/verify-pcmhd-carrier.py --tree-only
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LG = '/tmp/opencode/lg'

fails = []


def check(name, ok, detail=''):
    print(('PASS' if ok else 'FAIL'), name, detail)
    if not ok:
        fails.append(name)


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding='utf-8') as f:
        return f.read()


def imported(path):
    out = subprocess.check_output(['readelf', '--dyn-syms', '-W', path], text=True, errors='replace')
    return {line.split()[-1] for line in out.splitlines() if ' UND ' in line}


# ---------------------------------------------------------------- the arithmetic itself
CHUNK = 192000 * 8 * 2 * 32 // 1000
check('192k/8ch/16-bit 32 ms == 98304 B', CHUNK == 98304, f'{CHUNK}')
check('98304 == 3 x 32768-byte type IV bursts', 3 * 32768 == CHUNK)
check('the C1 chunk x HBR is the same size', 24576 * 4 == CHUNK)

# ---------------------------------------------------------------- firmware facts
def firmware_facts():
    libpf = os.path.join(LG, 'libpf.so')
    needed = ('libgstreamer-1.0.so.0', 'libgobject-2.0.so.0', 'libglib-2.0.so.0')
    d = subprocess.check_output(['readelf', '-d', '-W', libpf], text=True, errors='replace')
    for soname in needed:
        check(f'libpf NEEDED {soname}', soname in d)
    sym = imported(libpf)
    for name in (
        'gst_element_factory_make', 'gst_element_get_static_pad', 'gst_pad_add_probe',
        'gst_pad_remove_probe', 'gst_pad_send_event', 'gst_event_new_caps', 'gst_event_parse_caps',
        'gst_caps_from_string', 'gst_caps_get_structure', 'gst_structure_get_name',
        'gst_structure_get_int', 'gst_structure_get_string', 'gst_mini_object_unref',
        'gst_object_unref', 'g_object_set',
    ):
        check(f'libpf imports {name}', name in sym)


if '--tree-only' in sys.argv:
    print('SKIP firmware corpus checks (--tree-only)')
else:
    try:
        firmware_facts()
    except Exception as exc:
        check('firmware corpus available', False, str(exc))

# ---------------------------------------------------------------- this tree's wiring
carrier = read('rust-modules/src', 'player', 'es_carrier.rs')
engine = read('rust-modules/src', 'player', 'engine.rs')
feed = read('rust-modules/src', 'ff.rs')
hook = read('src', 'pcmhd_sink.c')
makefile = read('Makefile')
elfcheck = read('ci', 'check-elf.sh')

check('es_carrier declares PCMHD_CHUNK = 98304', 'pub(crate) const PCMHD_CHUNK: usize = 98304;' in carrier)
check('es_carrier gates the mode to webOS 25', 'const PCMHD_MEASURED_MAJOR: u32 = 25;' in carrier and 'major == PCMHD_MEASURED_MAJOR' in carrier)
check('es_carrier reads the dev trigger', 'nj_base::devtrig::flag("pcmhd")' in carrier)
check('es_carrier arms the C hook through atomic accessors', 'nj_pcmhd_armed_store(i32::from(on))' in carrier)
check('the mode starts OFF', 'static PCMHD: AtomicBool = AtomicBool::new(false);' in carrier)
check('the demuxer waits for accepted sink CAPS', 'fn pcmhd_wait_ready()' in carrier and 'PCMHD_SINK_READY' in carrier)
check('Pcmhd reuses the HBR burst (wide=true)', 'dtshd_burst(frame, core_len, true)' in carrier)
check('Pcmhd chunks three bursts at 32768', 'let period = BURST * HBR;' in carrier and 'i * period' in carrier)
check('Pcmhd timestamps on FRAME_NS', 'self.next_pts = Some(pts + FRAME_NS);' in carrier)
check('Pcmhd rejects unsupported timing', 'if !timing.supported()' in carrier)
check('Pcmhd flushes a partial EOF group', 'pub(crate) fn finish' in carrier)

check('engine requires dts + firmware + trigger + known 48k source',
      'let pcmhd = dts_iec' in engine
      and 'crate::player::es_carrier::pcmhd_firmware()' in engine
      and 'crate::player::es_carrier::pcmhd_requested()' in engine
      and 'crate::route::stream_asample_rate(ps) == 48_000' in engine)
check('engine rolls arming back on every failed start', 'struct PcmhdStartGuard' in engine and 'if !self.committed' in engine)
check('engine disarms at teardown', 'crate::player::es_carrier::set_pcmhd(false);' in engine)
check('the 3 MB/s audio lane gets 4x its byte cap in this mode',
      'let audio_bytes = if crate::player::es_carrier::pcmhd() { AQ_AUDIO_BYTES * 4 } else { AQ_AUDIO_BYTES };' in engine)
check('engine widens the payload only under the mode and fails closed',
      'fn with_pcmhd_info(p: &str) -> Result<String' in engine
      and 'match with_pcmhd_info(&av)' in engine)
check('the widened declaration is 8-channel 192 kHz',
      '"channelMode":"8-channel"' in engine and '"sampleRate":192' in engine)

check('demuxer has its own PCMHD packer', 'crate::player::es_carrier::Pcmhd::default()' in feed)
check('demuxer validates container and core timing', 'rate != 48_000' in feed and 'dts_core_timing(&core)' in feed)
check('demuxer waits for accepted CAPS before feed', 'pcmhd_wait_ready()' in feed)
check('demuxer feeds the chunk, not a silent E-AC3', 'pcmhd_au = Some(au);' in feed)
check('demuxer flushes partial EOF audio', 'pcmhd_packer.finish()' in feed)
check('demuxer resets the packer on seek', 'pcmhd_packer.reset();' in feed)
check('demuxer keeps the core extraction off in PCMHD', '&& !pcmhd {' in feed)

cap_keys = [
    'format=(string)S16LE', 'layout=(string)interleaved', 'rate=(int)192000',
    'channels=(int)8', 'channel-mask=(bitmask)0xc3f', 'acmod=(uint)0',
    'es=(boolean)true', 'codec-str=(string)mat', 'clock-x4=(boolean)true',
    'data-rate=(int)192000', 'data-channels=(int)8', 'data-bps=(int)16',
]
for key in cap_keys:
    check(f'hook caps carry {key}', key in hook)
check('hook never invents a DTS codec label', 'codec-str=(string)dts' not in hook)
for const, value in (('PCMHD_EVENT_PROBE_MASK', '0x140u'),
                     ('PCMHD_PROBE_OK', '0'), ('PCMHD_PROBE_DROP', '1')):
    check(f'hook constant {const} = {value}', re.search(rf'#define {const} {re.escape(value)}\b', hook) is not None)
check('hook uses public event/probe accessors', 'gst_pad_probe_info_get_event' in hook and 'gst_event_get_type' in hook)
check('hook has no donor event/probe offsets', 'PCMHD_EVENT_TYPE_OFFSET' not in hook and 'PCMHD_PROBE_INFO_DATA_OFFSET' not in hook)
check('hook validates es=false and incomplete carrier caps', 'structure_get_boolean' in hook and 'carrier_fields_match' in hook)
check('hook preserves original CAPS on send refusal', 'if (!ok)' in hook and 'return PCMHD_PROBE_OK' in hook)
check('hook balances pad refs and disarms', 's.object_ref(pad)' not in hook and 'void nj_pcmhd_disarm(void)' in hook)
check('remove_probe has the public void ABI', 'typedef void (*remove_probe_fn)' in hook)
check('hook sets and verifies pcm-multi-ch through exported GObject functions',
      'dlsym(gobj, "g_object_set")' in hook and 'dlsym(gobj, "g_object_get")' in hook
      and 'pcm_multi_ch != 1' in hook)
check('hook does not resolve non-exported GLib class macros',
      'g_type_instance_get_class' not in hook and 'g_object_class_find_property' not in hook)
check('hook is inert without the armed flag', '!nj_pcmhd_armed_load()' in hook)

check('Makefile exports the factory hook only with devtriggers',
      'PCMHD_HOOK ?=' in makefile
      and '$(if $(filter 1,$(PCMHD_HOOK)),-Wl,--export-dynamic-symbol=gst_element_factory_make)' not in makefile
      and 'ifneq ($(filter 1,$(PCMHD_HOOK)),)\n'
          'SMP_INTERPOSER_LDFLAG += -Wl,--export-dynamic-symbol=gst_element_factory_make\n'
          'endif' in makefile)
check('check-elf is feature-aware', 'PCMHD_HOOK="${PCMHD_HOOK:-0}"' in elfcheck and 'non-devtriggers build must not define/import' in elfcheck)

print()
print('FAILURES:', len(fails))
for f in fails:
    print(' -', f)
sys.exit(1 if fails else 0)
