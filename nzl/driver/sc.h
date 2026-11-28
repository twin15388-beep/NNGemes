#ifndef SC_H
#define SC_H

#include <ntddk.h>

/* Wire protocol (commands, limits, NA_SHM layout, compile-time ABI asserts)
 * lives in exactly one place, shared with the user-mode client. See
 * nzl/include/nzl/na_wire.h for the offset table and the rationale. */
#include "nzl/na_wire.h"

#endif
