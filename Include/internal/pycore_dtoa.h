#ifndef Py_INTERNAL_DTOA_H
#define Py_INTERNAL_DTOA_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* Implemented in Python/floatconv.c (Żmij + ffc.h); see the contracts there. */
extern double _Py_dg_strtod(const char *str, char **ptr);
extern char* _Py_dg_dtoa(double d, int mode, int ndigits,
                         int *decpt, int *sign, char **rve);
extern void _Py_dg_freedtoa(char *s);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_DTOA_H */
