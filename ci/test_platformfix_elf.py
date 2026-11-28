#!/usr/bin/env python3
"""Artifact-gate regression tests with canned readelf output; no ELF, NDK or compiler."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("platformfix_elf", Path(__file__).with_name("check-platformfix-elf.py"))
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


def outputs():
    symbol = gate.SYMBOL
    return {
        "-h": "Class: ELF32\nType: DYN\nMachine: ARM\nFlags: soft-float\n",
        "-A": "Tag_CPU_arch: v7\n",
        "--dyn-syms": f"1: 00000000 64 FUNC GLOBAL DEFAULT 1 {symbol}\n",
        "-d": ("0x1 (NEEDED) Shared library: [libstdc++.so.6]\n"
               "0x2 (NEEDED) Shared library: [libgcc_s.so.1]\n"
               "0x3 (NEEDED) Shared library: [libc.so.6]\n"
               "0x4 (NEEDED) Shared library: [libm.so.6]\n"),
        "--version-info": ("Version needs section (4 versions)\n"
                           "  GLIBCXX_3.4\n  GLIBCXX_3.4.21\n  CXXABI_1.3\n  GCC_3.0\n"),
    }


class ArtifactGate(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="platformfix-elf-tests-")
        self.addCleanup(self.temp.cleanup)
        self.binary = Path(self.temp.name) / "libplatformfix.so"
        self.binary.write_bytes(b"synthetic; readelf is mocked")

    def run_gate(self, spec=None, query_error=None):
        values = outputs() if spec is None else spec

        def fake_query(_readelf, _binary, option):
            if query_error is not None and option == query_error:
                raise subprocess.CalledProcessError(7, ["readelf", option])
            return values[option]

        with patch.object(gate, "query", side_effect=fake_query):
            return gate.check(self.binary, "readelf")

    def test_valid_arm_dso(self):
        self.run_gate()

    def test_missing_artifact_is_a_failure_not_a_skip(self):
        self.binary.unlink()
        with self.assertRaisesRegex(gate.GateError, "missing"):
            self.run_gate()

    def test_wrong_elf_identity_is_rejected(self):
        for option, old, new in (("-h", "ELF32", "ELF64"), ("-h", "Type: DYN", "Type: EXEC"),
                                 ("-h", "Machine: ARM", "Machine: X86_64"),
                                 ("-h", "soft-float", "hard-float"), ("-A", "v7", "v6")):
            with self.subTest(option=option, new=new):
                spec = outputs()
                spec[option] = spec[option].replace(old, new)
                with self.assertRaises(gate.GateError):
                    self.run_gate(spec)

    def test_exact_symbol_must_be_one_defined_global_default_func(self):
        good = outputs()["--dyn-syms"].strip()
        cases = [
            "",
            good.replace(gate.SYMBOL, gate.SYMBOL + "Extra"),
            good + "\n" + good,
            good.replace("GLOBAL DEFAULT 1", "GLOBAL DEFAULT UND"),
            good.replace("GLOBAL", "WEAK"),
            good.replace("DEFAULT", "HIDDEN"),
            good.replace("FUNC", "OBJECT"),
            good.replace(gate.SYMBOL, gate.SYMBOL + "@GLIBCXX_3.4.21"),
        ]
        for rows in cases:
            with self.subTest(rows=rows):
                spec = outputs()
                spec["--dyn-syms"] = rows + "\n"
                with self.assertRaisesRegex(gate.GateError, "symbol"):
                    self.run_gate(spec)

    def test_dt_needed_cannot_link_lg_or_drop_libc(self):
        for needed, message in (("0x1 (NEEDED) Shared library: [libstdc++.so.6]\n", "libc.so.6"),
                                ("0x1 (NEEDED) Shared library: [libstdc++.so.6]\n"
                                 "0x2 (NEEDED) Shared library: [libc.so.6]\n"
                                 "0x3 (NEEDED) Shared library: [libplayerAPIs.so.1]\n",
                                 "unexpected DT_NEEDED"),
                                ("0x1 (NEEDED) Shared library: [libc.so.6]\n",
                                 "C++ runtime versions require libstdc++.so.6")):
            with self.subTest(message=message):
                spec = outputs()
                spec["-d"] = needed
                with self.assertRaisesRegex(gate.GateError, re.escape(message)):
                    self.run_gate(spec)

    def test_no_cxx_runtime_dependency_is_valid_when_no_cxx_versions_are_required(self):
        spec = outputs()
        spec["-d"] = "0x1 (NEEDED) Shared library: [libc.so.6]\n"
        spec["--version-info"] = "Version needs section (1 versions)\n  GLIBC_2.4\n"
        self.run_gate(spec)

    def test_cxx_runtime_version_ceiling_and_positive_control(self):
        for version, valid in (("GLIBCXX_3.4.20", True), ("GLIBCXX_3.4.21", True),
                               ("GLIBCXX_3.4.22", False), ("CXXABI_1.3", True),
                               ("CXXABI_1.3.1", True), ("GCC_3.0", True), ("GCC_3.1", True)):
            with self.subTest(version=version):
                spec = outputs()
                spec["--version-info"] = version + "\n"
                if valid:
                    self.run_gate(spec)
                else:
                    with self.assertRaisesRegex(gate.GateError, "exceed"):
                        self.run_gate(spec)
        spec = outputs()
        spec["--version-info"] = "no version rows\n"
        with self.assertRaisesRegex(gate.GateError, "not working"):
            self.run_gate(spec)

    def test_readelf_failure_is_never_success(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_gate(query_error="--dyn-syms")


if __name__ == "__main__":
    unittest.main()
