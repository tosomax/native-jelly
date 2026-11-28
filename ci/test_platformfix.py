#!/usr/bin/env python3
"""Static SWDEC contracts: layer, target guards and CI wiring, with no build or device."""
from __future__ import annotations

import configparser
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent.parent


class ModuleLayer(unittest.TestCase):
    def test_platformfix_belongs_to_base(self):
        layers = configparser.ConfigParser()
        layers.read(ROOT / "ci/module-layers.ini")
        self.assertIn("platformfix", layers["base"]["members"].split())


class PlatformfixTarget(unittest.TestCase):
    def test_default_all_reads_the_debug_dso_manifest_after_it_is_defined(self):
        # GNU Make expands prerequisites immediately. With the old ordering, `all` read an
        # undefined PLATFORMFIX_FILES and CI built nativejelly without libplatformfix.so.
        makefile = (ROOT / "Makefile").read_text()
        all_at = makefile.index("\nall:")
        for definition in ("PLATFORMFIX_ALLOWED =", "PLATFORMFIX_FILES   =",
                           "$(PLATFORMFIX_SO):"):
            with self.subTest(definition=definition):
                self.assertLess(makefile.index(definition), all_at)
        all_rule = makefile[all_at + 1:].splitlines()[0]
        self.assertEqual(
            all_rule,
            "all: pkg/nativejelly pkg/nativejelly-storage $(PLATFORMFIX_FILES)",
        )
        self.assertIn(
            "PLATFORMFIX_ALLOWED = $(if $(filter debug,$(FLAVOR)),$(if $(RELEASE),,yes),)",
            makefile[:all_at],
        )
        self.assertIn(
            "PLATFORMFIX_FILES   = $(if $(filter yes,$(PLATFORMFIX_ALLOWED)),$(PLATFORMFIX_SO),)",
            makefile[:all_at],
        )
        self.assertIn(".DEFAULT_GOAL := all", makefile)

    def test_manual_target_refuses_every_non_debug_or_nonempty_release(self):
        makefile = (ROOT / "Makefile").read_text()
        self.assertIn(
            "PLATFORMFIX_ALLOWED = $(if $(filter debug,$(FLAVOR)),$(if $(RELEASE),,yes),)",
            makefile,
        )
        self.assertIn(".PHONY: platformfix", makefile)
        allowed = makefile.index("ifeq ($(PLATFORMFIX_ALLOWED),yes)")
        refused = makefile.index("else\nplatformfix:", allowed)
        following = makefile.index("\nendif", refused)
        self.assertIn("platformfix: $(PLATFORMFIX_SO)", makefile[allowed:refused])
        self.assertIn("FLAVOR=debug", makefile[refused:following])
        self.assertIn("RELEASE", makefile[refused:following])
        self.assertIn("exit 1", makefile[refused:following])


class RuntimeContracts(unittest.TestCase):
    def test_payload_predicate_includes_the_native_dts_trigger_decision(self):
        engine = (ROOT / "rust-modules/src/player/engine.rs").read_text()
        self.assertIn(
            'dts_native && matches!(audio_declared, "DTS" | "DTSX" | "DTSE") && enabled',
            engine,
        )
        self.assertRegex(
            engine,
            r"let swdec = software_audio_decoder_requested\(\s*audio_declared,\s*dts_native,",
        )

    def test_trigger_decision_is_latched_and_inherited_by_the_child(self):
        rust = (ROOT / "rust-modules/base/src/platformfix.rs").read_text()
        self.assertIn("static DECISION: OnceLock<bool>", rust)
        self.assertIn("enabled_latched(&DECISION", rust)
        self.assertIn("trigger_arms(trigger) || sentinel_active(sentinel)", rust)
        self.assertIn('.env(SENTINEL, SENTINEL_VALUE)', rust)

    def test_rust_and_cpp_use_the_same_exact_sentinel_value(self):
        rust = (ROOT / "rust-modules/base/src/platformfix.rs").read_text()
        cpp = (ROOT / "interposer/platformfix.cpp").read_text()
        host_test = (ROOT / "interposer/hosttest/test.cpp").read_text()
        runner = (ROOT / "interposer/hosttest/run.sh").read_text()
        self.assertIn('pub const SENTINEL_VALUE: &str = "1";', rust)
        self.assertIn("raw == Some(SENTINEL_VALUE)", rust)
        self.assertIn('env_is_exact("NJ_PLATFORMFIX_ACTIVE", "1")', cpp)
        self.assertIn('std::string(armed) == "1"', host_test)
        self.assertIn("for marker in '' 0 1; do", runner)


class DtsWorkflow(unittest.TestCase):
    def test_push_runs_on_the_spike_and_checkout_uses_the_event_commit(self):
        workflow = (ROOT / ".github/workflows/dts-native-build.yml").read_text()
        branches = re.search(r"push:\s*\n\s*branches: \[([^\]]*)\]", workflow)
        self.assertIsNotNone(branches)
        self.assertIn("dts-swdec-spike", branches.group(1))
        checkout = re.search(
            r"uses: actions/checkout@[^\n]+\n\s*with:\n(?:\s*#[^\n]*\n)*\s*ref: ([^\n]+)",
            workflow,
        )
        self.assertIsNotNone(checkout)
        self.assertEqual(checkout.group(1).strip(), "${{ github.sha }}")
        self.assertNotIn("v011-dtsinfo-fixed", workflow)

    def test_host_mechanism_and_dso_gates_are_not_masked(self):
        ci = (ROOT / ".github/workflows/ci.yml").read_text()
        dts = (ROOT / ".github/workflows/dts-native-build.yml").read_text()
        for workflow in (ci, dts):
            with self.subTest(workflow="ci" if workflow is ci else "dts"):
                self.assertIn("interposer/hosttest/run.sh", workflow)
                self.assertIn("ci/check-platformfix-elf.py", workflow)
        self.assertIn("pkg/lib*-plx.so.* pkg/libplatformfix.so", ci)
        host_test = (ROOT / "interposer/hosttest/run.sh").read_text()
        self.assertNotIn("|| true", host_test)
        self.assertIn("set -eu", host_test)


if __name__ == "__main__":
    unittest.main()
