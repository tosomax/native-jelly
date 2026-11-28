/* The webOS 25 PCM-carrier hook for rust-modules/src/player/es_carrier.rs -- DIAGNOSTIC ONLY.
 *
 * On webOS 25 there is no ES renderer to swap (`ioctl 0x40285207` exists in none of its libraries);
 * the audio path is the GStreamer `audiosink` (`libgstlgaudiosink.so`), which takes `audio/x-raw`
 * caps and calls `lgao_feed()` as PCM unless those caps carry the bitstream fields
 * `docs/agent-reports/es-carrier-webos25.md` recovered from the sink's own DWARF and strings.
 * So the carrier's 98304-byte IEC 61937 chunks are declared and fed as PCM 192 kHz 8 ch, and this
 * file rewrites the one CAPS event the Load cannot express:
 *
 *   es=true, clock-x4=true, codec-str="mat", data-rate=192000, data-channels=8, data-bps=16,
 *   acmod=0, channel-mask=0xc3f, plus the sink property `pcm-multi-ch=true`.
 *
 * ARMING: nothing happens unless Rust set `nj_pcmhd_armed` (the `/tmp/nativejelly-pcmhd` trigger on
 * webOS 25 with a receiver). With it cleared, `gst_element_factory_make` is a plain forward.
 *
 * ABI EVIDENCE, all read from the shipped/donor binaries -- none of these numbers is recalled:
 *  - `GstMiniObject` is 36 bytes and `_GstEvent.type` is at offset 36: DWARF of the donor G5
 *    `libgstlgaudiosink.so` (GstEvent byte_size 56, member `type` at 36).
 *  - `GST_EVENT_CAPS` = 12814: same DWARF (GstEventType enumerator).
 *  - `GstPadProbeInfo.data` at offset 8 and the event read back at `[data+0x24]`: compiled
 *    `event_probe` in dts_restore_plus's `out/libgstdtsdec.so` (`ldr r5,[r1,#8]`).
 *  - probe mask 0x140 = EVENT_DOWNSTREAM|EVENT_FLUSH, and `GST_PAD_PROBE_OK`/`DROP`/`REMOVE` =
 *    0/1/2: immediates at that build's `gst_pad_add_probe` call site and in `buffer_probe`/
 *    `event_probe`, where the source says which constant each return is.
 *  - `_GTypeInstance.g_class` at 0 and `_GObject.g_type_instance` at 0 (12-byte GObject), so
 *    G_OBJECT_GET_CLASS is `*(void **)object`: DWARF of the same donor sink.
 *  - Every resolved symbol below is a dynamic import of libpf/libgstdecproxy on this firmware
 *    (`readelf --dyn-syms`), i.e. the library that defines it is present and its name is right.
 *    The function SIGNATURES are GStreamer 1.x's public ABI; the ones a probe touches are pinned
 *    by the structures above.
 *
 * THE ONE UNPROVEN STEP: replacing the caps by sending the carrier CAPS event on the sink pad from
 * inside its own downstream probe and dropping the original. Sending events straight to that pad is
 * what dts_restore_plus does for STREAM_START, and its own comments say a *different* caps event
 * reopens LG's renderer, but it normally delivers carrier caps as its element's srcpad caps, which
 * this app cannot do. Confirm on the TV by `pcmhd-sink.log`: "caps rewritten" must precede
 * "sink saw carrier caps", and the receiver must show DTS-HD/X.
 *
 * Log: /tmp/nativejelly-pcmhd-sink.log (append, opened per line: no buffering, no stdio state on
 * a streaming thread).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Rust: player::es_carrier::nj_pcmhd_armed (non-zero only for the armed dev playback). */
extern volatile int nj_pcmhd_armed;

#define PCMHD_LOG "/tmp/nativejelly-pcmhd-sink.log"
/* audio/x-raw 192 kHz 8 ch S16LE -- what the Load declares -- plus the sink's bitstream fields. */
#define PCMHD_CAPS \
  "audio/x-raw, format=(string)S16LE, layout=(string)interleaved, rate=(int)192000, " \
  "channels=(int)8, channel-mask=(bitmask)0xc3f, acmod=(uint)0, es=(boolean)true, " \
  "codec-str=(string)mat, clock-x4=(boolean)true, data-rate=(int)192000, " \
  "data-channels=(int)8, data-bps=(int)16"
