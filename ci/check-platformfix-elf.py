#!/usr/bin/env python3
"""Gate the ARM libplatformfix.so artifact without compiling it.

The host mechanism test proves preload/PLT behaviour on the host architecture. This proves
different facts about the shipped ARM object: ELF32/ARMv7 soft-float identity, the one exact
mangled GLOBAL/DEFAULT C++ definition (a `@version` suffix is tolerated), a deliberately small
system-library dependency set, and no C++ runtime symbol version above the documented webOS
4.4.2 floor. It does not prove the TV's real symbol or playback.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

SYMBOL = "_ZN3smp4util37isPlatformSupportSoftwareAudioDecoderENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE"
CANDIDATE = "isPlatformSupportSoftwareAudioDecoder"
_VERSION_INDEX = re.compile(r"^\(\d+\)$")
REQUIRED_NEEDED = {"libc.so.6"}
ALLOWED_NEEDED = REQUIRED_NEEDED | {"libdl.so.2", "libgcc_s.so.1", "libm.so.6", "libstdc++.so.6"}
# The documented C++ ABI floor is GLIBCXX_3.4.21. The exact cxx11 mangled name above already
# rejects the old std::string ABI; this ceiling rejects a newer libstdc++ requirement that
# fwcompat's name-only firmware inventory cannot see. CXXABI/GCC requirements are collected and
# printed, but this lane has no primary-source ceiling for them and does not invent one.
VERSION_CEILINGS = {"GLIBCXX": (3, 4, 21)}


class GateError(ValueError):
    pass


def query(readelf: str, binary: Path, option: str | list[str]) -> str:
    options = [option] if isinstance(option, str) else option
    result = subprocess.run([readelf, *options, str(binary)], check=True, capture_output=True,
                            text=True)
    return result.stdout


def dynsym_fields(line: str) -> list[str]:
    """A .dynsym row's fields; readelf may append a separate version index like `(2)`."""
    fields = line.split()
    if fields and _VERSION_INDEX.fullmatch(fields[-1]):
        fields = fields[:-1]
    return fields


def candidate_rows(symbols: str) -> str:
    return "\n".join(line for line in symbols.splitlines() if CANDIDATE in line) or "(none)"


def version_key(name: str) -> tuple[int, ...] | None:
    match = re.fullmatch(r"(GLIBCXX|CXXABI|GCC)_([0-9]+(?:\.[0-9]+)+)", name)
    if not match:
        return None
    parts = [int(part) for part in match.group(2).split(".")]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def check(binary: Path, readelf: str) -> None:
    if not binary.is_file():
        raise GateError(f"{binary} is missing; the debug build/package must carry the interposer")

    header = query(readelf, binary, "-h")
    for pattern, message in (
        (r"Class:\s+ELF32", "not ELF32"),
        (r"Type:\s+DYN", "not a shared object (ET_DYN)"),
        (r"Machine:\s+ARM", "not ARM"),
        (r"Flags:.*soft-float", "not the soft-float ABI"),
    ):
        if not re.search(pattern, header):
            raise GateError(message)
    attributes = query(readelf, binary, "-A")
    if not re.search(r"Tag_CPU_arch:\s+v7", attributes):
        raise GateError("not ARMv7 (Tag_CPU_arch)")

    # -W keeps long C++ names on one row; read from the right edge so extra/renamed columns do not
    # shift Type/Bind/Vis/Ndx. Only a `@...` version suffix may follow the exact mangled name.
    symbols = query(readelf, binary, ["-W", "--dyn-syms"])
    rows = []
    for line in symbols.splitlines():
        fields = dynsym_fields(line)
        if len(fields) >= 5 and fields[-1].split("@", 1)[0] == SYMBOL:
            rows.append((fields, line))
    if len(rows) != 1:
        raise GateError(
            f"exact interposer symbol must occur once in .dynsym, found {len(rows)}\n"
            f"  candidate rows: {candidate_rows(symbols)}"
        )
    fields, line = rows[0]
    if (fields[-5] != "FUNC" or fields[-4] != "GLOBAL" or fields[-3] != "DEFAULT"
            or fields[-2] == "UND"):
        raise GateError(
            "interposer symbol is not a defined GLOBAL/DEFAULT FUNC\n"
            f"  row read: {line}"
        )

    dynamic = query(readelf, binary, "-d")
    needed = set(re.findall(r"\(NEEDED\).*?Shared library:\s+\[([^\]]+)\]", dynamic))
    missing = REQUIRED_NEEDED - needed
    if missing:
        raise GateError("missing required DT_NEEDED: " + ", ".join(sorted(missing)))
    unexpected = needed - ALLOWED_NEEDED
    if unexpected:
        raise GateError("unexpected DT_NEEDED (the interposer must resolve LG symbols with RTLD_NEXT): "
                        + ", ".join(sorted(unexpected)))

    versions_text = query(readelf, binary, "--version-info")
    all_versions = sorted(set(re.findall(
        r"\b[A-Z][A-Z0-9]+_[0-9]+(?:\.[0-9]+)+\b", versions_text
    )))
    if not all_versions:
        raise GateError("no symbol version requirements found; version inspection is not working")
    versions = [name for name in all_versions if version_key(name) is not None]
    if versions and "libstdc++.so.6" not in needed:
        raise GateError("C++ runtime versions require libstdc++.so.6 in DT_NEEDED")
    too_new = []
    for name in versions:
        key = version_key(name)
        family = name.split("_", 1)[0]
        if key is not None and family in VERSION_CEILINGS and key > VERSION_CEILINGS[family]:
            too_new.append(name)
    if too_new:
        raise GateError("C++ runtime versions exceed the supported floor: " + ", ".join(too_new))

    print("platformfix ELF: ELF32 / ARMv7 / soft-float / ET_DYN")
    print("platformfix symbol: exact mangled GLOBAL/DEFAULT FUNC")
    print("platformfix DT_NEEDED: " + ", ".join(sorted(needed)))
    print("platformfix C++ versions: " + (", ".join(versions) if versions else "none required"))


def default_readelf() -> str:
    if os.environ.get("READELF"):
        return os.environ["READELF"]
    found = shutil.which("readelf") or shutil.which("llvm-readelf")
    if found:
        return found
    sdk = Path(os.environ.get("WEBOS_SDK", Path.home() / "webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot"))
    candidate = sdk / "bin/arm-webos-linux-gnueabi-readelf"
    if candidate.is_file():
        return str(candidate)
    raise GateError("no readelf found; install binutils or set READELF/WEBOS_SDK")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=Path("pkg/libplatformfix.so"))
    parser.add_argument("--readelf", help="readelf executable (default: READELF, PATH, then the NDK)")
    args = parser.parse_args()
    try:
        check(args.binary, args.readelf or default_readelf())
    except (GateError, OSError, subprocess.CalledProcessError) as error:
        print(f"platformfix-elf: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
