/* The webOS 25 PCM-carrier hook for rust-modules/src/player/es_carrier.rs -- DIAGNOSTIC ONLY.
 *
 * The Load declares PCM S16LE 192 kHz 8 ch while the demuxer feeds IEC 61937 carrier bytes. This
 * hook replaces the audiosink's CAPS event with the bitstream fields recovered from the donor sink:
 *
 *   es=true, clock-x4=true, codec-str="mat", data-rate=192000, data-channels=8, data-bps=16,
 *   acmod=0, channel-mask=0xc3f, plus the sink property `pcm-multi-ch=true`.
 *
 * SAFETY CONTRACT
 *   - Built/exported only with the Rust `devtriggers` feature (see Makefile).
 *   - Disarmed calls resolve only the real gst_element_factory_make and forward it. No probe symbol
 *     is required, so an unrelated GStreamer pipeline cannot be broken by this diagnostic.
 *   - Event/probe/GObject access uses public GStreamer/GObject functions, not donor struct offsets.
 *   - Rust does not feed a carrier chunk until this probe reports accepted carrier CAPS. Missing
 *     interposition, unexpected caps, a missing property, or a rejected replacement all fail closed.
 *
 * `gst_pad_send_event()` returning true proves the downstream chain accepted the replacement; it
 * does NOT prove how LG's sink parsed it. That final step remains device-only evidence. The original
 * CAPS event is dropped only after acceptance; on refusal it is allowed to continue and Rust is
 * disarmed before any further carrier feed.
 *
 * Log: /tmp/nativejelly-pcmhd-sink.log (append, opened per line).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Rust owns the atomics and the arming generation; C never shares a Rust static's object type. */
extern int nj_pcmhd_armed_load(void);
extern void nj_pcmhd_armed_store(int value);
extern uint64_t nj_pcmhd_session_load(void);
extern void nj_pcmhd_sink_ready(uint64_t session);
extern void nj_pcmhd_sink_refused(uint64_t session);

#define PCMHD_LOG "/tmp/nativejelly-pcmhd-sink.log"
#define PCMHD_CAPS \
  "audio/x-raw, format=(string)S16LE, layout=(string)interleaved, rate=(int)192000, " \
  "channels=(int)8, channel-mask=(bitmask)0xc3f, acmod=(uint)0, es=(boolean)true, " \
  "codec-str=(string)mat, clock-x4=(boolean)true, data-rate=(int)192000, " \
  "data-channels=(int)8, data-bps=(int)16"
/* Public GstPadProbeType EVENT_DOWNSTREAM|EVENT_FLUSH and GstPadProbeReturn OK/DROP. */
#define PCMHD_EVENT_PROBE_MASK 0x140u
#define PCMHD_PROBE_OK 0
#define PCMHD_PROBE_DROP 1

typedef struct GstElement GstElement;
typedef struct GstPad GstPad;
typedef struct GstEvent GstEvent;
typedef struct GstCaps GstCaps;
typedef struct GstStructure GstStructure;
typedef struct GstPadProbeInfo GstPadProbeInfo;

GstElement *gst_element_factory_make(const char *factory, const char *name);

typedef GstElement *(*make_fn)(const char *, const char *);
typedef GstPad *(*get_pad_fn)(GstElement *, const char *);
typedef unsigned long (*add_probe_fn)(GstPad *, unsigned int, int (*)(GstPad *, void *, void *), void *, void (*)(void *));
typedef void (*remove_probe_fn)(GstPad *, unsigned long);
typedef int (*send_event_fn)(GstPad *, GstEvent *);
typedef GstEvent *(*event_new_caps_fn)(GstCaps *);
typedef void (*event_parse_caps_fn)(GstEvent *, GstCaps **);
typedef GstEvent *(*probe_info_get_event_fn)(GstPadProbeInfo *);
typedef int (*event_get_type_fn)(const GstEvent *);
typedef const char *(*event_type_get_name_fn)(int);
typedef GstCaps *(*caps_from_string_fn)(const char *);
typedef GstStructure *(*caps_get_structure_fn)(const GstCaps *, unsigned int);
typedef const char *(*structure_get_name_fn)(const GstStructure *);
typedef int (*structure_get_int_fn)(const GstStructure *, const char *, int *);
typedef int (*structure_get_uint_fn)(const GstStructure *, const char *, unsigned int *);
typedef int (*structure_get_uint64_fn)(const GstStructure *, const char *, uint64_t *);
typedef const char *(*structure_get_string_fn)(const GstStructure *, const char *);
typedef int (*structure_get_boolean_fn)(const GstStructure *, const char *, int *);
typedef void (*mini_object_unref_fn)(void *);
typedef void (*object_unref_fn)(void *);
typedef void *(*object_class_find_property_fn)(void *, const char *);
typedef void (*g_object_set_fn)(void *, const char *, ...);
typedef unsigned long (*g_type_from_name_fn)(const char *);
typedef void *(*g_type_instance_get_class_fn)(void *, unsigned long);

