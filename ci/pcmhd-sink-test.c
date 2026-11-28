/* Host regression harness for src/pcmhd_sink.c. No GStreamer installation is needed: dlsym/dlopen
 * are link-wrapped and every public API used by the hook has a small fake. This exercises the real
 * interposer source, including its static probe/install/disarm paths. It does NOT prove libpf's
 * target callsite or LG sink semantics; the runtime CAPS handshake covers a missing callsite by
 * refusing to feed. Build/run command: ci/test-pcmhd-sink.sh (not run by the source-only lane).
 */
/* pcmhd_sink.c needs clock_gettime/CLOCK_MONOTONIC, which glibc only declares when
 * _GNU_SOURCE is set before the FIRST system header is included. This harness includes
 * system headers before it includes the interposer source, so define the guard here;
 * the identical guard in pcmhd_sink.c remains for standalone compilation. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/pcmhd_sink.c"

struct GstStructure {
  const char *name;
  const char *format;
  const char *layout;
  int rate;
  int channels;
  int es; /* -1 absent, -2 present but not boolean, 0 false, 1 true */
  int clock_x4;
  const char *codec;
  int data_rate;
  int data_channels;
  int data_bps;
  unsigned int acmod;
  uint64_t channel_mask;
};

struct GstCaps {
  GstStructure *structure;
};

struct GstEvent {
  int type;
  GstCaps *caps;
};

struct GstPad {
  int refs;
  int probes;
  unsigned long id;
  int (*callback)(GstPad *, void *, void *);
  void *user;
};

struct GstElement {
  int refs;
  int property;
  int has_property;
  GstPad pad;
};

/* Deliberately only one pointer at offset zero. The old info+8 ABI guess would read garbage. */
typedef struct FakeProbeInfo {
  GstEvent *event;
} FakeProbeInfo;

static GstElement element;
static GstElement replacement_element;
static GstStructure carrier_structure;
static GstCaps carrier_caps;
static GstEvent replacement_event;
/* Arbitrary on purpose: the hook must ask gst_event_type_get_name(), not compare a donor enum. */
#define FAKE_CAPS_EVENT_TYPE 777
static int armed;
static uint64_t session;
static int ready_calls;
static int refused_calls;
static int make_calls;
static int make_missing;
static int make_calls_create; /* fake real make calls the create wrapper internally */
static int probe_symbols_missing;
static int add_probe_fail;
static int add_probe_calls;
static int send_result;
static int send_calls;
static int nested_return;
static GstEvent *last_sent;
/* Stub for the Rust export used by sink_log(); the device build writes the real event log. */
static char hook_log_blob[8192];
static int hook_log_calls;

void nj_pcmhd_hooks_log(const char *message) {
  hook_log_calls++;
  size_t used = strlen(hook_log_blob);
  snprintf(hook_log_blob + used, sizeof(hook_log_blob) - used, "%s\n",
           message ? message : "(null)");
}

int nj_pcmhd_armed_load(void) { return armed; }
void nj_pcmhd_armed_store(int value) { armed = value != 0; }
uint64_t nj_pcmhd_session_load(void) { return session; }
void nj_pcmhd_sink_ready(uint64_t seen) { if (seen == session) ready_calls++; }
void nj_pcmhd_sink_refused(uint64_t seen) { if (seen == session) { refused_calls++; armed = 0; } }

struct GstElementFactory { const char *name; };
static GstElementFactory audiosink_factory = { "audiosink" };
static GstElementFactory identity_factory = { "identity" };
static int fake_create_count;
static int create_missing;

static GstElement *fake_make(const char *factory, const char *name) {
  make_calls++;
  /* Real GStreamer's make may be implemented through create; exercise that nested callsite. */
  if (make_calls_create)
    return gst_element_factory_create(&audiosink_factory, name);
  if (!factory || strcmp(factory, "audiosink"))
    return &replacement_element;
  return make_calls % 2 ? &element : &replacement_element;
}

static GstElement *fake_create(GstElementFactory *factory, const char *name) {
  (void)name;
  fake_create_count++;
  if (!factory || strcmp(factory->name, "audiosink"))
    return &replacement_element;
  return fake_create_count % 2 ? &element : &replacement_element;
}

static const char *fake_object_get_name(GstElementFactory *factory) {
  return factory ? factory->name : 0;
}
/* The fake name accessor returns static strings, so GLib's real allocator is not involved. */
static void fake_g_free(void *ptr) { (void)ptr; }

static GstPad *fake_get_static_pad(GstElement *sink, const char *name) {
  assert(name && !strcmp(name, "sink"));
  sink->pad.refs++;
  return &sink->pad;
}

