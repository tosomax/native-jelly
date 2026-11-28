#!/usr/bin/env python3
"""The ARM wrapper changes language only for the explicit C++ opt-in.

Pure selector tests: no SDK, compiler, link evidence or subprocess is needed.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("arm_cc", Path(__file__).with_name("arm-cc.py"))
arm_cc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(arm_cc)


class DriverSelection(unittest.TestCase):
    def test_only_exact_one_selects_the_cxx_driver(self):
        for value in (None, "", "0", "false", "true", "01", " 1", "1 "):
            with self.subTest(value=value):
                expected = ("arm-webos-linux-gnueabi-g++" if value == "1"
                            else "arm-webos-linux-gnueabi-gcc")
                self.assertEqual(arm_cc.driver_for(value), expected)


if __name__ == "__main__":
    unittest.main()