struct syms {
  int make_ready;
  int probe_ready;
  make_fn make;
  get_pad_fn get_static_pad;
  add_probe_fn add_probe;
  remove_probe_fn remove_probe;
  send_event_fn send_event;
  event_new_caps_fn event_new_caps;
  event_parse_caps_fn event_parse_caps;
  probe_info_get_event_fn probe_info_get_event;
  event_get_type_fn event_get_type;
  event_type_get_name_fn event_type_get_name;
  caps_from_string_fn caps_from_string;
  caps_get_structure_fn caps_get_structure;
  structure_get_name_fn structure_get_name;
  structure_get_int_fn structure_get_int;
  structure_get_uint_fn structure_get_uint;
  structure_get_uint64_fn structure_get_uint64;
  structure_get_string_fn structure_get_string;
  structure_get_boolean_fn structure_get_boolean;
  mini_object_unref_fn mini_object_unref;
  object_unref_fn object_unref;
  object_class_find_property_fn class_find_property;
  g_object_set_fn g_object_set;
  g_type_from_name_fn g_type_from_name;
  g_type_instance_get_class_fn g_type_instance_get_class;
};

static struct syms s;
static pthread_mutex_t pcmhd_lock = PTHREAD_MUTEX_INITIALIZER;
static GstPad *probe_pad;
static unsigned long probe_id;
static __thread int in_carrier;
/* Diagnostics are per installation/playback, not process-global and not racy. */
static _Atomic unsigned told_caps;
static _Atomic unsigned told_rewrite;
static _Atomic unsigned told_refused;

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

static void reset_counters_locked(void) {
  atomic_store_explicit(&told_caps, 0, memory_order_relaxed);
  atomic_store_explicit(&told_rewrite, 0, memory_order_relaxed);
  atomic_store_explicit(&told_refused, 0, memory_order_relaxed);
}

static void refuse_carrier(uint64_t session, const char *why) {
  if (atomic_fetch_add_explicit(&told_refused, 1, memory_order_relaxed) == 0)
    sink_log("REFUSED session=%llu: %s", (unsigned long long)session, why);
  nj_pcmhd_sink_refused(session);
}

/* RTLD_NOLOAD never loads anything new; the plain retry covers a soname promoted before first use. */
static void *lib(const char *soname) {
  void *h = dlopen(soname, RTLD_NOW | RTLD_NOLOAD);
  if (!h)
    h = dlopen(soname, RTLD_NOW);
  return h;
}

/* Independent from every probe symbol: this is the only resolution a disarmed call may need. */
static int resolve_make_locked(void) {
  if (s.make_ready)
    return 1;
  void *gst = lib("libgstreamer-1.0.so.0");
  if (!gst)
    return 0;
  *(void **)(&s.make) = dlsym(gst, "gst_element_factory_make");
  if ((void *)s.make == (void *)&gst_element_factory_make) {
    *(void **)(&s.make) = dlsym(RTLD_NEXT, "gst_element_factory_make");
    if ((void *)s.make == (void *)&gst_element_factory_make)
      s.make = 0;
  }
  if (!s.make)
    return 0;
  s.make_ready = 1;
  return 1;
}

