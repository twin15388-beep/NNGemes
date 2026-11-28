#!/usr/bin/env python3
"""Catch duplicate struct/class members in the Windows-only sources.

The MSVC build is the only real compile gate, and it runs on the owner's
machine, so a duplicate member costs him a whole round trip (C2086). These
files cannot be compiled in the sandbox, so at least this class of mistake is
checked here: every struct body is flattened and member names must be unique.
"""
import re
import sys

TARGETS = [
    "nzl/src/overlay_windows.cpp",
    "nzl/src/probe_windows.cpp",
    "nzl/src/main_windows.cpp",
    "nzl/src/driver_memory_windows.cpp",
]


def struct_body(src, start):
    depth = 1
    i = start
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[start:i - 1]


def check(path):
    problems = []
    src = open(path, encoding="utf-8").read()
    for match in re.finditer(r"\b(?:struct|class)\s+(\w+)\s*\{", src):
        name = match.group(1)
        body = struct_body(src, match.end())
        flat = re.sub(r"\{[^{}]*\}", "", body)
        seen = set()
        for decl in flat.split(";"):
            decl = decl.strip()
            if "=" in decl:
                decl = decl.split("=")[0].strip()
            if "(" in decl or not decl or decl.startswith("//"):
                continue
            parts = decl.split()
            if len(parts) < 2:
                continue
            ident = parts[-1].replace("*", "").replace("&", "").strip()
            if not re.fullmatch(r"\w+", ident or ""):
                continue
            if ident in seen:
                problems.append(f"{path}: struct {name}: duplicate member '{ident}'")
            seen.add(ident)
    return problems


def main():
    problems = []
    for path in TARGETS:
        try:
            problems.extend(check(path))
        except FileNotFoundError:
            problems.append(f"{path}: missing")
    for problem in problems:
        print("FAIL", problem)
    if problems:
        return 1
    print("STRUCT_MEMBERS_OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
