#!/bin/sh
# NZL Studio - type gate for the shared-memory wire protocol.
#
# Why this exists: under MSVC (LLP64) NA_U32 is `unsigned long`, while
# uint32_t is `unsigned int`. Same size, DIFFERENT TYPES, so `uint32_t*` does
# not convert to `NA_U32*` - the owner's build died on exactly that
# (test_protocol.cpp C2664, 2026-11-28 15:18). On Linux (LP64) NA_U32 is
# `unsigned int` == uint32_t, so g++ accepts the same code without a peep.
# There is no portable compiler flag for this; the grep is the gate.
#
# Rule: files that implement or exercise the wire protocol use NA_U8/NA_U32/
# NA_U64 only. Application code that merely passes values by copy may use
# <cstdint> names - it is not in this list.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
files="$root/nzl/include/nzl/na_wire.h \
$root/nzl/include/nzl/na_validate.h \
$root/nzl/include/nzl/na_scan.h \
$root/nzl/include/nzl/na_pebwalk.h \
$root/nzl/tests/test_protocol.cpp \
$root/nzl/driver/sc_drv.c \
$root/nzl/driver/sc.h"

status=0
for f in $files; do
    hits=$(awk '
        /^[[:space:]]*\/\*/ { inblock = 1 }
        inblock { if ($0 ~ /\*\//) inblock = 0; next }
        { sub(/\/\/.*/, "") }
        /(^|[^A-Za-z0-9_])(u?int(8|16|32|64)_t)([^A-Za-z0-9_]|$)/ { print FILENAME ":" FNR ": " $0 }
    ' "$f")
    if [ -n "$hits" ]; then
        echo "[TYPE GATE] $f uses <stdint> names on the wire protocol:"
        echo "$hits"
        status=1
    fi
done
if [ "$status" -ne 0 ]; then
    echo "use NA_U8 / NA_U32 / NA_U64 instead"
    exit 1
fi
echo "WIRE_TYPES_OK"