static int resolve_probe_locked(void) {
  if (s.probe_ready)
    return 1;
  void *gst = lib("libgstreamer-1.0.so.0");
  void *gobj = lib("libgobject-2.0.so.0");
  if (!gst || !gobj) {
    sink_log("NOT ARMED: libgstreamer/libgobject not loaded (gst=%p gobj=%p)", gst, gobj);
    return 0;
  }
#define SYM(fn, name) \
  do { \
    *(void **)(&s.fn) = dlsym(gst, name); \
    if (!s.fn) { \
      sink_log("NOT ARMED: libgstreamer has no %s", name); \
      return 0; \
    } \
  } while (0)
  SYM(get_static_pad, "gst_element_get_static_pad");
  SYM(add_probe, "gst_pad_add_probe");
  SYM(remove_probe, "gst_pad_remove_probe");
  SYM(send_event, "gst_pad_send_event");
  SYM(event_new_caps, "gst_event_new_caps");
  SYM(event_parse_caps, "gst_event_parse_caps");
  SYM(probe_info_get_event, "gst_pad_probe_info_get_event");
  SYM(event_get_type, "gst_event_get_type");
  SYM(event_type_get_name, "gst_event_type_get_name");
  SYM(caps_from_string, "gst_caps_from_string");
  SYM(caps_get_structure, "gst_caps_get_structure");
  SYM(structure_get_name, "gst_structure_get_name");
  SYM(structure_get_int, "gst_structure_get_int");
  SYM(structure_get_uint, "gst_structure_get_uint");
  SYM(structure_get_uint64, "gst_structure_get_uint64");
  SYM(structure_get_string, "gst_structure_get_string");
  SYM(structure_get_boolean, "gst_structure_get_boolean");
  SYM(mini_object_unref, "gst_mini_object_unref");
  SYM(object_unref, "gst_object_unref");
#undef SYM
  *(void **)(&s.class_find_property) = dlsym(gobj, "g_object_class_find_property");
  *(void **)(&s.g_object_set) = dlsym(gobj, "g_object_set");
  *(void **)(&s.g_type_from_name) = dlsym(gobj, "g_type_from_name");
  *(void **)(&s.g_type_instance_get_class) = dlsym(gobj, "g_type_instance_get_class");
  if (!s.class_find_property || !s.g_object_set || !s.g_type_from_name || !s.g_type_instance_get_class) {
    sink_log("NOT ARMED: libgobject lacks public class/property functions");
    return 0;
  }
  s.probe_ready = 1;
  sink_log("symbols resolved through public GStreamer/GObject APIs");
  return 1;
}

static int base_pcm_matches(GstStructure *st) {
  const char *name = s.structure_get_name(st);
  const char *format = s.structure_get_string(st, "format");
  const char *layout = s.structure_get_string(st, "layout");
  int rate = 0, channels = 0;
  return name && !strcmp(name, "audio/x-raw")
    && format && !strcmp(format, "S16LE")
    && layout && !strcmp(layout, "interleaved")
    && s.structure_get_int(st, "rate", &rate) && rate == 192000
    && s.structure_get_int(st, "channels", &channels) && channels == 8;
}

static int carrier_fields_match(GstStructure *st) {
  int clock_x4 = 0, data_rate = 0, data_channels = 0, data_bps = 0;
  unsigned int acmod = 1;
  uint64_t channel_mask = 0;
  const char *codec = s.structure_get_string(st, "codec-str");
  return s.structure_get_boolean(st, "clock-x4", &clock_x4) && clock_x4
    && codec && !strcmp(codec, "mat")
    && s.structure_get_int(st, "data-rate", &data_rate) && data_rate == 192000
    && s.structure_get_int(st, "data-channels", &data_channels) && data_channels == 8
    && s.structure_get_int(st, "data-bps", &data_bps) && data_bps == 16
    && s.structure_get_uint(st, "acmod", &acmod) && acmod == 0
    && s.structure_get_uint64(st, "channel-mask", &channel_mask) && channel_mask == 0xc3f;
}

