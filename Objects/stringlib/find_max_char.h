/* Finding the optimal width of unicode characters in a buffer */

/* find_max_char for one-byte will work for bytes objects as well. */
#if !STRINGLIB_IS_UNICODE && STRINGLIB_SIZEOF_CHAR > 1
# error "find_max_char.h is specific to Unicode"
#endif

#define MAX_CHAR_ASCII 0x7f
#define MAX_CHAR_UCS1  0xff
#define MAX_CHAR_UCS2  0xffff
#define MAX_CHAR_UCS4  0x10ffff

/* Bits that are set in a character wider than ASCII, UCS1 or UCS2. */
#define MASK_ASCII 0xFFFFFF80
#define MASK_UCS1  0xFFFFFF00
#define MASK_UCS2  0xFFFF0000

#if STRINGLIB_SIZEOF_CHAR == 1
# define FIND_MAX_CHAR_TYPE unsigned char   /* bytes use a plain, maybe signed, char */
# define MASK_MAX_CHAR MASK_ASCII
# define MAX_CHAR MAX_CHAR_UCS1
#elif STRINGLIB_SIZEOF_CHAR == 2
# define FIND_MAX_CHAR_TYPE STRINGLIB_CHAR
# define MASK_MAX_CHAR MASK_UCS1
# define MAX_CHAR MAX_CHAR_UCS2
#elif STRINGLIB_SIZEOF_CHAR == 4
# define FIND_MAX_CHAR_TYPE STRINGLIB_CHAR
# define MASK_MAX_CHAR MASK_UCS2
# define MAX_CHAR MAX_CHAR_UCS4
#else
# error Invalid STRINGLIB_SIZEOF_CHAR (must be 1, 2 or 4)
#endif

/* Number of characters OR-ed together before checking for an early exit.
   Smaller blocks exit sooner on wide data but pay a reduction per block. */
#define FIND_MAX_CHAR_BLOCK 64

/* Return an upper bound on the widest character in [begin, end): one of
   MAX_CHAR_ASCII, MAX_CHAR_UCS1, MAX_CHAR_UCS2 or MAX_CHAR_UCS4. */
Py_LOCAL_INLINE(Py_UCS4)
STRINGLIB(find_max_char)(const STRINGLIB_CHAR *begin, const STRINGLIB_CHAR *end)
{
    const FIND_MAX_CHAR_TYPE *p = (const FIND_MAX_CHAR_TYPE *)begin;
    const FIND_MAX_CHAR_TYPE *_end = (const FIND_MAX_CHAR_TYPE *)end;
    FIND_MAX_CHAR_TYPE bits = 0;

    /* OR the characters together instead of comparing each one: the loop
       has no data-dependent branch, so compilers vectorize it. */
    while (_end - p >= FIND_MAX_CHAR_BLOCK) {
        FIND_MAX_CHAR_TYPE block_bits = 0;
        for (int i = 0; i < FIND_MAX_CHAR_BLOCK; i++) {
            block_bits |= p[i];
        }
        bits |= block_bits;
        if (bits & MASK_MAX_CHAR) {
            return MAX_CHAR;
        }
        p += FIND_MAX_CHAR_BLOCK;
    }
    while (p < _end) {
        bits |= *p++;
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

#undef FIND_MAX_CHAR_BLOCK
#undef FIND_MAX_CHAR_TYPE
#undef MASK_MAX_CHAR
#undef MAX_CHAR
#undef MASK_ASCII
#undef MASK_UCS1
#undef MASK_UCS2
#undef MAX_CHAR_ASCII
#undef MAX_CHAR_UCS1
#undef MAX_CHAR_UCS2
#undef MAX_CHAR_UCS4