/* GstPadProbeType EVENT_DOWNSTREAM|EVENT_FLUSH: the mask dts_restore_plus passes (0x140). */
#define PCMHD_EVENT_PROBE_MASK 0x140u
/* GstPadProbeReturn, from the same build's probes. */
#define PCMHD_PROBE_OK 0
#define PCMHD_PROBE_DROP 1
/* GstEventType CAPS. */
#define PCMHD_EVENT_CAPS 12814u
/* GstEvent.type, after the 36-byte GstMiniObject. */
#define PCMHD_EVENT_TYPE_OFFSET 36u
/* GstPadProbeInfo.data. */
#define PCMHD_PROBE_INFO_DATA_OFFSET 8u

typedef struct GstElement GstElement;
typedef struct GstPad GstPad;
typedef struct GstEvent GstEvent;
typedef struct GstCaps GstCaps;
typedef struct GstStructure GstStructure;

typedef GstElement *(*make_fn)(const char *, const char *);
typedef GstPad *(*get_pad_fn)(GstElement *, const char *);
typedef unsigned long (*add_probe_fn)(GstPad *, unsigned int, int (*)(GstPad *, void *, void *), void *, void (*)(void *));
typedef int (*remove_probe_fn)(GstPad *, unsigned long);
typedef int (*send_event_fn)(GstPad *, GstEvent *);
typedef GstEvent *(*event_new_caps_fn)(GstCaps *);
typedef void (*event_parse_caps_fn)(GstEvent *, GstCaps **);
typedef GstCaps *(*caps_from_string_fn)(const char *);
typedef GstStructure *(*caps_get_structure_fn)(const GstCaps *, unsigned int);
typedef const char *(*structure_get_name_fn)(const GstStructure *);
typedef int (*structure_get_int_fn)(const GstStructure *, const char *, int *);
typedef const char *(*structure_get_string_fn)(const GstStructure *, const char *);
typedef int (*structure_has_field_fn)(const GstStructure *, const char *);
typedef void (*mini_object_unref_fn)(void *);
typedef void *(*object_ref_fn)(void *);
typedef void (*object_unref_fn)(void *);
typedef void *(*object_class_find_property_fn)(void *, const char *);

struct syms {
  int tried;
  int ok;
  make_fn make;
  get_pad_fn get_static_pad;
  add_probe_fn add_probe;
  remove_probe_fn remove_probe;
  send_event_fn send_event;
  event_new_caps_fn event_new_caps;
  event_parse_caps_fn event_parse_caps;
  caps_from_string_fn caps_from_string;
  caps_get_structure_fn caps_get_structure;
  structure_get_name_fn structure_get_name;
  structure_get_int_fn structure_get_int;
  structure_get_string_fn structure_get_string;
  structure_has_field_fn structure_has_field;
  mini_object_unref_fn mini_object_unref;
  object_ref_fn object_ref;
  object_unref_fn object_unref;
  object_class_find_property_fn class_find_property;
  void (*g_object_set)(void *, const char *, ...);
};

static struct syms s;
static pthread_mutex_t pcmhd_lock = PTHREAD_MUTEX_INITIALIZER;
static GstPad *probe_pad;
static unsigned long probe_id;
/* A replacement CAPS event sent from inside our own probe must not be rewritten again. */
static __thread int in_carrier;

static void sink_log(const char *fmt, ...) {
  FILE *f = fopen(PCMHD_LOG, "a");
  struct timespec now;
  if (!f)
    return;
  clock_gettime(CLOCK_MONOTONIC, &now);
  fprintf(f, "%ld.%03ld ", (long)now.tv_sec, now.tv_nsec / 1000000L);
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fputc('\n', f);
  fclose(f);
}

/* dlsym on the already-loaded library, not RTLD_NEXT: libpf is dlopen'd and its dependencies need
 * not be in the main executable's global scope. RTLD_NOLOAD never loads anything new; the plain
 * retry covers a build where the soname was promoted before we ran. */
static void *lib(const char *soname) {
  void *h = dlopen(soname, RTLD_NOW | RTLD_NOLOAD);
  if (!h)
    h = dlopen(soname, RTLD_NOW);
  return h;
}