/* 0 = not our declared PCM shape, 1 = rewrite required, 2 = already a complete carrier. */
static int is_our_pcm(GstCaps *caps) {
  GstStructure *st = s.caps_get_structure(caps, 0);
  int es = 0;
  if (!st || !base_pcm_matches(st))
    return 0;
  /* Absent, false, or non-boolean `es` all need the carrier fields. Only an explicit true plus
   * every expected carrier value is allowed to bypass the rewrite. */
  if (!s.structure_get_boolean(st, "es", &es) || !es)
    return 1;
  return carrier_fields_match(st) ? 2 : 1;
}

static void log_caps(uint64_t session, GstCaps *caps, int state) {
  GstStructure *st = s.caps_get_structure(caps, 0);
  const char *name = st ? s.structure_get_name(st) : 0;
  const char *format = st ? s.structure_get_string(st, "format") : 0;
  const char *layout = st ? s.structure_get_string(st, "layout") : 0;
  const char *codec = st ? s.structure_get_string(st, "codec-str") : 0;
  int rate = -1, channels = -1, es = -1, clock_x4 = -1, data_rate = -1, data_channels = -1, data_bps = -1;
  unsigned int acmod = 1;
  uint64_t mask = 0;
  if (st) {
    s.structure_get_int(st, "rate", &rate);
    s.structure_get_int(st, "channels", &channels);
    s.structure_get_boolean(st, "es", &es);
    s.structure_get_boolean(st, "clock-x4", &clock_x4);
    s.structure_get_int(st, "data-rate", &data_rate);
    s.structure_get_int(st, "data-channels", &data_channels);
    s.structure_get_int(st, "data-bps", &data_bps);
    s.structure_get_uint(st, "acmod", &acmod);
    s.structure_get_uint64(st, "channel-mask", &mask);
  }
  sink_log("session=%llu sink CAPS name=%s format=%s layout=%s rate=%d channels=%d es=%d clock-x4=%d codec-str=%s data=%d/%d/%d acmod=%u mask=%llx state=%d",
           (unsigned long long)session, name ? name : "?", format ? format : "?", layout ? layout : "?",
           rate, channels, es, clock_x4, codec ? codec : "?", data_rate, data_channels, data_bps,
           acmod, (unsigned long long)mask, state);
}

static int on_event(GstPad *pad, void *info, void *user) {
  uint64_t session = (uint64_t)(uintptr_t)user;
  GstEvent *event;
  GstCaps *caps = 0;
  int state;
  if (!session || session != nj_pcmhd_session_load() || !nj_pcmhd_armed_load() || !info || in_carrier)
    return PCMHD_PROBE_OK;
  /* Public accessors replace the donor-specific info+8 and event+36 reads. */
  event = s.probe_info_get_event((GstPadProbeInfo *)info);
  if (!event)
    return PCMHD_PROBE_OK;
  const char *type_name = s.event_type_get_name(s.event_get_type(event));
  if (!type_name || strcmp(type_name, "caps"))
    return PCMHD_PROBE_OK;
  s.event_parse_caps(event, &caps);
  if (!caps)
    return PCMHD_PROBE_OK;
  state = is_our_pcm(caps);
  if (atomic_fetch_add_explicit(&told_caps, 1, memory_order_relaxed) == 0)
    log_caps(session, caps, state);
  if (!state) {
    refuse_carrier(session, "sink negotiated something other than PCM S16LE 192 kHz 8 ch");
    return PCMHD_PROBE_OK;
  }
  if (state == 2) {
    if (atomic_fetch_add_explicit(&told_rewrite, 1, memory_order_relaxed) == 0)
      sink_log("session=%llu carrier CAPS already complete; downstream feed may start", (unsigned long long)session);
    nj_pcmhd_sink_ready(session);
    return PCMHD_PROBE_OK;
  }

  GstCaps *carrier = s.caps_from_string(PCMHD_CAPS);
  if (!carrier) {
    refuse_carrier(session, "gst_caps_from_string refused the carrier caps");
    return PCMHD_PROBE_OK;
  }
  GstEvent *sent = s.event_new_caps(carrier);
  s.mini_object_unref(carrier);
  if (!sent) {
    refuse_carrier(session, "gst_event_new_caps failed");
    return PCMHD_PROBE_OK;
  }
  in_carrier = 1;
  /* gst_pad_send_event consumes the event. The nested CAPS re-enters this probe and passes through. */
  int ok = s.send_event(pad, sent);
  in_carrier = 0;
  if (!ok) {
    refuse_carrier(session, "downstream rejected the replacement CAPS event; original CAPS preserved");
    return PCMHD_PROBE_OK;
  }
  if (atomic_fetch_add_explicit(&told_rewrite, 1, memory_order_relaxed) == 0)
    sink_log("session=%llu replacement carrier CAPS accepted downstream (send_event=1); original dropped", (unsigned long long)session);
  nj_pcmhd_sink_ready(session);
  return PCMHD_PROBE_DROP;
}

