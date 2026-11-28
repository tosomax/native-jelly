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
 *   - Disarmed calls resolve only the real factory function and forward it. No probe symbol is
 *     required, so an unrelated GStreamer pipeline cannot be broken by this diagnostic.
 *   - Event/probe access uses public GStreamer functions; the sink property is verified through
 *     exported GObject set/get functions, not donor struct offsets or non-exported GLib macros.
 *   - Rust does not feed a carrier chunk until this probe reports accepted carrier CAPS. Missing
 *     interposition, unexpected caps, a missing property, or a rejected replacement all fail closed.
 *   - Both factory wrappers forward unconditionally; arming only adds the probe installation.
 *
 * `gst_pad_send_event()` returning true proves the downstream chain accepted the replacement; it
 * does NOT prove how LG's sink parsed it. That final step remains device-only evidence. The original
 * CAPS event is dropped only after acceptance; on refusal it is allowed to continue and Rust is
 * disarmed before any further carrier feed.
 *
 * Log: the event log through `nj_pcmhd_hooks_log()` (Rust) is the primary channel — the file at
 * /tmp/nativejelly-pcmhd-sink.log is only a secondary copy. A failed fopen() can no longer hide
 * whether an interposer was reached.
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
/* Rust export in rust-modules/src/player/es_carrier.rs: nj_base::eventlog::log, the channel the
 * television is already known to write. The host harness provides an equivalent stub. */
extern void nj_pcmhd_hooks_log(const char *message);

#define PCMHD_LOG "/tmp/nativejelly-pcmhd-sink.log"
#define PCMHD_LOG_LINE_MAX 640
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
typedef struct GstElementFactory GstElementFactory;
typedef struct GstPad GstPad;
typedef struct GstEvent GstEvent;
typedef struct GstCaps GstCaps;
typedef struct GstStructure GstStructure;
typedef struct GstPadProbeInfo GstPadProbeInfo;

GstElement *gst_element_factory_make(const char *factory, const char *name);
GstElement *gst_element_factory_create(GstElementFactory *factory, const char *name);

typedef GstElement *(*make_fn)(const char *, const char *);
typedef GstElement *(*create_fn)(GstElementFactory *, const char *);
/* GstElementFactory is a GstObject. GST_OBJECT_NAME()/GST_PLUGIN_FEATURE_NAME() are macros; the
 * exported accessor below is the one that works through dlsym (verified in libgstreamer-1.0). */
typedef char *(*object_get_name_fn)(void *);
typedef void (*g_free_fn)(void *);
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
typedef void (*g_object_set_fn)(void *, const char *, ...);
typedef void (*g_object_get_fn)(void *, const char *, ...);

struct syms {
  int make_ready;
  int create_ready;
  int probe_ready;
  make_fn make;
  create_fn create;
  object_get_name_fn object_get_name;
  g_free_fn g_free_sym;
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
  g_object_set_fn g_object_set;
  g_object_get_fn g_object_get;
};

static struct syms s;
static pthread_mutex_t pcmhd_lock = PTHREAD_MUTEX_INITIALIZER;
static GstPad *probe_pad;
static unsigned long probe_id;
static __thread int in_carrier;
/* Real gst_element_factory_make may implement itself by calling gst_element_factory_create. Such a
 * nested create is an implementation detail of MAKE, not a libpf callsite: it must be forwarded
 * without counting, logging or installing a second probe. */
static __thread int in_real_make;
/* Diagnostics are per installation/playback, not process-global and not racy. */
static _Atomic unsigned told_caps;
static _Atomic unsigned told_rewrite;
static _Atomic unsigned told_refused;
/* Entry counts are deliberately process-lifetime: the Rust refusal line must be able to say the
 * wrappers were NEVER entered even when every log file is missing. */
static _Atomic uint64_t make_calls_total;
static _Atomic uint64_t create_calls_total;

uint64_t nj_pcmhd_make_calls(void) {
  return atomic_load_explicit(&make_calls_total, memory_order_relaxed);
}

uint64_t nj_pcmhd_create_calls(void) {
  return atomic_load_explicit(&create_calls_total, memory_order_relaxed);
}

/* One HOOK line per (wrapper, factory, session); bounded so a library that creates a different
 * factory every frame cannot fill the event log. `keys` are the caller's factory-name pointer for
 * make and the GstElementFactory pointer for create; `owned` holds the g_free-able name copy that
 * gst_object_get_name() returns, reused for every create with the same factory pointer. */
#define PCMHD_HOOK_LOG_SLOTS 16
struct hook_log_state {
  uint64_t session;
  const void *keys[PCMHD_HOOK_LOG_SLOTS];
  char *owned[PCMHD_HOOK_LOG_SLOTS];
  unsigned used;
};
static struct hook_log_state make_logged;
static struct hook_log_state create_logged;