static unsigned long fake_add_probe(GstPad *pad, unsigned int mask, int (*callback)(GstPad *, void *, void *), void *user, void (*destroy)(void *)) {
  (void)destroy;
  assert(mask == PCMHD_EVENT_PROBE_MASK);
  if (add_probe_fail)
    return 0;
  add_probe_calls++;
  pad->probes++;
  pad->id = 1000 + pad->probes;
  pad->callback = callback;
  pad->user = user;
  return pad->id;
}

static void fake_remove_probe(GstPad *pad, unsigned long id) {
  assert(id == pad->id);
  if (pad->probes > 0)
    pad->probes--;
}

static int fake_send_event(GstPad *pad, GstEvent *event) {
  send_calls++;
  last_sent = event;
  if (send_result && pad->callback) {
    FakeProbeInfo info = { event };
    nested_return = pad->callback(pad, &info, pad->user);
  }
  return send_result;
}

static GstEvent *fake_event_new_caps(GstCaps *caps) {
  replacement_event.type = FAKE_CAPS_EVENT_TYPE;
  replacement_event.caps = caps;
  return &replacement_event;
}

static void fake_event_parse_caps(GstEvent *event, GstCaps **caps) { *caps = event->caps; }
static GstEvent *fake_probe_info_get_event(GstPadProbeInfo *info) { return ((FakeProbeInfo *)info)->event; }
static int fake_event_get_type(const GstEvent *event) { return event->type; }
static const char *fake_event_type_get_name(int type) { return type == FAKE_CAPS_EVENT_TYPE ? "caps" : "other"; }
static GstCaps *fake_caps_from_string(const char *caps) { assert(!strcmp(caps, PCMHD_CAPS)); return &carrier_caps; }
static GstStructure *fake_caps_get_structure(const GstCaps *caps, unsigned int index) { return index == 0 ? caps->structure : 0; }
static const char *fake_structure_get_name(const GstStructure *st) { return st->name; }
static const char *fake_structure_get_string(const GstStructure *st, const char *field) {
  if (!strcmp(field, "format")) return st->format;
  if (!strcmp(field, "layout")) return st->layout;
  if (!strcmp(field, "codec-str")) return st->codec;
  return 0;
}
static int fake_structure_get_int(const GstStructure *st, const char *field, int *value) {
  if (!strcmp(field, "rate")) *value = st->rate;
  else if (!strcmp(field, "channels")) *value = st->channels;
  else if (!strcmp(field, "data-rate")) *value = st->data_rate;
  else if (!strcmp(field, "data-channels")) *value = st->data_channels;
  else if (!strcmp(field, "data-bps")) *value = st->data_bps;
  else return 0;
  return 1;
}
static int fake_structure_get_uint(const GstStructure *st, const char *field, unsigned int *value) {
  if (strcmp(field, "acmod")) return 0;
  *value = st->acmod;
  return 1;
}
static int fake_structure_get_uint64(const GstStructure *st, const char *field, uint64_t *value) {
  if (strcmp(field, "channel-mask")) return 0;
  *value = st->channel_mask;
  return 1;
}
static int fake_structure_get_boolean(const GstStructure *st, const char *field, int *value) {
  if (!strcmp(field, "es")) {
    if (st->es < 0) return 0;
    *value = st->es;
    return 1;
  }
  if (!strcmp(field, "clock-x4")) {
    *value = st->clock_x4;
    return 1;
  }
  return 0;
}
static void fake_mini_object_unref(void *object) { assert(object == &carrier_caps); }
static void fake_object_unref(void *object) {
  GstPad *pad = object;
  assert(pad->refs > 0);
  pad->refs--;
}
static void fake_g_object_set(void *object, const char *property, ...) {
  GstElement *sink = object;
  va_list args;
  assert(!strcmp(property, "pcm-multi-ch"));
  va_start(args, property);
  int value = va_arg(args, int);
  (void)va_arg(args, int); /* documented varargs terminator */
  va_end(args);
  if (sink->has_property)
    sink->property = value;
}
static void fake_g_object_get(void *object, const char *property, ...) {
  GstElement *sink = object;
  va_list args;
  assert(!strcmp(property, "pcm-multi-ch"));
  va_start(args, property);
  int *value = va_arg(args, int *);
  (void)va_arg(args, int); /* documented varargs terminator */
  va_end(args);
  if (sink->has_property)
    *value = sink->property;
}

void *__wrap_dlopen(const char *soname, int flags) {
  (void)flags;
  return !strcmp(soname, "libgstreamer-1.0.so.0") || !strcmp(soname, "libgobject-2.0.so.0")
    ? (void *)(uintptr_t)1 : 0;
}