/* pcmhd_lock must be held. get_static_pad returns one transfer-full reference; it is stored directly
 * and released exactly once on replacement, add_probe failure, or disarm. */
static void install_locked(GstElement *sink, uint64_t session) {
  if (!nj_pcmhd_armed_load() || session != nj_pcmhd_session_load())
    return;
  if (probe_pad) {
    s.remove_probe(probe_pad, probe_id);
    s.object_unref(probe_pad);
    probe_pad = 0;
    probe_id = 0;
  }
  GstPad *pad = s.get_static_pad(sink, "sink");
  if (!pad) {
    refuse_carrier(session, "audiosink has no sink pad");
    return;
  }
  reset_counters_locked();
  probe_pad = pad;
  probe_id = s.add_probe(pad, PCMHD_EVENT_PROBE_MASK, on_event, (void *)(uintptr_t)session, 0);
  if (!probe_id) {
    s.object_unref(pad);
    probe_pad = 0;
    refuse_carrier(session, "gst_pad_add_probe failed");
    return;
  }
  unsigned long gobject_type = s.g_type_from_name("GObject");
  void *class = gobject_type ? s.g_type_instance_get_class(sink, gobject_type) : 0;
  if (!class || !s.class_find_property(class, "pcm-multi-ch")) {
    s.remove_probe(pad, probe_id);
    s.object_unref(pad);
    probe_pad = 0;
    probe_id = 0;
    refuse_carrier(session, "audiosink has no pcm-multi-ch property");
    return;
  }
  s.g_object_set(sink, "pcm-multi-ch", 1, 0);
  sink_log("session=%llu audiosink found: probe %lu on sink pad, pcm-multi-ch requested",
           (unsigned long long)session, probe_id);
}

/* Called by Rust whenever a playback starts or stops. Removes the hook and drops the pad reference
 * instead of leaving the last sink alive between playbacks. */
void nj_pcmhd_disarm(void) {
  pthread_mutex_lock(&pcmhd_lock);
  if (probe_pad) {
    s.remove_probe(probe_pad, probe_id);
    s.object_unref(probe_pad);
    probe_pad = 0;
    probe_id = 0;
  }
  reset_counters_locked();
  pthread_mutex_unlock(&pcmhd_lock);
}

GstElement *gst_element_factory_make(const char *factory_name, const char *name) {
  make_fn make;
  uint64_t session = 0;
  int probe_ready = 0;
  int armed = nj_pcmhd_armed_load();
  int audiosink = factory_name && !strcmp(factory_name, "audiosink");

  pthread_mutex_lock(&pcmhd_lock);
  int make_ready = resolve_make_locked();
  make = s.make;
  if (make_ready && armed && audiosink) {
    session = nj_pcmhd_session_load();
    probe_ready = resolve_probe_locked();
  }
  pthread_mutex_unlock(&pcmhd_lock);

  if (!make_ready) {
    /* This is the one unavoidable failure: without the real factory there is nothing to forward.
     * Probe symbols never cause it, and disarmed calls do not resolve them. */
    if (armed)
      sink_log("NOT ARMED: real gst_element_factory_make unavailable");
    return 0;
  }
  if (armed && audiosink && !probe_ready)
    refuse_carrier(session, "probe symbols unavailable; factory still forwarded");

  GstElement *element = make(factory_name, name);
  if (element && nj_pcmhd_armed_load() && audiosink && session == nj_pcmhd_session_load()) {
    pthread_mutex_lock(&pcmhd_lock);
    install_locked(element, session);
    pthread_mutex_unlock(&pcmhd_lock);
  }
  return element;
}