/* lib() never loads a new library for a name that is not already present unless the call can
 * legitimately need it; g_free is the public GLib counterpart of gst_object_get_name(). */
static void free_name_locked(char *name) {
  if (!name)
    return;
  if (!s.g_free_sym) {
    void *glib = lib("libglib-2.0.so.0");
    if (glib)
      *(void **)(&s.g_free_sym) = dlsym(glib, "g_free");
  }
  if (s.g_free_sym)
    s.g_free_sym(name);
}

/* pcmhd_lock must be held. */
static void clear_hook_log_locked(struct hook_log_state *st) {
  for (unsigned i = 0; i < st->used; i++)
    free_name_locked(st->owned[i]);
  memset(st, 0, sizeof(*st));
}

/* Event log FIRST: the dedicated /tmp file may be unwritable on the device, and a silent fopen
 * failure there is exactly what made the previous run indistinguishable from "hook never called".
 * The file is kept as a secondary copy for the existing runbook. */
static void sink_log(const char *fmt, ...) {
  char line[PCMHD_LOG_LINE_MAX];
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int prefix = snprintf(line, sizeof(line), "%ld.%03ld ",
                        (long)now.tv_sec, now.tv_nsec / 1000000L);
  if (prefix < 0 || (size_t)prefix >= sizeof(line))
    return;
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(line + prefix, sizeof(line) - prefix, fmt, args);
  va_end(args);
  if (written < 0)
    return;
  nj_pcmhd_hooks_log(line);
  FILE *f = fopen(PCMHD_LOG, "a");
  if (f) {
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
  }
}

/* pcmhd_lock must be held. Callers that first borrow a cached name MUST sync the session before
 * that lookup, so a stale cached pointer cannot survive into a new generation. */
static void sync_hook_log_session_locked(struct hook_log_state *st, uint64_t session) {
  if (st->session != session) {
    clear_hook_log_locked(st);
    st->session = session;
  }
}

/* pcmhd_lock must be held. Returns 1 when `owned` was adopted into the table; the caller must
 * free it otherwise. The name is the library's own string (make) or a cached copy (create). */
