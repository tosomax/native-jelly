#!/usr/bin/env python3
"""Exercise the real ELF gate with synthetic tools; no compiler, NDK, ELF or device needed."""
import concurrent.futures
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HOOK = "_ZN17StarfishMediaAPIs20callbackFunctionHookEixPKc"
LOAD = "_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_PvES2_"
PCMHD_MAKE = "gst_element_factory_make"
PCMHD_CREATE = "gst_element_factory_create"
FILLER = "synthetic-padding-line\n" * 100000  # More than 2 MiB, far beyond a pipe buffer.

FAKE_TOOL = r'''#!/usr/bin/env python3
import json, os, signal, sys, time
from pathlib import Path
signal.signal(signal.SIGPIPE, signal.SIG_DFL)
tool = Path(sys.argv[0]).name
key = tool + (":" + sys.argv[1] if tool == "readelf" else "")
spec = json.loads(Path(os.environ["ELF_TEST_SPEC"]).read_text())
if key == "readelf:-h" and "ELF_TEST_BARRIER" in os.environ:
    barrier = Path(os.environ["ELF_TEST_BARRIER"])
    (barrier / ("ready-" + os.environ["ELF_TEST_RUN"])).touch()
    until = time.monotonic() + 5
    while len(list(barrier.glob("ready-*"))) < 2:
        if time.monotonic() > until: sys.exit(99)
        time.sleep(0.01)
    scratch = sorted(str(p) for p in Path(os.environ["TMPDIR"]).glob("check-elf.*"))
    (barrier / ("seen-" + os.environ["ELF_TEST_RUN"])).write_text(json.dumps(scratch))
record = spec[key]
data = record["output"].encode()
while data:
    count = os.write(1, data)
    data = data[count:]
sys.exit(record.get("exit", 0))
'''


def defaults():
    return {key: {"output": value} for key, value in {
        "readelf:-h": "Class: ELF32\nMachine: ARM\nFlags: soft-float\nType: EXEC\n",
        "readelf:-A": "Tag_CPU_arch: v7\n",
        "readelf:--dyn-syms": f"1: 00000000 16 FUNC GLOBAL DEFAULT 1 {HOOK}\n"
                              "2: 00000010 64 FUNC GLOBAL DEFAULT 1 ioctl\n",
        "readelf:-rW": "No relocations\n",
        "readelf:-l": "  LOAD 0x000000 0x00010000 0x00010000\n",
        "readelf:-n": "Build ID: " + "a" * 40 + "\n",
        "readelf:-d": "Shared library: [libalpha.so]\nShared library: [libbeta.so]\n",
        "objdump": "  00: dmb ish\n" * 101,
        "strings": "YOUR_PMS_HOST\n",
    }.items()}


class ElfGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="elf-gate-tests-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "ci").mkdir()
        (self.root / "tools").mkdir()
        (self.root / "scratch").mkdir()
        source = (ROOT / "ci/check-elf.sh").read_text()
        # Baseline isolation ONLY: its absolute legacy scratch path must not clobber another
        # checkout. The fixed script has no such literal, so its copy is byte-for-byte unchanged.
        source = source.replace("/tmp/dt-needed.actual", str(self.root / "legacy-needed.actual"))
        self.script = self.root / "ci/check-elf.sh"
        self.script.write_text(source)
        (self.root / "ci/expected-dt-needed.txt").write_text("libalpha.so\nlibbeta.so\n")
        for name in ("readelf", "objdump", "strings"):
            path = self.root / "tools" / name
            path.write_text(FAKE_TOOL)
            path.chmod(0o755)
        self.spec = self.root / "spec.json"

    def run_gate(self, spec=None, extra_env=None):
        if spec is not None:
            self.spec.write_text(json.dumps(spec))
        env = os.environ.copy()
        env.update({"READELF": str(self.root / "tools/readelf"),
                    "OBJDUMP": str(self.root / "tools/objdump"),
                    "PATH": str(self.root / "tools") + os.pathsep + env.get("PATH", ""),
                    "TMPDIR": str(self.root / "scratch"), "CI": "true", "PCMHD_HOOK": "0",
                    "ELF_TEST_SPEC": str(self.spec)})
        env.update(extra_env or {})
        return subprocess.run(["bash", str(self.script), "synthetic.elf"], cwd=self.root,
                              env=env, capture_output=True, text=True, timeout=20)

    def assert_pass(self, spec):
        result = self.run_gate(spec)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("all ELF assertions passed", result.stdout)

    def test_small_valid_artifact(self):
        self.assert_pass(defaults())

    def test_early_required_placeholder_with_large_tail(self):
        spec = defaults()
        spec["strings"]["output"] = "YOUR_PMS_HOST\n" + FILLER
        self.assert_pass(spec)

    def test_early_forbidden_strings_with_placeholder_at_eof(self):
        for forbidden, diagnostic in [("/home/synthetic_builder/work/file.rs", "build-host paths"),
                                      ("/Users/synthetic_builder/work/file.rs", "build-host paths"),
                                      ("10.23.45.67", "private IP address")]:
            with self.subTest(forbidden=forbidden):
                spec = defaults()
                spec["strings"]["output"] = forbidden + "\n" + FILLER + "YOUR_PMS_HOST\n"
                result = self.run_gate(spec)
                self.assertNotEqual(result.returncode, 0, "forbidden value was allowed: " + result.stdout)
                self.assertIn(diagnostic, result.stdout)

    def test_three_component_locale_or_version_data_is_not_an_ipv4_address(self):
        spec = defaults()
        spec["strings"]["output"] += "10.11.12\n172.16.12\n192.168.12\n"
        self.assert_pass(spec)

    def test_private_shaped_subsequence_inside_longer_dotted_data_is_not_an_address(self):
        spec = defaults()
        spec["strings"]["output"] += "1.2.3.4.5.6.7.8.9.10.11.12.13.14\n"
        self.assert_pass(spec)

    def test_complete_private_ipv4_ranges_remain_forbidden(self):
        for address in ("10.11.12.13", "10.0.0.1", "10.255.255.255",
                        "172.16.0.1", "172.31.255.255", "192.168.0.1", "192.168.255.255"):
            with self.subTest(address=address):
                spec = defaults()
                spec["strings"]["output"] += "http://" + address + ":32400/library\n"
                result = self.run_gate(spec)
                self.assertNotEqual(result.returncode, 0, "private address was allowed")
                self.assertIn("private IP address", result.stdout)
                self.assertIn(address, result.stdout, "the gate must identify the full four-octet address")

    def test_the_sign_in_screens_example_address_is_not_a_compiled_in_host(self):
        # `jellyfin.login.server_hint`: an example of what to type, a LAN address on purpose, and
        # its pseudo-locale rendering. It is catalogue text, not a configured server.
        spec = defaults()
        spec["strings"]["output"] += "192.168.1.20:8096\n[!! 192.168.1.20:8096 !!]\n"
        self.assert_pass(spec)

    def test_only_the_exact_example_address_is_allowed(self):
        for near in ("http://192.168.1.20:32400/library", "192.168.1.20\n", "192.168.1.21:8096"):
            with self.subTest(near=near):
                spec = defaults()
                spec["strings"]["output"] += near + "\n"
                result = self.run_gate(spec)
                self.assertNotEqual(result.returncode, 0, "a private address was allowed: " + near)
                self.assertIn("private IP address", result.stdout)

    def test_the_allowed_example_is_the_catalogues_own_hint(self):
        catalogue = json.loads((Path(__file__).resolve().parents[1] / "locales/en/jellyfin.json").read_text())

        def values(node):
            if isinstance(node, dict):
                for key, value in node.items():
                    if key == "jellyfin.login.server_hint":
                        yield value["value"] if isinstance(value, dict) else value
                    else:
                        yield from values(value)
            elif isinstance(node, list):
                for item in node:
                    yield from values(item)

        hints = set(values(catalogue))
        script = (Path(__file__).resolve().parent / "check-elf.sh").read_text()
        self.assertEqual(len(hints), 1, hints)
        line = f"SERVER_HINT_EXAMPLE='{hints.pop()}'"
        self.assertTrue(line in script, f"check-elf.sh must allow exactly the hint the sign-in screen shows: {line}")

    def test_neighbouring_public_ipv4_ranges_are_allowed(self):
        spec = defaults()
        spec["strings"]["output"] += "9.11.12.13\n11.11.12.13\n172.15.255.255\n172.32.0.1\n192.167.0.1\n192.169.0.1\n"
        self.assert_pass(spec)

    def test_the_es_renderer_ioctl_hook_must_be_exported_and_defined(self):
        for rows, why in [(f"1: 00000000 16 FUNC GLOBAL DEFAULT 1 {HOOK}\n", "missing"),
                          (f"1: 00000000 16 FUNC GLOBAL DEFAULT 1 {HOOK}\n2: 00000000 0 FUNC GLOBAL DEFAULT UND ioctl\n",
                           "only imported")]:
            with self.subTest(why):
                spec = defaults()
                spec["readelf:--dyn-syms"]["output"] = rows
                result = self.run_gate(spec)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("ioctl hook", result.stdout)

    def test_the_pcm_carrier_hook_is_feature_aware(self):
        make = f"3: 00000020 64 FUNC GLOBAL DEFAULT 1 {PCMHD_MAKE}\n"
        create = f"4: 00000030 64 FUNC GLOBAL DEFAULT 1 {PCMHD_CREATE}\n"
        spec = defaults()
        spec["readelf:--dyn-syms"]["output"] += make + create
        result = self.run_gate(spec, extra_env={"PCMHD_HOOK": "1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"devtriggers: {PCMHD_MAKE} hook exported", result.stdout)
        self.assertIn(f"devtriggers: {PCMHD_CREATE} hook exported", result.stdout)

        result = self.run_gate(spec, extra_env={"PCMHD_HOOK": "0"})
        self.assertNotEqual(result.returncode, 0, "a release artifact must not carry the hooks")
        self.assertIn("non-devtriggers build must not define/import", result.stdout)

        spec = defaults()
        result = self.run_gate(spec, extra_env={"PCMHD_HOOK": "1"})
        self.assertNotEqual(result.returncode, 0, "a devtriggers artifact must export both hooks")
        self.assertIn("not present exactly once", result.stdout)

        # One interposer alone is not enough: libpf may use either callsite.
        for missing, present in [("make", create), ("create", make)]:
            with self.subTest(missing=missing):
                spec = defaults()
                spec["readelf:--dyn-syms"]["output"] += present
                result = self.run_gate(spec, extra_env={"PCMHD_HOOK": "1"})
                self.assertNotEqual(result.returncode, 0, "the other interposer was not required")
                self.assertIn("not present exactly once", result.stdout)

        spec = defaults()
        spec["readelf:--dyn-syms"]["output"] += (
            f"3: 00000020 64 FUNC GLOBAL DEFAULT UND {PCMHD_MAKE}\n" + create
        )
        result = self.run_gate(spec, extra_env={"PCMHD_HOOK": "1"})
        self.assertNotEqual(result.returncode, 0, "an import is not an interposer")
        self.assertIn("defined GLOBAL/DEFAULT", result.stdout)

        spec = defaults()
        spec["readelf:--dyn-syms"]["output"] += (
            make + f"4: 00000030 64 FUNC GLOBAL DEFAULT UND {PCMHD_CREATE}\n"
        )
        result = self.run_gate(spec, extra_env={"PCMHD_HOOK": "1"})
        self.assertNotEqual(result.returncode, 0, "an imported create is not an interposer")
        self.assertIn("defined GLOBAL/DEFAULT", result.stdout)

    def test_early_forbidden_relocation_with_large_tail(self):
        spec = defaults()
        spec["readelf:-rW"]["output"] = LOAD + "\n" + FILLER
        result = self.run_gate(spec)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("dynamic relocation", result.stdout)

    def test_large_metadata_outputs_still_pass(self):
        for key in ["readelf:-h", "readelf:-A", "readelf:--dyn-syms", "readelf:-n", "readelf:-d", "readelf:-l"]:
            with self.subTest(key=key):
                spec = defaults()
                if key == "readelf:-l":
                    spec[key]["output"] *= 70000  # Force sort's early-head SIGPIPE too.
                else:
                    spec[key]["output"] += FILLER
                self.assert_pass(spec)

    def test_producer_failure_is_never_success_even_after_valid_output(self):
        for key in defaults():
            with self.subTest(key=key):
                spec = defaults()
                spec[key]["exit"] = 7
                spec[key]["output"] += FILLER
                result = self.run_gate(spec)
                self.assertNotEqual(result.returncode, 0, "producer failed but gate passed: " + key)
                self.assertEqual(list((self.root / "scratch").iterdir()), [])

    def test_predicates_still_reject_invalid_artifacts(self):
        for key, replacement in [("readelf:-A", "Tag_CPU_arch: v6\n"),
                                 ("readelf:-n", "Build ID: abc\n"),
                                 ("readelf:-d", "Shared library: [unexpected.so]\n"),
                                 ("readelf:--dyn-syms", ""), ("strings", "no placeholder\n"),
                                 ("objdump", "  00: dmb ish\n")]:
            with self.subTest(key=key):
                spec = defaults()
                spec[key]["output"] = replacement
                result = self.run_gate(spec)
                self.assertNotEqual(result.returncode, 0, key)

    def test_parallel_invocations_have_private_scratch_and_remove_it(self):
        self.spec.write_text(json.dumps(defaults()))
        barrier = self.root / "barrier"
        barrier.mkdir()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            runs = [pool.submit(self.run_gate, extra_env={"ELF_TEST_BARRIER": str(barrier),
                    "ELF_TEST_RUN": str(n)}) for n in range(2)]
            for run in runs:
                result = run.result()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for n in range(2):
            seen = json.loads((barrier / f"seen-{n}").read_text())
            self.assertEqual(len(seen), 2, "each invocation must reserve private scratch")
            self.assertEqual(len(set(seen)), 2)
        self.assertEqual(list((self.root / "scratch").iterdir()), [])


class PcmhdSourceContractTests(unittest.TestCase):
    def test_c_hook_uses_public_event_abi_and_balanced_pad_ownership(self):
        hook = (ROOT / "src/pcmhd_sink.c").read_text()
        self.assertIn("gst_pad_probe_info_get_event", hook)
        self.assertIn("gst_event_get_type", hook)
        self.assertIn("gst_event_type_get_name", hook)
        self.assertIn('dlsym(gobj, "g_object_set")', hook)
        self.assertIn('dlsym(gobj, "g_object_get")', hook)
        self.assertIn("pcm_multi_ch != 1", hook)
        self.assertNotIn("g_type_instance_get_class", hook)
        self.assertNotIn("g_object_class_find_property", hook)
        self.assertNotIn("PCMHD_EVENT_TYPE_OFFSET", hook)
        self.assertNotIn("PCMHD_PROBE_INFO_DATA_OFFSET", hook)
        self.assertIn("typedef void (*remove_probe_fn)(GstPad *, unsigned long);", hook)
        self.assertNotIn("s.object_ref(pad)", hook)
        self.assertIn("void nj_pcmhd_disarm(void)", hook)
        self.assertIn("#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif", hook)

    def test_harness_matches_resolved_symbols_and_cannot_fake_glib_macros(self):
        hook = (ROOT / "src/pcmhd_sink.c").read_text()
        harness = (ROOT / "ci/pcmhd-sink-test.c").read_text()
        resolved = set(re.findall(r'(?:SYM\([^,]+,\s*|dlsym\([^,]+,\s*)"([^"]+)"', hook))
        fakes = set(re.findall(r'SYM\("([^"]+)"', harness))
        # Both factory symbols are resolved only by the interposers; the harness supplies its own
        # fake real functions for them, outside the SYM() probe-symbol table.
        self.assertEqual(resolved - {PCMHD_MAKE, PCMHD_CREATE}, fakes)
        self.assertFalse(fakes - resolved)
        self.assertNotIn("g_type_instance_get_class", resolved)
        self.assertNotIn("g_type_instance_get_class", fakes)
        self.assertIn("g_object_get", resolved)
        self.assertIn("replacement_event.type = FAKE_CAPS_EVENT_TYPE", harness)
        self.assertIn("GstEvent event = { FAKE_CAPS_EVENT_TYPE, &caps }", harness)
        self.assertIn("CAPS case '%s': got=%d expected=%d", harness)

    def test_hook_diagnostics_reach_the_event_log_and_are_bounded(self):
        hook = (ROOT / "src/pcmhd_sink.c").read_text()
        carrier = (ROOT / "rust-modules/src/player/es_carrier.rs").read_text()
        feed = (ROOT / "rust-modules/src/ff.rs").read_text()
        self.assertIn("extern void nj_pcmhd_hooks_log(const char *message);", hook)
        self.assertIn("nj_pcmhd_hooks_log(line);", hook)
        self.assertIn('HOOK %s factory=%s armed=%d session=%llu', hook)
        self.assertIn("PCMHD_HOOK_LOG_SLOTS", hook)
        self.assertIn("log_factory_call_locked", hook)
        self.assertIn('pub unsafe extern "C" fn nj_pcmhd_hooks_log', carrier)
        self.assertIn("crate::player::log(&format!(\"pcmhd-sink: {line}\"));", carrier)
        self.assertIn("pub(crate) fn pcmhd_factory_call_counts", carrier)
        self.assertIn("hook_calls={hook_calls} create_calls={create_calls}", feed)

    def test_factory_create_interposer_forwards_and_installs_the_same_probe(self):
        hook = (ROOT / "src/pcmhd_sink.c").read_text()
        makefile = (ROOT / "Makefile").read_text()
        self.assertIn("GstElement *gst_element_factory_create(GstElementFactory *factory, const char *name)", hook)
        self.assertIn('dlsym(gst, "gst_element_factory_create")', hook)
        self.assertIn('dlsym(gst, "gst_object_get_name")', hook)
        self.assertIn('dlsym(glib, "g_free")', hook)
        self.assertGreaterEqual(hook.count("install_locked(element, session);"), 2)
        self.assertIn("-Wl,--export-dynamic-symbol=gst_element_factory_create", makefile)
        self.assertIn("uint64_t nj_pcmhd_make_calls(void)", hook)
        self.assertIn("uint64_t nj_pcmhd_create_calls(void)", hook)
        self.assertIn("_Atomic uint64_t make_calls_total", hook)
        self.assertIn("_Atomic uint64_t create_calls_total", hook)

    def test_nested_create_from_real_make_is_forwarded_silently(self):
        hook = (ROOT / "src/pcmhd_sink.c").read_text()
        harness = (ROOT / "ci/pcmhd-sink-test.c").read_text()
        self.assertIn("static __thread int in_real_make;", hook)
        self.assertIn("if (in_real_make) {", hook)
        self.assertIn("int nested_ready = resolve_create_locked();", hook)
        self.assertIn("return nested_ready ? create(factory, name) : 0;", hook)
        self.assertIn("int previous = in_real_make;", hook)
        self.assertIn("in_real_make = 1;", hook)
        self.assertIn("in_real_make = previous;", hook)
        self.assertIn("make_calls_create", harness)
        self.assertIn("add_probe_calls", harness)
        self.assertIn('assert(!strstr(hook_log_blob, "HOOK create"));', harness)

    def test_c_sources_do_not_redefine_makefile_compiler_macros(self):
        make_lines = [line for line in (ROOT / "Makefile").read_text().splitlines()
                      if not line.lstrip().startswith("#")]
        passed = set(re.findall(r"(?<![A-Za-z0-9_])-D([A-Za-z_][A-Za-z0-9_]*)", "\n".join(make_lines)))
        sources = list((ROOT / "src").glob("*.c")) + list((ROOT / "ci").glob("*.c"))
        for path in sources:
            source = path.read_text()
            for macro in passed:
                local = re.search(rf"^#define {re.escape(macro)}\b", source, re.M)
                if local:
                    guarded = macro == "_GNU_SOURCE" and "#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif" in source
                    self.assertTrue(guarded, f"{path.relative_to(ROOT)} redefines Makefile macro {macro}")

    def test_build_and_demux_fail_closed_without_accepted_caps_or_supported_timing(self):
        makefile = (ROOT / "Makefile").read_text()
        self.assertIn("PCMHD_HOOK ?=", makefile)
        self.assertIn("src/pcmhd_sink.c,$(wildcard src/*.c)", makefile)
        self.assertNotIn("$(if $(filter 1,$(PCMHD_HOOK)),-Wl,--export-dynamic-symbol=gst_element_factory_make)", makefile)
        self.assertIn("ifneq ($(filter 1,$(PCMHD_HOOK)),)", makefile)
        self.assertIn("SMP_INTERPOSER_LDFLAG += -Wl,--export-dynamic-symbol=gst_element_factory_make", makefile)
        self.assertIn("SMP_INTERPOSER_LDFLAG += -Wl,--export-dynamic-symbol=gst_element_factory_create", makefile)
        carrier = (ROOT / "rust-modules/src/player/es_carrier.rs").read_text()
        feed = (ROOT / "rust-modules/src/ff.rs").read_text()
        self.assertIn("timing.supported()", carrier)
        self.assertIn("pub(crate) fn finish", carrier)
        self.assertIn("pcmhd_wait_ready()", feed)
        self.assertIn("dts_core_timing(&core)", feed)
        self.assertIn("rate != 48_000", feed)


class PcmhdLdflagExpansionTests(unittest.TestCase):
    """Statically expand the Makefile's export guard — no `make`, no build, no toolchain.

    Regression for CI ARM run 46157912726: the old line was

        SMP_INTERPOSER_LDFLAG += $(if $(filter 1,$(PCMHD_HOOK)),-Wl,--export-dynamic-symbol=gst_element_factory_make)

    make splits an `if`'s arguments on every *top-level* comma, so the then-branch
    was only `-Wl` and `--export-dynamic-symbol=...` was parsed as the else-branch;
    the linker driver then rejected the bare `-Wl`. The flags now live in a real
    `ifneq` block, where their commas are literal. These tests parse that block out
    of the Makefile and evaluate its two outcomes exactly.
    """

    FLAGS = ["-Wl,--export-dynamic-symbol=gst_element_factory_make",
             "-Wl,--export-dynamic-symbol=gst_element_factory_create"]

    @staticmethod
    def _split_args(text):
        """Split on commas outside $(...) — the nesting make's own parser honours."""
        parts, current, depth = [], [], 0
        for ch in text:
            if ch == ",":
                if depth == 0:
                    parts.append("".join(current))
                    current = []
                    continue
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            current.append(ch)
        parts.append("".join(current))
        return parts

    @classmethod
    def _expand(cls, text, variables):
        text = re.sub(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)",
                      lambda m: variables.get(m.group(1), ""), text)
        return re.sub(r"\$\(filter\s+([^,)]+),([^)]*)\)",
                      lambda m: " ".join(w for w in m.group(2).split() if w == m.group(1)), text)

    def _guard_and_flags(self):
        makefile = (ROOT / "Makefile").read_text()
        match = re.search(
            r"^(?P<guard>if(?:eq|neq)\s+\(.*\))\n"
            r"(?P<flags>(?:^SMP_INTERPOSER_LDFLAG\s*\+=\s*[^\n]*\n)+)"
            r"^endif\b",
            makefile, re.M,
        )
        self.assertIsNotNone(match, "gst factory exports must sit in an ifeq/ifneq block")
        directive, args = match.group("guard").split(None, 1)
        lhs, rhs = self._split_args(args.strip()[1:-1])
        flags = [line.split("+=", 1)[1].strip() for line in match.group("flags").splitlines()]
        return directive, lhs.strip(), rhs.strip(), flags

    def _expanded_flags(self, pcmhd_hook):
        directive, lhs, rhs, flags = self._guard_and_flags()
        self.assertEqual(flags, self.FLAGS, "the guarded block must append exactly the two linker flags")
        variables = {"PCMHD_HOOK": pcmhd_hook}
        left, right = self._expand(lhs, variables), self._expand(rhs, variables)
        taken = (left == right) if directive == "ifeq" else (left != right)
        return flags if taken else []

    def test_hook_one_expands_to_the_exact_flags(self):
        self.assertEqual(self._expanded_flags("1"), self.FLAGS)
        self.assertEqual(self._expanded_flags("1 "), self.FLAGS)

    def test_hook_zero_or_empty_expands_to_nothing(self):
        self.assertEqual(self._expanded_flags("0"), [])
        self.assertEqual(self._expanded_flags(""), [])

    def test_old_if_form_split_at_the_comma(self):
        # Show the bug this fixes: make would read `then` = `-Wl`, and treat the
        # rest as the else-branch, so gcc saw a bare `-Wl` and failed.
        old = "$(if $(filter 1,$(PCMHD_HOOK)),-Wl,--export-dynamic-symbol=gst_element_factory_make)"
        condition, then_branch, else_branch = self._split_args(old[len("$(if "):-1])
        self.assertEqual(condition.strip(), "$(filter 1,$(PCMHD_HOOK))")
        self.assertEqual(then_branch.strip(), "-Wl")
        self.assertEqual(else_branch.strip(), "--export-dynamic-symbol=gst_element_factory_make")
        self.assertNotIn(old, (ROOT / "Makefile").read_text())


class MakeCheckContractTests(unittest.TestCase):
    def test_host_check_runs_elf_gate_regressions(self):
        # `make check` itself is just `tools/check-lock.py`'s machine-wide queue wrapper
        # around `check-unlocked`, which fans out to the `check-python` recipe this asserts on;
        # `make check` still runs it, just serialized.
        lines = (ROOT / "Makefile").read_text().splitlines()

        def recipe_of(target):
            start = next(i for i, line in enumerate(lines) if line.startswith(target + ":"))
            recipe = []
            for line in lines[start + 1:]:
                if line and not line.startswith(("\t", "#")):
                    break
                recipe.append(line)
            return recipe

        # `check-unlocked` runs the `check-cargo` and `check-python` branches side by side; the
        # Python-only gates (this one) live in the second.
        unlocked = "\n".join(recipe_of("check-unlocked"))
        self.assertIn("check-cargo", unlocked)
        self.assertIn("check-python", unlocked)
        self.assertIn("\tpython3 ci/test_check_elf.py", recipe_of("check-python"))


if __name__ == "__main__":
    unittest.main()