void *__wrap_dlsym(void *handle, const char *name) {
  (void)handle;
  if (!strcmp(name, "gst_element_factory_make")) return make_missing ? 0 : (void *)fake_make;
  if (!strcmp(name, "gst_element_factory_create")) return create_missing ? 0 : (void *)fake_create;
#define SYM(symbol, function) if (!strcmp(name, symbol)) return (void *)function
  /* Not a probe symbol: the create wrapper needs the factory name even when probes are missing. */
  SYM("gst_object_get_name", fake_object_get_name);
  SYM("g_free", fake_g_free);
  /* GLib exposes G_TYPE_INSTANCE_GET_CLASS as a macro, not an exported function. */
  if (!strcmp(name, "g_type_instance_get_class")) return 0;
  if (probe_symbols_missing) return 0;
  SYM("gst_element_get_static_pad", fake_get_static_pad);
  SYM("gst_pad_add_probe", fake_add_probe);
  SYM("gst_pad_remove_probe", fake_remove_probe);
  SYM("gst_pad_send_event", fake_send_event);
  SYM("gst_event_new_caps", fake_event_new_caps);
  SYM("gst_event_parse_caps", fake_event_parse_caps);
  SYM("gst_pad_probe_info_get_event", fake_probe_info_get_event);
  SYM("gst_event_get_type", fake_event_get_type);
  SYM("gst_event_type_get_name", fake_event_type_get_name);
  SYM("gst_caps_from_string", fake_caps_from_string);
  SYM("gst_caps_get_structure", fake_caps_get_structure);
  SYM("gst_structure_get_name", fake_structure_get_name);
  SYM("gst_structure_get_int", fake_structure_get_int);
  SYM("gst_structure_get_uint", fake_structure_get_uint);
  SYM("gst_structure_get_uint64", fake_structure_get_uint64);
  SYM("gst_structure_get_string", fake_structure_get_string);
  SYM("gst_structure_get_boolean", fake_structure_get_boolean);
  SYM("gst_mini_object_unref", fake_mini_object_unref);
  SYM("gst_object_unref", fake_object_unref);
  SYM("g_object_set", fake_g_object_set);
  SYM("g_object_get", fake_g_object_get);
#undef SYM
  return 0;
}

static void base_caps(GstStructure *st, int es) {
  memset(st, 0, sizeof(*st));
  st->name = "audio/x-raw";
  st->format = "S16LE";
  st->layout = "interleaved";
  st->rate = 192000;
  st->channels = 8;
  st->es = es;
  st->clock_x4 = 1;
  st->codec = "mat";
  st->data_rate = 192000;
  st->data_channels = 8;
  st->data_bps = 16;
  st->acmod = 0;
  st->channel_mask = 0xc3f;
}

static void reset(void) {
  nj_pcmhd_disarm();
  memset(&s, 0, sizeof(s));
  probe_pad = 0;
  probe_id = 0;
  in_carrier = 0;
  memset(&element, 0, sizeof(element));
  memset(&replacement_element, 0, sizeof(replacement_element));
  element.has_property = replacement_element.has_property = 1;
  base_caps(&carrier_structure, 1);
  carrier_caps.structure = &carrier_structure;
  memset(&replacement_event, 0, sizeof(replacement_event));
  armed = 0;
  session = 1;
  ready_calls = refused_calls = make_calls = 0;
  fake_create_count = 0;
  make_calls_create = 0;
  add_probe_calls = 0;
  hook_log_calls = 0;
  hook_log_blob[0] = 0;
  make_missing = create_missing = probe_symbols_missing = add_probe_fail = 0;
  send_result = 1;
  send_calls = 0;
  nested_return = -1;
  last_sent = 0;
  reset_counters_locked();
}

static int install_for_test(void) {
  armed = 1;
  GstElement *made = gst_element_factory_make("audiosink", 0);
  assert(made);
  assert(make_calls == 1);
  assert(probe_pad && probe_id && probe_pad->probes == 1 && probe_pad->refs == 1);
  assert(element.property == 1 || replacement_element.property == 1);
  return 0;
}

static int invoke_caps(GstStructure *st) {
  GstCaps caps = { st };
  GstEvent event = { FAKE_CAPS_EVENT_TYPE, &caps };
  FakeProbeInfo info = { &event };
  assert(probe_pad && probe_pad->callback);
  return probe_pad->callback(probe_pad, &info, probe_pad->user);
}

