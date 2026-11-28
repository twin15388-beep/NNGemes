#!/usr/bin/env python3
"""NZL Studio - static checker for the PowerShell scripts in tools/.

Why: PowerShell variable names are CASE-INSENSITIVE, and a script parameter is
just a variable. `param([switch]$Build)` followed by `$build = Join-Path ...`
assigns a string to a SwitchParameter and dies at runtime with

    Не удается преобразовать значение "System.String" в тип
    "System.Management.Automation.SwitchParameter"

on whatever line the assignment happens to sit. That exact bug killed
NZL-Report.ps1 at 2026-11-28 15:21, after the build had already succeeded,
so the report the agent needed never got written. There is no PowerShell in
this sandbox (release assets are blocked), so this checker is the gate.

Checks, per file:
  1. balanced {} () [] outside strings and comments;
  2. no assignment to a name that matches a declared [switch] parameter
     (case-insensitive) - the $Build/$build class. Reassigning a [string]
     parameter is legal PowerShell and is not flagged;
  3. no duplicate parameter names (also case-insensitive).

Usage:  python3 tools/check_ps1.py [files...]     (default: tools/*.ps1)
Exit 0 = clean, 1 = at least one violation.
"""
import glob
import os
import re
import sys


def strip_strings_and_comments(text):
    """Return (code_without_literals, comments_removed_text)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith('<#', i):
            j = text.find('#>', i)
            newlines = text[i:(j if j != -1 else n)].count('\n')
            out.append('\n' * newlines)
            i = (j + 2) if j != -1 else n
            continue
        c = text[i]
        if c == '#':
            j = text.find('\n', i)
            i = j if j != -1 else n
            continue
        if c == "'":
            i += 1
            while i < n:
                if text[i] == "'":
                    if i + 1 < n and text[i + 1] == "'":
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            out.append('""')
            continue
        if c == '"':
            i += 1
            while i < n:
                if text[i] == '`':
                    i += 2
                    continue
                if text[i] == '"':
                    if i + 1 < n and text[i + 1] == '"':
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            out.append('""')
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def find_params(code):
    """(name, declared_type) for each parameter of the leading param(...) block.

    A parameter IS a variable, so reassigning a [string] parameter is legal
    PowerShell and several tools/ scripts do it on purpose. A [switch]
    parameter is the trap: assigning a string to it throws
    ArgumentTransformationMetadataException at runtime.
    """
    m = re.search(r'^\s*param\s*\(', code, re.IGNORECASE)
    if not m:
        return []
    depth = 0
    i = m.end() - 1
    start = i
    while i < len(code):
        if code[i] == '(':
            depth += 1
        elif code[i] == ')':
            depth -= 1
            if depth == 0:
                break
        i += 1
    block = code[start + 1:i]   # inside the outer parentheses only

    params = []
    segment = ''
    depth = 0
    for ch in block:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            params.append(segment)
            segment = ''
            continue
        segment += ch
    params.append(segment)

    found = []
    for segment in params:
        hit = re.search(r'\[\s*([A-Za-z_][A-Za-z0-9_.]*)\s*\]\s*\$([A-Za-z_][A-Za-z0-9_]*)', segment)
        if hit:
            found.append((hit.group(2), hit.group(1).lower()))
        else:
            bare = re.search(r'\$([A-Za-z_][A-Za-z0-9_]*)', segment)
            if bare:
                found.append((bare.group(1), ''))
    return found


def check_file(path):
    problems = []
    with open(path, encoding='utf-8-sig') as handle:
        raw = handle.read()
    code = strip_strings_and_comments(raw)

    for open_c, close_c, label in (('{', '}', 'brace'), ('(', ')', 'paren'), ('[', ']', 'bracket')):
        depth = 0
        neg = False
        for ch in code:
            if ch == open_c:
                depth += 1
            elif ch == close_c:
                depth -= 1
                if depth < 0:
                    neg = True
                    break
        if depth != 0 or neg:
            problems.append('%s: unbalanced %s (depth=%d, went negative=%s)'
                            % (path, label, depth, neg))

    params = find_params(code)
    lowered = {}
    switches = {}
    for name, kind in params:
        if name.lower() in lowered:
            problems.append('%s: duplicate parameter $%s (parameters are case-insensitive)'
                            % (path, name))
        lowered[name.lower()] = name
        if kind == 'switch':
            switches[name.lower()] = name

    # Assignments: "$name =" (also +=, -=, *=), skipping -eq/-ne/etc.
    # Only [switch] parameters are flagged: they cannot hold a value, so any
    # assignment is the $Build/$build class of runtime failure.
    for match in re.finditer(r'\$([A-Za-z_][A-Za-z0-9_]*)\s*(\+|-|\*|/)?=(?!=)', code):
        name = match.group(1)
        if name.lower() in switches:
            line = code[:match.start()].count('\n') + 1
            problems.append('%s:%d: assigns to $%s, which is the [switch] parameter $%s '
                            '(case-insensitive collision, throws at runtime)'
                            % (path, line, name, switches[name.lower()]))
    return problems


BOM = b"\xef\xbb\xbf"
SMART = "\u2018\u2019\u201c\u201d"


def encoding_problems(path):
    """PowerShell 5.1 decodes a BOM-less .ps1 with the system ANSI codepage.
    On a Russian Windows that is cp1251, where the UTF-8 bytes of capital
    'В' 'Г' 'Д' become U+2018..U+201D - and PowerShell accepts those as string
    delimiters. A double-quoted string containing capital 'Д' therefore ends
    in the middle, and the parser reports an unexpected '}' lines later.
    tools/NZL-Overlay.ps1 shipped that way on 2026-11-28 and would not start.
    The fix is a UTF-8 BOM, which makes every PowerShell read the file as
    UTF-8; this check makes sure nobody drops it again."""
    raw = open(path, "rb").read()
    problems = []
    if not raw.startswith(BOM):
        phantom = sum(1 for c in raw.decode("cp1251", "replace") if c in SMART)
        problems.append(
            "missing UTF-8 BOM - PowerShell 5.1 will decode this as the ANSI "
            "codepage and %d smart quotes appear inside strings" % phantom)
    # Second hazard, independent of the BOM: PowerShell accepts curly quotes
    # as string delimiters too. A real U+2018..U+201D in the source (pasted
    # from a document) terminates a string early no matter how the file is
    # decoded, so it must never appear in code or in prose here.
    text = raw.decode("utf-8", "replace")
    for number, line in enumerate(text.splitlines(), 1):
        if any(c in SMART for c in line):
            problems.append(
                "%s:%d: contains a curly quote (U+2018..U+201D); PowerShell "
                "treats those as string delimiters" % (path, number))
    return problems


def bat_line_endings(path):
    """cmd.exe splits a batch file on \\n and strips ONE \\r. A file with
    \\r\\r\\n therefore leaves a stray \\r inside every line - 'set "ROOT=...\\r"'
    becomes an unusable path. NZL.bat was written that way on 2026-11-28 and
    would have failed on every command."""
    raw = open(path, "rb").read()
    problems = []
    if b"\r\r" in raw:
        problems.append("%s: doubled CR before LF (%d lines) - cmd.exe keeps a "
                        "trailing \\r inside each line" % (path, raw.count(b"\r\r\n")))
    bare = raw.count(b"\n") - raw.count(b"\r\n")
    if bare:
        problems.append("%s: %d LF-only line endings - .bat must be CRLF "
                        "(labels and goto break otherwise)" % (path, bare))
    return problems


def main(argv):
    targets = argv[1:]
    if not targets:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        targets = sorted(glob.glob(os.path.join(root, 'tools', '*.ps1')))
        targets += sorted(glob.glob(os.path.join(root, '*.bat')))
        targets += sorted(glob.glob(os.path.join(root, 'nzl', 'driver', '*.bat')))
    failures = 0
    for path in targets:
        if path.lower().endswith(".bat"):
            problems = bat_line_endings(path)
        else:
            problems = encoding_problems(path) + check_file(path)
        if problems:
            failures += 1
            for problem in problems:
                print('[PS1] ' + problem)
        else:
            print('ok: %s' % os.path.basename(path))
    if failures:
        print('%d file(s) with problems' % failures)
        return 1
    print('PS1_CHECKS_OK')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
