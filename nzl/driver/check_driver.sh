#!/bin/sh
# NZL Studio — host-side syntax gate for the kernel driver translation unit.
#
# Compiles nzl/driver/sc_drv.c with the host compiler against the minimal WDK
# shim in hostcheck/. It catches typos, wrong argument counts, undeclared
# identifiers, type mismatches and layout-assert failures in code shared with
# user mode. It does NOT replace build_driver.bat: only MSVC + the real WDK
# headers produce a loadable driver.sys, and only a live probe proves runtime
# behaviour.
#
# Usage:  sh nzl/driver/check_driver.sh          (exit 0 = clean)
#         CC=clang sh nzl/driver/check_driver.sh
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
cc=${CC:-gcc}

echo "syntax: $cc sc_drv.c (WDK shim: nzl/driver/hostcheck)"
"$cc" -std=c11 -Wall -Wextra -Werror -Wno-multichar -fsyntax-only \
    -I "$root/nzl/driver/hostcheck" \
    -I "$root/nzl/include" \
    -I "$root/nzl/driver" \
    "$root/nzl/driver/sc_drv.c"
echo "DRIVER_SYNTAX_OK"
