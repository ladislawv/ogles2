/*
 * Force-included (-include) into every gl4es source for AmigaOS 3 (m68k,
 * GCC 6.5 + clib2): what gl4es expects of the platform and the toolchain
 * lacks. ports/gl4es/README.md.
 */
#ifndef OGLES2_GL4ES_OS3_BUILD_H
#define OGLES2_GL4ES_OS3_BUILD_H

/* gl4es exports each GL entry point as an alias of its gl4es_* function
 * (gl/attributes.h). The Amiga's a.out objects have no alias attribute; an
 * assembler equate in the same file makes the second name. */
#define OS3_STR(x) #x
#define OS3_XSTR(x) OS3_STR(x)
#define OS3_ALIAS(RET, ENM, DEF, INM) \
    RET ENM DEF;                      \
    __asm__(".globl _" OS3_XSTR(ENM) "\n_" OS3_XSTR(ENM) " = _" OS3_XSTR(INM) "\n")

#define EXPORT
#define AliasExport(RET, NAME, X, DEF)       OS3_ALIAS(RET, NAME##X, DEF, gl4es_##NAME)
#define AliasExport_A(RET, NAME, X, DEF, INM) OS3_ALIAS(RET, NAME##X, DEF, gl4es_##INM)
#define AliasExport_D(RET, NAME, X, DEF)     AliasExport(RET, NAME, X, DEF)
#define AliasExport_D_1(RET, NAME, X, DEF)   AliasExport(RET, NAME, X, DEF)
#define AliasExport_M(RET, NAME, X, DEF, SUF) AliasExport(RET, NAME, X, DEF)
#define AliasExport_V(RET, NAME)             AliasExport(RET, NAME, , (void))
#define AliasExport_1(RET, NAME, X, DEF)     AliasExport(RET, NAME, X, DEF)
#define NonAliasExportDecl(RET, NAME, DEF)   RET NAME DEF
#define AliasDecl(RET, NAME, DEF, OLD)       OS3_ALIAS(RET, NAME, DEF, OLD)

/* glx.c's crash handler names signals clib2 does not define (it is only
 * installed with LIBGL_STACKTRACE or LIBGL_XREFRESH). */
#include <signal.h>
#ifndef SIGBUS
#define SIGBUS SIGSEGV
#endif
#ifndef SIGQUIT
#define SIGQUIT SIGTERM
#endif

#endif
