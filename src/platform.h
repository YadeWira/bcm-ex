/*

BCM - A BWT-based file compressor

Copyright (C) 2008-2021 Ilya Muravyov

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

/*

Portability layer for bcm-ex.

Modified work: based on BCM 1.65 by Ilya Muravyov
(https://github.com/FS-make-simple/bcm), changes Copyright (C) 2026 Yade Wira
Bravo. This file is new in bcm-ex and is distributed under the same
Apache License, Version 2.0, as the rest of the work.

This header MUST be included before any system header: it asks for 64-bit
file offsets on 32-bit glibc, which only works if <features.h> has not been
included yet.

What it hides:

  bcm_fseek / bcm_ftell   seek and tell a file past 2 GB on every target
                          (fseeko64/ftello64 on Windows, fseeko/ftello with
                          _FILE_OFFSET_BITS=64 everywhere else)
  bcm_stat                file size and times (struct stat on POSIX,
                          struct _stati64 on Windows)
  bcm_utime               restore the input file's timestamps on the output

*/

#ifndef BCM_PLATFORM_H
#define BCM_PLATFORM_H

/* 32-bit glibc: off_t must be 64 bits. Defined before every system header. */
#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#  define _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES 1
#  define _CRT_DISABLE_PERFCRIT_LOCKS 1
#endif

#if defined(_WIN32)
/* Windows 7 SP1 or newer, and never a dependency on a newer SDK. */
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>

#if defined(_WIN32)

#  include <io.h>
#  include <fcntl.h>
#  include <sys/utime.h>

#  define bcm_fseek  _fseeki64
#  define bcm_ftell  _ftelli64
#  define bcm_stat   _stati64
#  define bcm_utime  _utime

typedef struct _stati64  bcm_stat_t;
typedef struct _utimbuf  bcm_utimbuf_t;

#  define BCM_BINARY_MODE "b"

/* Windows starts the standard streams in text mode: reading translates CRLF
   to LF and stops at 0x1A, and writing turns every LF into CRLF. A compressed
   stream is arbitrary binary data, so both must be switched before use. */
static inline void bcm_set_binary_stdout(void)
{
    _setmode(_fileno(stdout), _O_BINARY);
}

static inline void bcm_set_binary_stdin(void)
{
    _setmode(_fileno(stdin), _O_BINARY);
}

#else

#  include <unistd.h>
#  include <utime.h>

#  define bcm_fseek  fseeko
#  define bcm_ftell  ftello
#  define bcm_stat   stat
#  define bcm_utime  utime

typedef struct stat     bcm_stat_t;
typedef struct utimbuf  bcm_utimbuf_t;

#  define BCM_BINARY_MODE "b"

/* Nothing to do: POSIX streams are always binary. */
static inline void bcm_set_binary_stdout(void) {}
static inline void bcm_set_binary_stdin(void) {}

#endif

#endif /* BCM_PLATFORM_H */