static int log_factory_call_locked(const char *kind, const char *factory,
                                   const void *key, char *owned,
                                   int armed, uint64_t session,
                                   struct hook_log_state *st) {
  sync_hook_log_session_locked(st, session);
  for (unsigned i = 0; i < st->used; i++)
    if (st->keys[i] == key)
      return 0;
  if (st->used >= PCMHD_HOOK_LOG_SLOTS)
    return 0;
  st->keys[st->used] = key;
  st->owned[st->used] = owned;
  st->used++;
  sink_log("HOOK %s factory=%s armed=%d session=%llu",
           kind, factory ? factory : "(null)", armed, (unsigned long long)session);
  return 1;
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

/* Same resolution scheme as make; the real create is needed even when disarmed, so forwarding is
 * unconditional. The export itself lives only in the PCMHD_HOOK branch of the Makefile. */
static int resolve_create_locked(void) {
  if (s.create_ready)
    return 1;
  void *gst = lib("libgstreamer-1.0.so.0");
  if (!gst)
    return 0;
  *(void **)(&s.create) = dlsym(gst, "gst_element_factory_create");
  if ((void *)s.create == (void *)&gst_element_factory_create) {
    *(void **)(&s.create) = dlsym(RTLD_NEXT, "gst_element_factory_create");
    if ((void *)s.create == (void *)&gst_element_factory_create)
      s.create = 0;
  }
  if (!s.create)
    return 0;
  s.create_ready = 1;
  return 1;
}

/* Borrowed name from this session's cache, if the factory was already seen. */
static const char *cached_factory_name_locked(GstElementFactory *factory) {
  for (unsigned i = 0; i < create_logged.used; i++)
    if (create_logged.keys[i] == (const void *)factory && create_logged.owned[i])
      return create_logged.owned[i];
  return 0;
}

/* gst_object_get_name() returns a g_free-able copy; the create wrapper caches one per factory
 * pointer and releases it on a session change or disarm. */
static char *new_factory_name_locked(GstElementFactory *factory) {
  if (!factory)
    return 0;
  if (!s.object_get_name) {
    void *gst = lib("libgstreamer-1.0.so.0");
    if (!gst)
      return 0;
    *(void **)(&s.object_get_name) = dlsym(gst, "gst_object_get_name");
  }
  return s.object_get_name ? s.object_get_name(factory) : 0;
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
  *(void **)(&s.g_object_set) = dlsym(gobj, "g_object_set");
  *(void **)(&s.g_object_get) = dlsym(gobj, "g_object_get");
  if (!s.g_object_set || !s.g_object_get) {
    sink_log("NOT ARMED: libgobject lacks public property set/get functions");
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
  /* G_OBJECT_GET_CLASS/G_TYPE_INSTANCE_GET_CLASS are macros, not exported functions. Verify the
   * property through the public varargs set/get ABI instead; a missing or write-only property
   * leaves the sentinel unchanged and fails closed. GLib may log a warning for a missing name. */
  int pcm_multi_ch = -1;
  s.g_object_set(sink, "pcm-multi-ch", 1, 0);
  s.g_object_get(sink, "pcm-multi-ch", &pcm_multi_ch, 0);
  if (pcm_multi_ch != 1) {
    s.remove_probe(pad, probe_id);
    s.object_unref(pad);
    probe_pad = 0;
    probe_id = 0;
    refuse_carrier(session, "audiosink pcm-multi-ch set/get round trip did not return true");
    return;
  }
  sink_log("session=%llu audiosink found: probe %lu on sink pad, pcm-multi-ch set/get verified",
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
  /* A new generation gets fresh HOOK lines and releases cached GStreamer names. */
  clear_hook_log_locked(&make_logged);
  clear_hook_log_locked(&create_logged);
  pthread_mutex_unlock(&pcmhd_lock);
}

GstElement *gst_element_factory_make(const char *factory_name, const char *name) {
  make_fn make;
  uint64_t session = nj_pcmhd_session_load();
  int probe_ready = 0;
  int armed = nj_pcmhd_armed_load();
  int audiosink = factory_name && !strcmp(factory_name, "audiosink");

  atomic_fetch_add_explicit(&make_calls_total, 1, memory_order_relaxed);
  pthread_mutex_lock(&pcmhd_lock);
  log_factory_call_locked("make", factory_name, factory_name, 0,
                          armed, session, &make_logged);
  int make_ready = resolve_make_locked();
  make = s.make;
  if (make_ready && armed && audiosink)
    probe_ready = resolve_probe_locked();
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

  /* Guard ONLY the real call: a create it invokes internally must not masquerade as a libpf
   * callsite. Save/restore so a re-entrant make cannot clear an outer guard. */
  int previous = in_real_make;
  in_real_make = 1;
  GstElement *element = make(factory_name, name);
  in_real_make = previous;
  if (element && nj_pcmhd_armed_load() && audiosink && session == nj_pcmhd_session_load()) {
    pthread_mutex_lock(&pcmhd_lock);
    install_locked(element, session);
    pthread_mutex_unlock(&pcmhd_lock);
  }
  return element;
}

/* The alternative callsite named in docs/agent-reports/pcmhd-sink-callsite-analysis-2026-11-28.md.
 * Forwarding is unconditional; installation happens only for an armed, current audiosink session,
 * through exactly the same install_locked() as make. No caps or feed logic is duplicated here. */
GstElement *gst_element_factory_create(GstElementFactory *factory, const char *name) {
  create_fn create;
  uint64_t session = nj_pcmhd_session_load();
  int probe_ready = 0;
  int armed = nj_pcmhd_armed_load();
  int audiosink = 0;

  /* The real make may call create internally. That nested call is an implementation detail of
   * make, not a libpf callsite: resolve and forward it, but do not count it, log it, or install
   * a probe. A direct libpf create takes the full path below. */
  if (in_real_make) {
    pthread_mutex_lock(&pcmhd_lock);
    int nested_ready = resolve_create_locked();
    create = s.create;
    pthread_mutex_unlock(&pcmhd_lock);
    return nested_ready ? create(factory, name) : 0;
  }

  atomic_fetch_add_explicit(&create_calls_total, 1, memory_order_relaxed);
  pthread_mutex_lock(&pcmhd_lock);
  sync_hook_log_session_locked(&create_logged, session);
  const char *factory_name = cached_factory_name_locked(factory);
  char *fresh_name = factory_name ? 0 : new_factory_name_locked(factory);
  if (!factory_name && fresh_name)
    factory_name = fresh_name;
  /* Key by the factory pointer: repeated creates neither spam the log nor re-read the name. */
  int adopted = log_factory_call_locked("create", factory_name, factory,
                                        fresh_name, armed, session, &create_logged);
  int create_ready = resolve_create_locked();
  create = s.create;
  audiosink = factory_name && !strcmp(factory_name, "audiosink");
  if (create_ready && armed && audiosink)
    probe_ready = resolve_probe_locked();
  if (fresh_name && !adopted)
    free_name_locked(fresh_name);
  pthread_mutex_unlock(&pcmhd_lock);

  if (!create_ready) {
    if (armed)
      sink_log("NOT ARMED: real gst_element_factory_create unavailable");
    return 0;
  }
  if (armed && audiosink && !probe_ready)
    refuse_carrier(session, "probe symbols unavailable; factory_create still forwarded");

  GstElement *element = create(factory, name);
  if (element && nj_pcmhd_armed_load() && audiosink && session == nj_pcmhd_session_load()) {
    pthread_mutex_lock(&pcmhd_lock);
    install_locked(element, session);
    pthread_mutex_unlock(&pcmhd_lock);
  }
  return element;
}
