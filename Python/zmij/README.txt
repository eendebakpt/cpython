Żmij — floating-point-to-string conversion, C port
https://github.com/vitaut/zmij
Version: v1.2-19-ga9d6363 (commit a9d6363, 2026-10-18)
License: MIT or BSL-1.0 (see LICENSE)

Vendored: zmij.c, zmij-c.h. Compiled as part of Python/floatconv.c.
Local change: ZMIJ_ASM uses __asm__ instead of asm (CPython builds with
-std=c11, where asm is not a keyword); proposed upstream.
