#!/bin/sh
# NZL Studio — host-side syntax gate for the shared-memory client TU.
#
# Compiles nzl/client/sc_client.cpp against the minimal Win32 shim in
# hostcheck/, so type drift against nzl/include/nzl/na_wire.h and typos in the
# client are caught without Windows. The real client is built by MSVC in
# NZL-BuildAll.ps1; this only proves it still parses and type-checks.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
cxx=${CXX:-g++}

echo "syntax: $cxx sc_client.cpp (Win32 shim: nzl/client/hostcheck)"
"$cxx" -std=c++20 -fsyntax-only -Wall -Wextra \
    -I "$root/nzl/client/hostcheck" \
    -I "$root/nzl/driver/hostcheck" \
    -I "$root/nzl/include" \
    -I "$root/nzl/client" \
    "$root/nzl/client/sc_client.cpp"
echo "CLIENT_SYNTAX_OK"