static int resolve(void) {
  if (s.tried)
    return s.ok;
  s.tried = 1;
  void *gst = lib("libgstreamer-1.0.so.0");
  void *gobj = lib("libgobject-2.0.so.0");
  if (!gst || !gobj) {
    if (nj_pcmhd_armed)
      sink_log("NOT ARMED: libgstreamer/libgobject not loaded (gst=%p gobj=%p)", gst, gobj);
    return 0;
  }
#define SYM(fn, name) \
  do { \
    *(void **)(&s.fn) = dlsym(gst, name); \
    if (!s.fn) { \
      if (nj_pcmhd_armed) \
        sink_log("NOT ARMED: libgstreamer has no %s", name); \
      return 0; \
    } \
  } while (0)
  SYM(make, "gst_element_factory_make");
  SYM(get_static_pad, "gst_element_get_static_pad");
  SYM(add_probe, "gst_pad_add_probe");
  SYM(remove_probe, "gst_pad_remove_probe");
  SYM(send_event, "gst_pad_send_event");
  SYM(event_new_caps, "gst_event_new_caps");
  SYM(event_parse_caps, "gst_event_parse_caps");
  SYM(caps_from_string, "gst_caps_from_string");
  SYM(caps_get_structure, "gst_caps_get_structure");
  SYM(structure_get_name, "gst_structure_get_name");
  SYM(structure_get_int, "gst_structure_get_int");
  SYM(structure_get_string, "gst_structure_get_string");
  SYM(structure_has_field, "gst_structure_has_field");
  SYM(mini_object_unref, "gst_mini_object_unref");
  SYM(object_ref, "gst_object_ref");
  SYM(object_unref, "gst_object_unref");
#undef SYM
  *(void **)(&s.class_find_property) = dlsym(gobj, "g_object_class_find_property");
  *(void **)(&s.g_object_set) = dlsym(gobj, "g_object_set");
  if (!s.class_find_property || !s.g_object_set) {
    if (nj_pcmhd_armed)
      sink_log("NOT ARMED: libgobject lacks g_object_class_find_property/g_object_set");
    return 0;
  }
  /* dlsym(handle) should be the library's own definition; if this executable ever answered
   * instead the call below would recurse, so refuse rather than crash. */
  if ((void *)s.make == (void *)&gst_element_factory_make) {
    *(void **)(&s.make) = dlsym(RTLD_NEXT, "gst_element_factory_make");
    if (!s.make || (void *)s.make == (void *)&gst_element_factory_make) {
      if (nj_pcmhd_armed)
        sink_log("NOT ARMED: gst_element_factory_make resolves to this hook");
      s.make = 0;
      return 0;
    }
  }
  s.ok = 1;
  if (nj_pcmhd_armed)
    sink_log("symbols resolved (libgstreamer=%p libgobject=%p)", gst, gobj);
  return 1;
}

/* Every condition the sink's own parser needs, checked field by field: a wrong caps string is
 * silent (LG decodes the carrier as PCM), so the values -- not just the media type -- decide. */
static int is_our_pcm(GstCaps *caps) {
  GstStructure *st = s.caps_get_structure(caps, 0);
  const char *name, *format;
  int rate = 0, channels = 0;
  if (!st)
    return 0;
  name = s.structure_get_name(st);
  format = s.structure_get_string(st, "format");
  if (!name || strcmp(name, "audio/x-raw") || !format || strcmp(format, "S16LE"))
    return 0;
  if (!s.structure_get_int(st, "rate", &rate) || rate != 192000)
    return 0;
  if (!s.structure_get_int(st, "channels", &channels) || channels != 8)
    return 0;
  /* Already a carrier's caps (a replay, or a rebuild resending ours): leave them alone. */
  return !s.structure_has_field(st, "es");
}

