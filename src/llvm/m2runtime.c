/*
 * m2runtime.c  —  Modula-2 Runtime Library
 *
 * Implements the external symbols declared by MCP4LLVM.c's emit_runtime_decls().
 * Compile and link this alongside the LLVM-generated object file:
 *
 *   llc -filetype=obj prog.ll -o prog.o
 *   cc prog.o m2runtime.o -o prog -lm
 *
 * LLVM type  →  C type
 *   i32      →  int32_t
 *   i8       →  int8_t  (char)
 *   double   →  double
 *   ptr      →  void*   (opaque pointer)
 *
 * Trap codes (CASE range error uses 4):
 *   1 = range check      2 = arithmetic overflow
 *   3 = nil dereference  4 = CASE no arm matched
 *   5 = stack overflow   6 = assertion failed
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* =========================================================================
 * Output  (maps to InOut.Write* / Terminal.Write*)
 * ========================================================================= */

void M2_WriteLn(void) {
    putchar('\n');
    fflush(stdout);
}

void M2_WriteInt(int32_t i) {
    printf("%d", i);
}

void M2_WriteCard(int32_t c) {   /* declared i32 in IR; treated as unsigned */
    printf("%u", (uint32_t)c);
}

void M2_WriteReal(double r) {
    /* Modula-2 REAL output: fixed notation, 6 significant digits */
    printf("%.6g", r);
}

void M2_WriteChar(int32_t c) {   /* i8 promoted to i32 on x86-64 ABI */
    putchar((unsigned char)(c & 0xFF));
}

void M2_WriteString(const char* s) {
    if (s) fputs(s, stdout);
}

/* =========================================================================
 * Input  (maps to InOut.Read*)
 * ========================================================================= */

void M2_ReadInt(int32_t* p) {
    if (scanf("%d", p) != 1) *p = 0;
}

void M2_ReadCard(uint32_t* p) {
    if (scanf("%u", p) != 1) *p = 0;
}

void M2_ReadReal(double* p) {
    if (scanf("%lf", p) != 1) *p = 0.0;
}

/* =========================================================================
 * Memory  (NEW / DISPOSE → malloc / free)
 * ========================================================================= */

void* M2_New(int32_t size) {
    void* p = calloc(1, (size_t)(size > 0 ? size : 1));
    if (!p) {
        fputs("Modula-2 runtime: NEW out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void M2_Dispose(void* p) {
    free(p);
}

/* =========================================================================
 * Arithmetic helpers  (INC / DEC)
 *
 * The pointer always refers to an i32 alloca in the generated code.
 * ========================================================================= */

void M2_Inc(int32_t* p, int32_t n) {
    *p += n;
}

void M2_Dec(int32_t* p, int32_t n) {
    *p -= n;
}

/* =========================================================================
 * Control flow
 * ========================================================================= */

void M2_Halt(void) {
    exit(0);
}

static const char* trap_msg(int32_t code) {
    switch (code) {
        case 1: return "range check failed";
        case 2: return "arithmetic overflow";
        case 3: return "nil pointer dereference";
        case 4: return "CASE: no arm matched selector";
        case 5: return "stack overflow";
        case 6: return "assertion failed";
        default: return "unknown runtime error";
    }
}

void M2_Trap(int32_t code) {
    fprintf(stderr, "\nModula-2 runtime trap %d: %s\n", code, trap_msg(code));
    exit(1);
}

/* =========================================================================
 * Coroutines  (SYSTEM.TRANSFER / SYSTEM.NEWPROCESS)
 *
 * Full coroutine support requires platform-specific context switching
 * (ucontext_t on POSIX, fibers on Windows).  Stubbed here; replace with
 * a real implementation when coroutines are needed.
 * ========================================================================= */

void M2_Transfer(void** from_proc, void** to_proc) {
    (void)from_proc;
    (void)to_proc;
    fputs("Modula-2 runtime: TRANSFER (coroutines) not implemented\n", stderr);
    exit(1);
}
