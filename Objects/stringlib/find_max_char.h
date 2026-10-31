/* Finding the optimal width of unicode characters in a buffer */

/* find_max_char for one-byte will work for bytes objects as well. */
#if !STRINGLIB_IS_UNICODE && STRINGLIB_SIZEOF_CHAR > 1
# error "find_max_char.h is specific to Unicode"
#endif

#define MAX_CHAR_ASCII 0x7f
#define MAX_CHAR_UCS1  0xff
#define MAX_CHAR_UCS2  0xffff
#define MAX_CHAR_UCS4  0x10ffff

/* Bits that are set in a 64-bit word holding several characters when at
   least one of them is wider than ASCII, UCS1 or UCS2.  Characters are
   always loaded at a multiple of their size, so the masks are the same on
   little and big endian platforms. */
#if STRINGLIB_SIZEOF_CHAR == 1
# define MASK_ASCII 0x8080808080808080ULL
# define MASK_LIMIT MASK_ASCII
# define MAX_CHAR_LIMIT MAX_CHAR_UCS1
#elif STRINGLIB_SIZEOF_CHAR == 2
# define MASK_ASCII 0xFF80FF80FF80FF80ULL
# define MASK_UCS1  0xFF00FF00FF00FF00ULL
# define MASK_LIMIT MASK_UCS1
# define MAX_CHAR_LIMIT MAX_CHAR_UCS2
#elif STRINGLIB_SIZEOF_CHAR == 4
# define MASK_ASCII 0xFFFFFF80FFFFFF80ULL
# define MASK_UCS1  0xFFFFFF00FFFFFF00ULL
# define MASK_UCS2  0xFFFF0000FFFF0000ULL
# define MASK_LIMIT MASK_UCS2
# define MAX_CHAR_LIMIT MAX_CHAR_UCS4
#else
# error Invalid STRINGLIB_SIZEOF_CHAR (must be 1, 2 or 4)
#endif

/* The characters are OR-ed together 16 bytes at a time with the GCC/clang
   vector extensions, where configure found that they compile to native SIMD
   instructions (SSE2 on x86-64, NEON on AArch64), and 8 bytes at a time
   elsewhere.  This does not depend on the auto-vectorizer, which for example
   leaves loops alone that were cold during PGO training.
   This header is included once per character width. */
#ifndef FIND_MAX_CHAR_VECTOR
# ifdef _Py_HAVE_EFFICIENT_BUILTIN_SHUFFLEVECTOR
typedef uint64_t find_max_char_vector_t __attribute__((vector_size(16)));
#  define FIND_MAX_CHAR_VECTOR find_max_char_vector_t
#  define FIND_MAX_CHAR_FOLD(v) ((v)[0] | (v)[1])
# else
#  define FIND_MAX_CHAR_VECTOR uint64_t
#  define FIND_MAX_CHAR_FOLD(v) (v)
# endif
#endif

/* Return an upper bound on the widest character in [begin, end): one of
   MAX_CHAR_ASCII, MAX_CHAR_UCS1, MAX_CHAR_UCS2 or MAX_CHAR_UCS4. */
Py_LOCAL_INLINE(Py_UCS4)
STRINGLIB(find_max_char)(const STRINGLIB_CHAR *begin, const STRINGLIB_CHAR *end)
{
    const unsigned char *p = (const unsigned char *)begin;
    size_t n = (size_t)(end - begin) * STRINGLIB_SIZEOF_CHAR;
    const unsigned char *last = p + n;
    uint64_t bits;

    if (n >= sizeof(FIND_MAX_CHAR_VECTOR)) {
        FIND_MAX_CHAR_VECTOR acc0 = {0}, acc1 = {0}, a, b;
        const size_t block = 8 * sizeof(a);

        /* Two accumulators keep the loop from serializing on one register.
           Only check for an early exit once per block. */
        while (n >= block) {
            for (size_t i = 0; i < block; i += 2 * sizeof(a)) {
                memcpy(&a, p + i, sizeof(a));
                memcpy(&b, p + i + sizeof(a), sizeof(b));
                acc0 |= a;
                acc1 |= b;
            }
            if (FIND_MAX_CHAR_FOLD(acc0 | acc1) & MASK_LIMIT) {
                return MAX_CHAR_LIMIT;
            }
            p += block;
            n -= block;
        }
        while (n >= 2 * sizeof(a)) {
            memcpy(&a, p, sizeof(a));
            memcpy(&b, p + sizeof(a), sizeof(b));
            acc0 |= a;
            acc1 |= b;
            p += 2 * sizeof(a);
            n -= 2 * sizeof(a);
        }
        if (n >= sizeof(a)) {
            memcpy(&a, p, sizeof(a));
            acc0 |= a;
        }
        /* The last vector overlaps with data that was already seen. */
        memcpy(&b, last - sizeof(b), sizeof(b));
        acc1 |= b;
        bits = FIND_MAX_CHAR_FOLD(acc0 | acc1);
    }
    else if (n >= 8) {
        uint64_t a, b;
        memcpy(&a, p, 8);
        memcpy(&b, last - 8, 8);
        bits = a | b;
    }
    else if (n >= 4) {
        uint32_t a, b;
        memcpy(&a, p, 4);
        memcpy(&b, last - 4, 4);
        bits = a | b;
    }
    else if (n >= 2) {
        uint16_t a, b;
        memcpy(&a, p, 2);
        memcpy(&b, last - 2, 2);
        bits = a | b;
    }
    else {
        bits = n ? *p : 0;
    }

#if STRINGLIB_SIZEOF_CHAR == 4
    if (bits & MASK_UCS2) {
        return MAX_CHAR_UCS4;
    }
#endif
#if STRINGLIB_SIZEOF_CHAR >= 2
    if (bits & MASK_UCS1) {
        return MAX_CHAR_UCS2;
    }
#endif
    if (bits & MASK_ASCII) {
        return MAX_CHAR_UCS1;
    }
    return MAX_CHAR_ASCII;
}

#undef MASK_LIMIT
#undef MAX_CHAR_LIMIT
#undef MASK_ASCII
#undef MASK_UCS1
#undef MASK_UCS2
#undef MAX_CHAR_ASCII
#undef MAX_CHAR_UCS1
#undef MAX_CHAR_UCS2
#undef MAX_CHAR_UCS4