static int on_event(GstPad *pad, void *info, void *user) {
  GstEvent *event;
  GstCaps *caps = 0;
  (void)user;
  if (!nj_pcmhd_armed || !info || in_carrier)
    return PCMHD_PROBE_OK;
  event = *(GstEvent **)((char *)info + PCMHD_PROBE_INFO_DATA_OFFSET);
  if (!event || *(uint32_t *)((char *)event + PCMHD_EVENT_TYPE_OFFSET) != PCMHD_EVENT_CAPS)
    return PCMHD_PROBE_OK;
  s.event_parse_caps(event, &caps);
  if (!caps)
    return PCMHD_PROBE_OK;
  if (!is_our_pcm(caps)) {
    /* Say once what actually arrived: libpf may have negotiated another shape (48 kHz stereo, or
     * a converted one), and that is the difference between "no DTS-HD" and "wrong rate". */
    static int told;
    if (!told++) {
      GstStructure *st = s.caps_get_structure(caps, 0);
      int rate = -1, channels = -1;
      if (st) {
        s.structure_get_int(st, "rate", &rate);
        s.structure_get_int(st, "channels", &channels);
      }
      sink_log("sink got %s %d Hz %d ch, not the declared 192 kHz/8 ch S16LE: caps NOT rewritten",
               st ? s.structure_get_name(st) : "?", rate, channels);
    }
    return PCMHD_PROBE_OK;
  }
  /* Send our carrier CAPS straight to this pad, then drop the original. The nested send re-enters
   * this probe; `in_carrier` passes it through to the sink. */
  GstCaps *carrier = s.caps_from_string(PCMHD_CAPS);
  if (!carrier) {
    sink_log("gst_caps_from_string refused the carrier caps: caps NOT rewritten");
    return PCMHD_PROBE_OK;
  }
  GstEvent *sent = s.event_new_caps(carrier);
  s.mini_object_unref(carrier);
  if (!sent) {
    sink_log("gst_event_new_caps failed: caps NOT rewritten");
    return PCMHD_PROBE_OK;
  }
  in_carrier = 1;
  /* gst_pad_send_event takes ownership of the event (one ref), as dts_restore_plus relies on. */
  int ok = s.send_event(pad, sent);
  in_carrier = 0;
  static int told;
  if (!told++)
    sink_log("caps rewritten to the carrier (es=true codec-str=mat 192 kHz 8 ch), send_event=%d", ok);
  return PCMHD_PROBE_DROP;
}

static void install(GstElement *sink) {
  GstPad *pad;
  pad = s.get_static_pad(sink, "sink");
  if (!pad) {
    sink_log("audiosink has no \"sink\" pad: caps NOT rewritten");
    return;
  }
  pthread_mutex_lock(&pcmhd_lock);
  if (probe_pad) {
    s.remove_probe(probe_pad, probe_id);
    s.object_unref(probe_pad);
    probe_pad = 0;
  }
  s.object_ref(pad);
  probe_pad = pad;
  probe_id = s.add_probe(pad, PCMHD_EVENT_PROBE_MASK, on_event, 0, 0);
  if (!probe_id) {
    s.object_unref(probe_pad);
    probe_pad = 0;
    pthread_mutex_unlock(&pcmhd_lock);
    sink_log("gst_pad_add_probe failed: caps NOT rewritten");
    return;
  }
  pthread_mutex_unlock(&pcmhd_lock);
  /* The carrier needs lgao_push_first_event to run the PCM-multi-channel setup. libpf sets this
   * itself on DirectMediaPlayer, not on the buffer-feed pipeline we use. */
  if (s.class_find_property(*(void **)sink, "pcm-multi-ch"))
    s.g_object_set(sink, "pcm-multi-ch", 1, 0);
  else
    sink_log("audiosink has no pcm-multi-ch property");
  sink_log("audiosink found: probe %lu on the sink pad, pcm-multi-ch requested", probe_id);
}

GstElement *gst_element_factory_make(const char *factory_name, const char *name) {
  GstElement *element;
  int ready;
  pthread_mutex_lock(&pcmhd_lock);
  ready = s.make ? 1 : resolve();
  pthread_mutex_unlock(&pcmhd_lock);
  if (!ready) {
    /* Never silently return NULL from a call every GStreamer pipeline depends on: say why. */
    fprintf(stderr, "pcmhd_sink: real gst_element_factory_make unavailable; element creation will fail\n");
    return 0;
  }
  element = s.make(factory_name, name);
  if (element && nj_pcmhd_armed && factory_name && !strcmp(factory_name, "audiosink"))
    install(element);
  return element;
}
