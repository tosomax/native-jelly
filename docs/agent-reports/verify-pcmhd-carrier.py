#!/usr/bin/env python3
"""Verify, without a television and without a rust/c toolchain, the facts the PCM-carrier
prototype in this tree rests on. Read-only. Prints PASS/FAIL per claim; exit 1 on any FAIL.

Two halves, both needed:
  * the SHIPPING/DONOR binaries still say what the design assumed
    (the report docs/agent-reports/es-carrier-webos25.md is the analysis);
  * THIS tree actually wires those facts together (the Rust decision, the feed, the C hook, the
    link export), so a later edit cannot leave, say, a 24576-byte chunk under an 8-channel Load.

Run: python3 docs/agent-reports/verify-pcmhd-carrier.py
"""
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LG = '/tmp/opencode/lg'
SINK = '/home/massimo/Documents/Codex/2026-10-19/ro/work/dts-native-bridge/donor-g5/usr/lib/gstreamer-1.0/libgstlgaudiosink.so'
DST = '/tmp/opencode/research/dts_restore_plus/webos25/restore'

fails = []


def check(name, ok, detail=''):
    print(('PASS' if ok else 'FAIL'), name, detail)
    if not ok:
        fails.append(name)


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding='utf-8') as f:
        return f.read()


def binary(path):
    with open(path, 'rb') as f:
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
libpf = os.path.join(LG, 'libpf.so')
needed = set()
for soname in ('libgstreamer-1.0.so.0', 'libgobject-2.0.so.0', 'libglib-2.0.so.0'):
    needed.add(soname)
d = subprocess.check_output(['readelf', '-d', '-W', libpf], text=True, errors='replace')
for soname in needed:
    check(f'libpf NEEDED {soname}', soname in d)

sym = imported(libpf)
for name in (
    'gst_element_factory_make', 'gst_element_get_static_pad', 'gst_pad_add_probe',
    'gst_pad_remove_probe', 'gst_pad_send_event', 'gst_event_new_caps', 'gst_event_parse_caps',
    'gst_caps_from_string', 'gst_caps_get_structure', 'gst_structure_get_name',
    'gst_structure_get_int', 'gst_structure_get_string', 'gst_structure_has_field',
    'gst_mini_object_unref', 'gst_object_ref', 'gst_object_unref', 'g_object_set',
):
    check(f'libpf imports {name}', name in sym)

# Donor sink DWARF: GstEvent.type @36 and GST_EVENT_CAPS=12814 (the design's two numbers).
try:
    dwarf = subprocess.check_output(['readelf', '--debug-dump=info', SINK], text=True, errors='replace')
    ev = dwarf.find('_GstEvent')
    seg = dwarf[ev:ev + 6000]
    check('GstEvent byte_size 56', 'DW_AT_byte_size   : 56' in seg)
    check('GstEvent.type @36', re.search(r'_GstEvent.*?DW_AT_name\s+:.*?: type\s*\n.*?DW_AT_data_member_location: 36', seg, re.S) is not None)
    caps = dwarf.find('GST_EVENT_CAPS')
    check('GST_EVENT_CAPS == 12814', caps > 0 and '12814' in dwarf[caps:caps + 300])
except Exception as e:  # readelf absent or donor missing
    check('donor sink DWARF', False, str(e))

# dts_restore_plus's compiled probes: data @8, mask 0x140, DROP=1 / REMOVE=2.
try:
    so = binary(os.path.join(DST, 'out', 'libgstdtsdec.so'))
    check('event_probe loads info->data at [r1,#8]', bytes.fromhex('085091e5') in so)
    check('probe mask immediate 0x140', bytes.fromhex('051da0e3') in so)
    check('GST_PAD_PROBE_DROP == 1 in buffer_probe', bytes.fromhex('0100a0e3') in so)
    src = open(os.path.join(DST, 'src', 'lgroute.c'), encoding='utf-8').read()
    check('mask source is DOWNSTREAM|FLUSH',
          'GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH' in src)
    check('dts_restore_plus sends events straight to the sink pad',
          'gst_pad_send_event (pad, gst_event_new_stream_start' in src)
except Exception as e:
    check('dts_restore_plus probe build', False, str(e))

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
check('es_carrier arms the C hook', 'nj_pcmhd_armed.store(i32::from(on), Ordering::Release);' in carrier)
check('the mode starts OFF', 'static PCMHD: AtomicBool = AtomicBool::new(false);' in carrier)
check('Pcmhd reuses the HBR burst (wide=true)', 'dtshd_burst(frame, core_len, true)' in carrier)
check('Pcmhd chunks three bursts at 32768', 'let period = BURST * HBR;' in carrier and 'i * period' in carrier)
check('Pcmhd timestamps on FRAME_NS', 'self.next_pts = Some(pts + FRAME_NS);' in carrier)

check('engine requires dts + firmware + trigger',
      'let pcmhd = dts_iec' in engine
      and 'crate::player::es_carrier::pcmhd_firmware()' in engine
      and 'crate::player::es_carrier::pcmhd_requested()' in engine)
check('engine arms/disarms every streamed start', 'crate::player::es_carrier::set_pcmhd(pcmhd);' in engine)
check('engine disarms at teardown', 'crate::player::es_carrier::set_pcmhd(false);' in engine)
check('the 3 MB/s audio lane gets 4x its byte cap in this mode',
      'let audio_bytes = if crate::player::es_carrier::pcmhd() { AQ_AUDIO_BYTES * 4 } else { AQ_AUDIO_BYTES };' in engine)
check('engine widens the payload only under the mode',
      re.search(r'\} else if pcmhd \{\s*//[^\n]*\n(?:\s*//[^\n]*\n)*\s*with_pcmhd_info\(&av\)', engine) is not None)
check('the widened declaration is 8-channel 192 kHz',
      '"channelMode":"8-channel"' in engine and '"sampleRate":192' in engine)

check('demuxer has its own PCMHD packer', 'crate::player::es_carrier::Pcmhd::default()' in feed)
check('demuxer feeds the chunk, not a silent E-AC3', 'pcmhd_au = Some(au);' in feed)
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
for const, value in (('PCMHD_EVENT_TYPE_OFFSET', '36u'), ('PCMHD_EVENT_CAPS', '12814u'),
                     ('PCMHD_PROBE_INFO_DATA_OFFSET', '8u'), ('PCMHD_EVENT_PROBE_MASK', '0x140u'),
                     ('PCMHD_PROBE_OK', '0'), ('PCMHD_PROBE_DROP', '1')):
    check(f'hook constant {const} = {value}', re.search(rf'#define {const} {re.escape(value)}\b', hook) is not None)
check('hook sets pcm-multi-ch on the sink', '"pcm-multi-ch"' in hook)
check('hook is inert without the armed flag', '!nj_pcmhd_armed' in hook)

check('Makefile exports the factory hook',
      '--export-dynamic-symbol=gst_element_factory_make' in makefile)
check('check-elf asserts the exported hook', 'gst_element_factory_make hook is not defined exactly once' in elfcheck)

print()
print('FAILURES:', len(fails))
for f in fails:
    print(' -', f)
sys.exit(1 if fails else 0)