static void expect_caps(int expected, GstStructure *st, const char *what) {
  int got = invoke_caps(st);
  if (got != expected) {
    fprintf(stderr,
            "CAPS case '%s': got=%d expected=%d armed=%d ready=%d refused=%d sends=%d event_type=%d\n",
            what, got, expected, armed, ready_calls, refused_calls, send_calls, FAKE_CAPS_EVENT_TYPE);
  }
  assert(got == expected);
}

static void test_disarmed_and_non_audio_need_only_real_make(void) {
  uint64_t before = nj_pcmhd_make_calls();
  reset();
  probe_symbols_missing = 1;
  assert(gst_element_factory_make("identity", 0));
  assert(make_calls == 1 && s.make_ready && !s.probe_ready);
  assert(strstr(hook_log_blob, "HOOK make factory=identity armed=0 session=1"));
  assert(hook_log_calls == 1); /* one line per factory name per session */
  assert(gst_element_factory_make("identity", 0));
  assert(hook_log_calls == 1);
  assert(gst_element_factory_make("audiosink", 0));
  assert(make_calls == 3 && !probe_pad && refused_calls == 0);
  assert(strstr(hook_log_blob, "HOOK make factory=audiosink armed=0 session=1"));
  assert(nj_pcmhd_make_calls() == before + 3);

  reset();
  probe_symbols_missing = 1;
  armed = 1;
  assert(gst_element_factory_make("identity", 0));
  assert(make_calls == 1 && !s.probe_ready && refused_calls == 0);
  assert(strstr(hook_log_blob, "HOOK make factory=identity armed=1 session=1"));

  reset();
  probe_symbols_missing = 1;
  armed = 1;
  assert(gst_element_factory_make("audiosink", 0));
  assert(make_calls == 1 && refused_calls == 1 && armed == 0 && !probe_pad);
  assert(strstr(hook_log_blob, "HOOK make factory=audiosink armed=1 session=1"));
}

static void test_factory_create_forwards_diagnoses_and_installs(void) {
  uint64_t make_before = nj_pcmhd_make_calls();
  uint64_t create_before = nj_pcmhd_create_calls();

  reset();
  assert(gst_element_factory_create(&identity_factory, 0) == &replacement_element);
  assert(fake_create_count == 1 && !probe_pad && refused_calls == 0);
  assert(nj_pcmhd_create_calls() == create_before + 1);
  assert(nj_pcmhd_make_calls() == make_before);
  assert(strstr(hook_log_blob, "HOOK create factory=identity armed=0 session=1"));
  assert(hook_log_calls == 1); /* repeated factory in one session is logged once */
  gst_element_factory_create(&identity_factory, 0);
  assert(fake_create_count == 2 && hook_log_calls == 1);

  /* make and create are distinguishable even for the same factory name and session. */
  gst_element_factory_make("identity", 0);
  assert(strstr(hook_log_blob, "HOOK make factory=identity armed=0 session=1"));
  assert(hook_log_calls == 2);

  reset();
  armed = 1;
  assert(gst_element_factory_create(&audiosink_factory, 0));
  assert(fake_create_count == 1 && probe_pad && probe_id && refused_calls == 0);
  assert(strstr(hook_log_blob, "HOOK create factory=audiosink armed=1 session=1"));
  nj_pcmhd_disarm();

  reset();
  probe_symbols_missing = 1;
  armed = 1;
  assert(gst_element_factory_create(&audiosink_factory, 0));
  assert(refused_calls == 1 && armed == 0 && !probe_pad);

  reset();
  create_missing = 1;
  armed = 1;
  assert(gst_element_factory_create(&audiosink_factory, 0) == 0);
  assert(fake_create_count == 0 && refused_calls == 0);
  assert(strstr(hook_log_blob, "NOT ARMED: real gst_element_factory_create unavailable"));
}

static void test_nested_create_from_real_make_is_silent(void) {
  uint64_t make_before = nj_pcmhd_make_calls();
  uint64_t create_before = nj_pcmhd_create_calls();

  /* Real make is implemented through create in this fixture. The nested create must be a plain
   * forward: no create counter, no HOOK line, and only make's single probe installation. */
  reset();
  armed = 1;
  make_calls_create = 1;
  assert(gst_element_factory_make("audiosink", 0));
  assert(make_calls == 1 && fake_create_count == 1);
  assert(nj_pcmhd_make_calls() == make_before + 1);
  assert(nj_pcmhd_create_calls() == create_before);
  assert(strstr(hook_log_blob, "HOOK make factory=audiosink armed=1 session=1"));
  assert(!strstr(hook_log_blob, "HOOK create"));
  assert(add_probe_calls == 1 && probe_pad && probe_pad->probes == 1);
  nj_pcmhd_disarm();

  /* A direct create is still a real callsite: counted, logged and installed as before. */
  reset();
  armed = 1;
  assert(gst_element_factory_create(&audiosink_factory, 0));
  assert(nj_pcmhd_create_calls() == create_before + 1);
  assert(strstr(hook_log_blob, "HOOK create factory=audiosink armed=1 session=1"));
  assert(add_probe_calls == 1 && probe_pad && probe_pad->probes == 1);
  nj_pcmhd_disarm();
}

