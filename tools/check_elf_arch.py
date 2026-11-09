#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check that a file is a 64-bit AArch64 ELF shared object.

Used by tools/fetch_onnxruntime_android.sh, because `file` is missing from many
minimal containers and a wrong-architecture .so fails much later, inside the
linker, with an error that does not mention the architecture.
"""
from __future__ import annotations

import struct
import sys

EM_AARCH64 = 0xB7
ET_DYN = 3


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_elf_arch.py <file.so>", file=sys.stderr)
        return 2
    path = sys.argv[1]
    with open(path, "rb") as fh:
        ident = fh.read(20)
    if len(ident) < 20 or ident[:4] != b"\x7fELF":
        print(f"not an ELF file: {path}", file=sys.stderr)
        return 1
    if ident[4] != 2:
        print(f"not a 64-bit ELF: {path}", file=sys.stderr)
        return 1
    endian = "<" if ident[5] == 1 else ">"
    e_type, e_machine = struct.unpack(endian + "HH", ident[16:20])
    if e_machine != EM_AARCH64:
        print(f"not an AArch64 library (e_machine={e_machine:#x})", file=sys.stderr)
        return 1
    if e_type != ET_DYN:
        print(f"not a shared object (e_type={e_type})", file=sys.stderr)
        return 1
    print("  ELF check: AArch64 64-bit shared object")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