static void test_pad_refs_install_replace_failure_and_disarm(void) {
  reset();
  install_for_test();
  GstPad *first = probe_pad;
  assert(first->refs == 1);
  armed = 1;
  assert(gst_element_factory_make("audiosink", 0));
  assert(make_calls == 2 && first->refs == 0 && first->probes == 0);
  assert(probe_pad && probe_pad->refs == 1 && probe_pad->probes == 1);
  nj_pcmhd_disarm();
  assert(!probe_pad && probe_id == 0 && probe_pad == 0);
  assert(element.pad.refs == 0 && replacement_element.pad.refs == 0);

  reset();
  add_probe_fail = 1;
  armed = 1;
  assert(gst_element_factory_make("audiosink", 0));
  assert(refused_calls == 1 && !probe_pad && probe_id == 0);
  assert(element.pad.refs == 0 && replacement_element.pad.refs == 0);
}

static void test_caps_false_absent_nonboolean_incomplete_and_complete(void) {
  const int es_values[] = { -1, 0, -2 };
  for (unsigned i = 0; i < sizeof(es_values) / sizeof(es_values[0]); i++) {
    reset();
    install_for_test();
    GstStructure st;
    base_caps(&st, es_values[i]);
    char what[64];
    snprintf(what, sizeof(what), "es fixture %d", es_values[i]);
    expect_caps(PCMHD_PROBE_DROP, &st, what);
    assert(send_calls == 1 && ready_calls == 1 && refused_calls == 0);
    assert(last_sent == &replacement_event && last_sent->caps == &carrier_caps);
    assert(nested_return == PCMHD_PROBE_OK); /* nested carrier CAPS must pass through */
    nj_pcmhd_disarm();
  }

  reset();
  install_for_test();
  GstStructure incomplete;
  base_caps(&incomplete, 1);
  incomplete.data_bps = 8; /* es=true but incomplete: still rewrite */
  expect_caps(PCMHD_PROBE_DROP, &incomplete, "es=true but incomplete carrier fields");
  assert(send_calls == 1 && ready_calls == 1 && refused_calls == 0);
  nj_pcmhd_disarm();

  reset();
  install_for_test();
  GstStructure complete;
  base_caps(&complete, 1);
  expect_caps(PCMHD_PROBE_OK, &complete, "complete carrier bypass");
  assert(send_calls == 0 && ready_calls == 1 && refused_calls == 0);
  nj_pcmhd_disarm();
}

static void test_rejected_replacement_preserves_original_and_refuses_feed(void) {
  reset();
  install_for_test();
  send_result = 0;
  GstStructure st;
  base_caps(&st, 0);
  expect_caps(PCMHD_PROBE_OK, &st, "rejected replacement preserves original");
  assert(send_calls == 1 && refused_calls == 1 && ready_calls == 0 && armed == 0);
  nj_pcmhd_disarm();
}

static void test_missing_property_and_unexpected_caps_refuse_feed(void) {
  reset();
  element.has_property = 0;
  armed = 1;
  assert(gst_element_factory_make("audiosink", 0));
  assert(refused_calls == 1 && !probe_pad && element.pad.refs == 0);

  reset();
  install_for_test();
  GstStructure st;
  base_caps(&st, 0);
  st.rate = 48000;
  st.channels = 2;
  expect_caps(PCMHD_PROBE_OK, &st, "unexpected negotiated shape refuses feed");
  assert(refused_calls == 1 && ready_calls == 0 && armed == 0 && send_calls == 0);
  nj_pcmhd_disarm();
}

int main(void) {
  test_disarmed_and_non_audio_need_only_real_make();
  test_factory_create_forwards_diagnoses_and_installs();
  test_nested_create_from_real_make_is_silent();
  test_pad_refs_install_replace_failure_and_disarm();
  test_caps_false_absent_nonboolean_incomplete_and_complete();
  test_rejected_replacement_preserves_original_and_refuses_feed();
  test_missing_property_and_unexpected_caps_refuse_feed();
  puts("pcmhd_sink host tests: OK");
  return 0;
}
