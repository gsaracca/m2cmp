/*
 * MCP4LLVM.c  —  LLVM IR Code Generator for the Modula-2 Compiler
 *
 * Replaces:
 *   MCP4MAIN.MOD  (statement dispatch + block traversal)
 *   MCP4CODE.MOD  (M-code stack-machine emission)
 *   MCP4ATTR.MOD  (load-mode attribute system)
 *   MCP4EXPR.MOD  (expression code generation)
 *   MCP4CALL.MOD  (procedure call code generation)
 *
 * Reads:   IL1 binary stream produced by Pass 3 (MCP3MAIN.MOD)
 * Writes:  textual LLVM IR  (.ll)  — no LLVM library required
 *
 * Compilation pipeline:
 *   source.MOD
 *     → [Pass 1] IL1, ASCII
 *     → [Pass 2] IL2, SYM
 *     → [Pass 3] IL1'          ← consumed here
 *     → [MCP4LLVM] out.ll
 *     → llc / clang → out.o / out.exe
 *
 * SSA strategy:
 *   All variables → alloca in function entry block.
 *   Reads  → load; writes → store.
 *   LLVM's mem2reg pass promotes these to proper SSA registers.
 *   This mirrors what Clang itself does for C locals.
 *
 * Single-process assumption:
 *   IL1 tokens of kind namesy/proceduresy/modulesy carry raw C pointers
 *   to the Identrec/Structrec nodes built by Passes 1-3.  This file must
 *   therefore be compiled and linked into the same binary as those passes.
 *   The struct layouts below must match the Modula-2 compiler's ABI
 *   (e.g. gm2 generates C-compatible layouts for flat records).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>

/* =========================================================================
 * 1.  MCBase TYPE MIRROR
 *
 *     C equivalents of the Modula-2 variant records from MCBase.DEF.
 *     Variant fields that are not needed by the code generator are omitted
 *     or collapsed; add them when required.
 * ========================================================================= */

typedef struct Identrec  Identrec;
typedef struct Structrec Structrec;
typedef Identrec*  Idptr;
typedef Structrec* Stptr;

/* Structform — matches MCBase.DEF enum order exactly */
typedef enum {
    enums, bools, chars, ints, cards, words,
    subranges, reals, pointers, sets, proctypes,
    arrays, records, hides, opens
} Structform;

/* Idclass — matches MCBase.DEF enum order exactly */
typedef enum {
    consts, types, vars, fields, pures, funcs,
    mods, unknown, indrct
} Idclass;

typedef enum { noparam, valparam, varparam, copyparam } Varkind;
typedef enum { global, local, absolute, separate }       Kindvar;
typedef enum { decp,disp,exlp,halp,incp,inlp,newp,nprp,trsp } Stpures;
typedef enum { absf,adrf,capf,chrf,fltf,higf,oddf,ordf,sizf,trcf,tszf,valf } Stfuncs;

/* Structrec — type descriptor */
struct Structrec {
    uint32_t    size;       /* word size (1 word = 2 bytes on Lilith) */
    Idptr       stidp;
    int         inlist;
    Structform  form;
    union {
        /* enums */
        struct { Idptr fcstp; uint32_t cstnr; } e;
        /* subranges */
        struct { Stptr scalp; uint32_t min, max; } sr;
        /* pointers */
        struct { Stptr elemp; } ptr;
        /* sets */
        struct { Stptr basep; } set;
        /* arrays */
        struct { Stptr elp, ixp; int dyn; } arr;
        /* records */
        struct { Idptr fieldp; } rec;
        /* proctypes */
        struct {
            Idptr fstparam;
            Idclass rkind;
            Stptr funcp;   /* return type (funcs only) */
        } proc;
        /* opens */
        struct { Stptr openstruc; } open;
    };
};

typedef struct {
    uint32_t value;   /* general one-word constant */
    int32_t  ivalue;  /* signed interpretation */
} Constval;

/* Identrec — identifier/symbol table node */
struct Identrec {
    uint32_t  name;      /* Spellix: index into identifier table */
    Idptr     link;
    int       val_flag;  /* TRUE → idtyp valid; FALSE → nxtidp valid */
    union {
        Idptr nxtidp;
        Stptr idtyp;
    };
    Idptr     globmodp;
    Idclass   klass;
    union {
        /* consts */
        Constval cvalue;
        /* vars */
        struct {
            int     indaccess;
            Kindvar state;
            Varkind vkind;
            uint32_t vlevel;
            uint32_t vaddr;
            Idptr    vlink;
        } var;
        /* fields */
        uint32_t fldaddr;
        /* pures / funcs / mods */
        struct {
            int      isstandard;
            uint32_t procnum;
            Idptr    locp;
            uint32_t plev;
            uint32_t varlength;
            uint32_t priolev;
            int      externalaccess;
            Idptr    locvarp;   /* local vars (non-code proc) */
            /* mods */
            int      globalmodule;
            Idptr    globvarp;
            uint32_t modnum;
            char     identifier[24];
            /* standard pures/funcs */
            Stpures pname;
            Stfuncs fname;
        } proc;
    };
};

/* Well-known type pointers — populated from MCBase globals on startup */
static Stptr g_boolptr, g_charptr, g_intptr, g_cardptr;
static Stptr g_realptr,  g_wordptr, g_addrptr;

/* =========================================================================
 * 2.  GLOBAL COMPILATION STATE
 * ========================================================================= */

static FILE*    ll_out   = NULL;
static FILE*    il1_in   = NULL;
static FILE*    asc_in   = NULL;   /* ASCII spelling table (Pass 1 output) */

static uint32_t reg_ctr  = 0;   /* SSA register counter    %t.N  */
static uint32_t lbl_ctr  = 0;   /* basic-block label counter L.N  */
static uint32_t str_ctr  = 0;   /* string constant counter        */

static int      unreachable = 0; /* suppress IR after ret/br       */

static Idptr    cur_proc   = NULL;  /* NULL → module body           */
static Idptr    cur_module = NULL;  /* main module Identrec         */

/* =========================================================================
 * 3.  IL1 READER   (mirrors MCP4GLOB.MOD scanner)
 *
 *  IL1 word layout (16-bit, big-endian on Lilith):
 *    bits 15..8  = Symbol enum value
 *    bits  7..0  = source column (for diagnostics)
 *
 *  Symbols >= proceduresy carry one extra word (or pointer) after the token.
 * ========================================================================= */

/* Symbol — must match MCBase.DEF Symbol enum exactly */
typedef enum {
    eop=0,
    andsy,divsy,times,slash,modsy,notsy,plus,minus,orsy,   /*  1.. 9 */
    eql,neq,grt,geq,lss,leq,insy,                          /* 10..16 */
    lparent,rparent,lbrack,rbrack,lconbr,rconbr,           /* 17..22 */
    comma,semicolon,period,colon,range,                     /* 23..27 */
    constsy,typesy,varsy,arraysy,recordsy,variant,setsy,    /* 28..34 */
    pointersy,tosy,arrow,hidden,                            /* 35..38 */
    importsy,exportsy,fromsy,qualifiedsy,                   /* 39..42 */
    codesy,beginsy,                                         /* 43..44 */
    casesy,ofsy,ifsy,thensy,elsifsy,elsesy,loopsy,          /* 45..51 */
    exitsy,repeatsy,untilsy,whilesy,dosy,withsy,            /* 52..57 */
    forsy,bysy,returnsy,becomes,endsy,                      /* 58..62 */
    call,endblock,                                          /* 63..64 */
    definitionsy,implementationsy,proceduresy,modulesy,     /* 65..68 */
    symbolsy,                                               /* 69     */
    ident,intcon,cardcon,intcarcon,realcon,charcon,stringcon,/* 70..76 */
    option,errorsy,eol,                                     /* 77..79 */
    namesy,                                                 /* 80     */
    field,anycon                                            /* 81..82 */
} Symbol;

static Symbol   sy;
static int32_t  val;
static Idptr    nptr;
static Stptr    cstPtr;
static int32_t  cString;
static uint32_t il1_line = 1;
static uint32_t il1_col  = 0;
static int      ctrl_range_check  = 1;
static int      arith_range_check = 1;

static uint16_t il1_word(void) {
    uint8_t hi = 0, lo = 0;
    fread(&hi, 1, 1, il1_in);
    fread(&lo, 1, 1, il1_in);
    return (uint16_t)((hi << 8) | lo);
}
static void* il1_ptr(void) {
    void* p = NULL;
    fread(&p, sizeof p, 1, il1_in);
    return p;
}
static int32_t il1_i32(void) {
    int32_t v = 0;
    fread(&v, sizeof v, 1, il1_in);
    return v;
}

static void get_symbol(void) {
    for (;;) {
        if (feof(il1_in)) { sy = eop; return; }
        uint16_t w = il1_word();
        sy      = (Symbol)(w >> 8);
        il1_col = w & 0xFF;

        if (sy < proceduresy) return; /* no payload word follows */

        if (sy == namesy || sy == modulesy || sy == proceduresy) {
            nptr = (Idptr)il1_ptr();
        } else if (sy == eol) {
            il1_line = (uint32_t)il1_i32();
            continue; /* transparent — skip eol and loop */
        } else if (sy == field) {
            val = il1_i32();
        } else if (sy == option) {
            val = il1_i32();
            char opt = (char)val;
            get_symbol();
            if      (opt == 'T') ctrl_range_check  = (sy == plus);
            else if (opt == 'R') arith_range_check = (sy == plus);
            get_symbol();
            continue;
        } else if (sy == anycon) {
            cstPtr  = (Stptr)il1_ptr();
            cString = il1_i32();
            val     = cString;
        }
        return;
    }
}

/* =========================================================================
 * 4.  TYPE SYSTEM  —  Modula-2 type node → LLVM IR type string
 *
 *  Mapping:
 *    INTEGER, CARDINAL, subrange-of-int/card  →  i32
 *    BOOLEAN                                  →  i1
 *    CHAR                                     →  i8
 *    REAL                                     →  double
 *    POINTER TO T, PROC, ADDRESS              →  ptr   (opaque pointers)
 *    ARRAY [n..m] OF T                        →  [count x ElemType]
 *    RECORD { f1:T1; f2:T2; ... }             →  { T1, T2, ... }
 *    SET OF T  (BITSET)                       →  i32
 *    WORD                                     →  i32
 * ========================================================================= */

static const char* llvm_type(Stptr sp, char* buf, size_t bufsz) {
    if (!sp) { strncpy(buf, "i32", bufsz); return buf; }
    switch (sp->form) {
        case bools:     return "i1";
        case chars:     return "i8";
        case ints:
        case cards:
        case words:     return "i32";
        case reals:     return "double";
        case pointers:
        case proctypes: return "ptr";
        case sets:      return "i32";  /* BITSET fits in one word */
        case subranges:
            return llvm_type(sp->sr.scalp, buf, bufsz);
        case enums:
            return "i32";
        case arrays: {
            uint32_t lo  = sp->arr.ixp->sr.min;
            uint32_t hi  = sp->arr.ixp->sr.max;
            uint32_t cnt = hi - lo + 1;
            char ebuf[256];
            snprintf(buf, bufsz, "[%u x %s]", cnt,
                     llvm_type(sp->arr.elp, ebuf, sizeof ebuf));
            return buf;
        }
        case records: {
            /* Walk field list and build "{ T0, T1, ... }" */
            char tmp[2048] = "{ ";
            Idptr fp = sp->rec.fieldp;
            int first = 1;
            while (fp) {
                if (!first) strncat(tmp, ", ", sizeof tmp - strlen(tmp) - 1);
                char fbuf[256];
                strncat(tmp, llvm_type(fp->idtyp, fbuf, sizeof fbuf),
                        sizeof tmp - strlen(tmp) - 1);
                first = 0;
                fp = fp->link;
            }
            strncat(tmp, " }", sizeof tmp - strlen(tmp) - 1);
            strncpy(buf, tmp, bufsz);
            return buf;
        }
        case hides:
        case opens:
            return llvm_type(sp->open.openstruc, buf, bufsz);
        default:
            strncpy(buf, "i32", bufsz);
            return buf;
    }
}

/* Word size (Lilith words) → byte count */
static uint32_t type_bytes(Stptr sp) {
    return sp ? sp->size * 2u : 4u; /* 1 Lilith word = 2 bytes */
}

/* =========================================================================
 * 5.  SPELLING TABLE + NAME MANGLER
 *
 * ASC file format (written by MCP1IO IdentSystem):
 *   Identifiers are written sequentially as raw characters, each terminated
 *   by a space (' ').  Spellix (= ip->name) is the byte offset in the file
 *   where the spelling begins.  Read chars until space to obtain the name.
 *
 * LLVM IR identifiers produced:
 *    @M2_ModName__ProcName_N   global procedure (N = procnum disambiguator)
 *    @M2_ModName__VarName      global variable
 *    %v_VarName_N              local alloca  (N = vaddr disambiguator)
 *    @M2_str_N                 string literal constant
 * ========================================================================= */

static void spell_lookup(uint32_t spix, char* buf, size_t n) {
    if (!asc_in || spix == 0 || n == 0) { snprintf(buf, n, "s%u", spix); return; }
    if (fseek(asc_in, (long)spix, SEEK_SET) != 0) { snprintf(buf, n, "s%u", spix); return; }
    size_t i = 0;
    int c;
    while (i + 1 < n && (c = fgetc(asc_in)) != EOF && c != ' ')
        buf[i++] = (char)c;
    buf[i] = '\0';
    /* Sanitize: replace chars that are illegal in unquoted LLVM identifiers */
    for (char* p = buf; *p; p++)
        if (*p == '-' || *p == '.' || *p == ' ') *p = '_';
}

static void mangle_module_var(const char* modname, const char* varname,
                               char* buf, size_t n) {
    snprintf(buf, n, "@M2_%s__%s", modname, varname);
}

/* Resolve Idptr to its source name and emit a global variable reference */
static void mangle_var_id(Idptr ip, char* buf, size_t n) {
    char modname[64] = "anon", varname[64] = "v";
    if (ip->globmodp) spell_lookup(ip->globmodp->name, modname, sizeof modname);
    if (ip->name)     spell_lookup(ip->name,           varname, sizeof varname);
    mangle_module_var(modname, varname, buf, n);
}

static void mangle_proc(Idptr ip, char* buf, size_t n) {
    char modname[64] = "anon", procname[64] = "p";
    if (ip->globmodp) spell_lookup(ip->globmodp->name, modname,  sizeof modname);
    if (ip->name)     spell_lookup(ip->name,           procname, sizeof procname);
    /* Append procnum so nested procs with the same name stay unique */
    snprintf(buf, n, "@M2_%s__%s_%u", modname, procname, ip->proc.procnum);
}

static void mangle_local_var(Idptr ip, char* buf, size_t n) {
    char varname[64] = "v";
    if (ip->name) spell_lookup(ip->name, varname, sizeof varname);
    /* Append vaddr to keep names unique across nested scopes */
    snprintf(buf, n, "%%v_%s_%u", varname, ip->var.vaddr);
}

static void mangle_string(uint32_t idx, char* buf, size_t n) {
    snprintf(buf, n, "@M2_str_%u", idx);
}

/* =========================================================================
 * 6.  CORE LLVM IR EMITTER
 * ========================================================================= */

/* Fresh SSA register: %t.N */
static const char* new_reg(char* buf, size_t n) {
    snprintf(buf, n, "%%t.%u", reg_ctr++);
    return buf;
}
/* Fresh basic-block label: L.N */
static const char* new_label(char* buf, size_t n) {
    snprintf(buf, n, "L.%u", lbl_ctr++);
    return buf;
}

/* Indented IR instruction */
static void ir(const char* fmt, ...) {
    if (unreachable) return;
    va_list ap; va_start(ap, fmt);
    fputs("  ", ll_out);
    vfprintf(ll_out, fmt, ap);
    fputc('\n', ll_out);
    va_end(ap);
}

/* Basic-block label (resets unreachable) */
static void ir_label(const char* lbl) {
    unreachable = 0;
    fprintf(ll_out, "\n%s:\n", lbl);
}

static void ir_br(const char* lbl) {
    ir("br label %%%s", lbl);
    unreachable = 1;
}
static void ir_condbr(const char* cond, const char* t, const char* f) {
    ir("br i1 %s, label %%%s, label %%%s", cond, t, f);
    unreachable = 1;
}
static void ir_ret_void(void) {
    ir("ret void");
    unreachable = 1;
}
static void ir_ret(Stptr type, const char* vreg) {
    char tbuf[256];
    ir("ret %s %s", llvm_type(type, tbuf, sizeof tbuf), vreg);
    unreachable = 1;
}

/* Load / store */
static void ir_load(const char* dst, Stptr type, const char* ptr_reg) {
    char tbuf[256];
    ir("%s = load %s, ptr %s", dst, llvm_type(type, tbuf, sizeof tbuf), ptr_reg);
}
static void ir_store(Stptr type, const char* val_reg, const char* ptr_reg) {
    char tbuf[256];
    ir("store %s %s, ptr %s", llvm_type(type, tbuf, sizeof tbuf), val_reg, ptr_reg);
}

/* GEP into a struct by field index */
static void ir_gep_field(const char* dst, Stptr struct_type,
                          const char* base, uint32_t field_idx) {
    char tbuf[256];
    ir("%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %u",
       dst, llvm_type(struct_type, tbuf, sizeof tbuf), base, field_idx);
}

/* GEP into an array by element index (register) */
static void ir_gep_array(const char* dst, Stptr arr_type,
                          const char* base, const char* idx_reg) {
    char tbuf[256];
    ir("%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %s",
       dst, llvm_type(arr_type, tbuf, sizeof tbuf), base, idx_reg);
}

/* Integer binary operations */
static void ir_binop(const char* dst, const char* op,
                      Stptr type, const char* l, const char* r) {
    char tbuf[256];
    ir("%s = %s %s %s, %s", dst, op, llvm_type(type, tbuf, sizeof tbuf), l, r);
}

/* Integer comparison → i1 result */
static void ir_icmp(const char* dst, const char* pred,
                     Stptr type, const char* l, const char* r) {
    char tbuf[256];
    ir("%s = icmp %s %s %s, %s", dst, pred, llvm_type(type, tbuf, sizeof tbuf), l, r);
}

/* Float comparison → i1 result */
static void ir_fcmp(const char* dst, const char* pred, const char* l, const char* r) {
    ir("%s = fcmp %s double %s, %s", dst, pred, l, r);
}

/* Call (returns register name or "" if void) */
static void ir_call_void(const char* func, const char* args_ir) {
    ir("call void %s(%s)", func, args_ir);
}
static void ir_call(const char* dst, Stptr ret_type,
                    const char* func, const char* args_ir) {
    char tbuf[256];
    ir("%s = call %s %s(%s)", dst, llvm_type(ret_type, tbuf, sizeof tbuf), func, args_ir);
}

/* Alloca — always emitted into the function's entry block.
   We buffer them and flush after the "entry:" label is written. */
typedef struct AllocaEntry AllocaEntry;
struct AllocaEntry {
    char name[64];
    char type_str[256];
    AllocaEntry* next;
};
static AllocaEntry* alloca_list = NULL;

static void declare_alloca(const char* name, Stptr type) {
    AllocaEntry* e = calloc(1, sizeof *e);
    strncpy(e->name, name, sizeof e->name - 1);
    llvm_type(type, e->type_str, sizeof e->type_str);
    e->next = alloca_list;
    alloca_list = e;
}

static void flush_allocas(void) {
    /* Emit in declaration order (reverse insertion: walk list) */
    AllocaEntry* stack[512]; int top = 0;
    for (AllocaEntry* e = alloca_list; e && top < 512; e = e->next)
        stack[top++] = e;
    for (int i = top - 1; i >= 0; i--)
        fprintf(ll_out, "  %s = alloca %s\n", stack[i]->name, stack[i]->type_str);
    /* Free */
    while (alloca_list) {
        AllocaEntry* n = alloca_list->next;
        free(alloca_list);
        alloca_list = n;
    }
}

/* =========================================================================
 * 7.  ATTRIBUTE SYSTEM   (replaces MCP4ATTR.MOD)
 *
 *  Instead of push/pop on a virtual stack we track each value as an Attr:
 *  a tagged union that holds either a constant or a register name.
 *
 *  The Load/Store/LoadAddr operations from MCP4ATTR become:
 *    load_attr  → emit a load instruction, return register
 *    store_attr → emit a store instruction
 *    addr_of    → return pointer register (alloca or GEP result)
 * ========================================================================= */

typedef enum {
    ATTR_CONST,    /* compile-time integer/bool/char constant       */
    ATTR_RCONST,   /* compile-time REAL constant                    */
    ATTR_GLOBAL,   /* pointer to a global (@M2_Mod__Name)           */
    ATTR_LOCAL,    /* pointer to a local alloca (%v_N)              */
    ATTR_REG,      /* SSA register already holding the value        */
    ATTR_PTREG,    /* pointer register + byte offset (for indexing) */
    ATTR_PROC,     /* procedure reference (@M2_Mod__pN)             */
    ATTR_STRING    /* @M2_str_N (pointer to string data)            */
} AttrMode;

typedef struct {
    AttrMode mode;
    Stptr    type;
    char     reg[64];    /* register / global name                  */
    int32_t  cval;       /* ATTR_CONST: integer/char/bool value     */
    double   rval;       /* ATTR_RCONST: float value                */
    int32_t  byte_off;   /* ATTR_PTREG: byte offset from base ptr   */
} Attr;

static Attr make_const_attr(Stptr type, int32_t v) {
    Attr a = {0};
    a.mode = ATTR_CONST; a.type = type; a.cval = v;
    return a;
}

/* Produce the LLVM IR value string for an Attr (for use in instructions) */
static const char* attr_value_str(Attr a, char* buf, size_t n) {
    if (a.mode == ATTR_CONST) {
        if (a.type == g_boolptr)
            snprintf(buf, n, "%s", a.cval ? "1" : "0");
        else
            snprintf(buf, n, "%d", a.cval);
    } else if (a.mode == ATTR_RCONST) {
        snprintf(buf, n, "%g", a.rval);
    } else {
        strncpy(buf, a.reg, n);
    }
    return buf;
}

/* Load the value of 'a' into a fresh SSA register; return REG attr */
static Attr load_attr(Attr a) {
    if (a.mode == ATTR_CONST || a.mode == ATTR_RCONST || a.mode == ATTR_REG)
        return a;  /* already a value */

    char dst[64]; new_reg(dst, sizeof dst);
    if (a.mode == ATTR_GLOBAL || a.mode == ATTR_LOCAL) {
        ir_load(dst, a.type, a.reg);
    } else if (a.mode == ATTR_PTREG) {
        /* base pointer + byte offset → GEP then load */
        char gep[64]; new_reg(gep, sizeof gep);
        ir("%s = getelementptr i8, ptr %s, i32 %d", gep, a.reg, a.byte_off);
        ir_load(dst, a.type, gep);
    } else if (a.mode == ATTR_PROC || a.mode == ATTR_STRING) {
        /* pointers are values themselves */
        strncpy(dst, a.reg, sizeof dst - 1);
        Attr r = a; r.mode = ATTR_REG; strncpy(r.reg, dst, sizeof r.reg - 1);
        return r;
    }
    Attr r = a; r.mode = ATTR_REG; strncpy(r.reg, dst, sizeof r.reg - 1);
    return r;
}

/* Get a pointer register for 'a' (so we can store into it) */
static const char* addr_of(Attr a, char* dst, size_t n) {
    if (a.mode == ATTR_GLOBAL || a.mode == ATTR_LOCAL) {
        strncpy(dst, a.reg, n);
        return dst;
    }
    if (a.mode == ATTR_PTREG && a.byte_off == 0) {
        strncpy(dst, a.reg, n);
        return dst;
    }
    if (a.mode == ATTR_PTREG) {
        new_reg(dst, n);
        ir("%s = getelementptr i8, ptr %s, i32 %d", dst, a.reg, a.byte_off);
        return dst;
    }
    /* Should not reach here for a valid lvalue */
    assert(0 && "addr_of: not an lvalue");
    return NULL;
}

/* Store src into the location described by dst */
static void store_attr(Attr dst_a, Attr src_a) {
    Attr val = load_attr(src_a);
    char vbuf[64]; attr_value_str(val, vbuf, sizeof vbuf);
    char ptr[64];  addr_of(dst_a, ptr, sizeof ptr);
    ir_store(dst_a.type, vbuf, ptr);
}

/* Build an Attr from an Identrec (variable, procedure, etc.) */
static Attr attr_from_id(Idptr ip, uint32_t cur_level) {
    Attr a = {0};
    a.type = ip->idtyp;

    if (ip->klass == vars) {
        a.cval = (int32_t)ip->var.vaddr;
        switch (ip->var.state) {
            case global:
                a.mode = ATTR_GLOBAL;
                mangle_var_id(ip, a.reg, sizeof a.reg);
                break;
            case local:
                a.mode = ATTR_LOCAL;
                mangle_local_var(ip, a.reg, sizeof a.reg);
                break;
            case separate:
                /* External module variable — treat as global */
                a.mode = ATTR_GLOBAL;
                mangle_var_id(ip, a.reg, sizeof a.reg);
                break;
            case absolute:
                /* Absolute address (SYSTEM usage) — emit as inttoptr */
                a.mode = ATTR_REG;
                new_reg(a.reg, sizeof a.reg);
                ir("%s = inttoptr i32 %u to ptr", a.reg, ip->var.vaddr);
                break;
        }
        /* Indirect-access variables store a pointer in their slot */
        if (ip->var.indaccess) {
            Attr ptr_a = a; ptr_a.type = ip->idtyp;
            Attr loaded = load_attr(ptr_a);
            a.mode    = ATTR_PTREG;
            a.byte_off = 0;
            strncpy(a.reg, loaded.reg, sizeof a.reg - 1);
        }
    } else if (ip->klass == pures || ip->klass == funcs) {
        a.mode = ATTR_PROC;
        mangle_proc(ip, a.reg, sizeof a.reg);
    }
    return a;
}

/* =========================================================================
 * 8.  BLOCK SYSTEM   (replaces MCP4CODE BlockSystem)
 * ========================================================================= */

/*
 * Function signature in LLVM IR:
 *
 *   define <RetType> @M2_Mod__pN(<ParamTypes>) {
 *   entry:
 *     <alloca for params>
 *     <alloca for locals>
 *     <store params into allocas>
 *     <body basic blocks>
 *   }
 */
static void emit_function_begin(Idptr proc) {
    cur_proc = proc;
    reg_ctr  = 0;
    unreachable = 0;
    alloca_list = NULL;

    char fname[128];
    mangle_proc(proc, fname, sizeof fname);

    /* Build parameter list */
    char params[1024] = "";
    Idptr param = proc->idtyp ? proc->idtyp->proc.fstparam : NULL;
    int   first = 1;
    while (param) {
        char tbuf[256], pbuf[64];
        llvm_type(param->idtyp, tbuf, sizeof tbuf);
        snprintf(pbuf, sizeof pbuf, "%%arg_%u", param->var.vaddr);
        if (!first) strncat(params, ", ", sizeof params - strlen(params) - 1);
        char pair[320];
        snprintf(pair, sizeof pair, "%s %s", tbuf, pbuf);
        strncat(params, pair, sizeof params - strlen(params) - 1);
        first = 0;
        param = param->var.vlink;
    }

    /* Return type */
    char ret_tbuf[256];
    if (proc->klass == funcs && proc->idtyp && proc->idtyp->proc.rkind == funcs)
        llvm_type(proc->idtyp->proc.funcp, ret_tbuf, sizeof ret_tbuf);
    else
        strncpy(ret_tbuf, "void", sizeof ret_tbuf);

    fprintf(ll_out, "\ndefine %s %s(%s) {\n", ret_tbuf, fname, params);
    fprintf(ll_out, "entry:\n");

    /* Register allocas for parameters and locals; flush immediately */
    param = proc->idtyp ? proc->idtyp->proc.fstparam : NULL;
    while (param) {
        char pname[64]; mangle_local_var(param, pname, sizeof pname);
        declare_alloca(pname, param->idtyp);
        param = param->var.vlink;
    }
    Idptr lv = proc->proc.locvarp;
    while (lv) {
        char lname[64]; mangle_local_var(lv, lname, sizeof lname);
        declare_alloca(lname, lv->idtyp);
        lv = lv->var.vlink;
    }
    flush_allocas();

    /* Store incoming parameter values into their allocas */
    param = proc->idtyp ? proc->idtyp->proc.fstparam : NULL;
    while (param) {
        char pname[64]; mangle_local_var(param, pname, sizeof pname);
        char arg[64];   snprintf(arg, sizeof arg, "%%arg_%u", param->var.vaddr);
        char tbuf[256]; llvm_type(param->idtyp, tbuf, sizeof tbuf);
        ir("store %s %s, ptr %s", tbuf, arg, pname);
        param = param->var.vlink;
    }
}

static void emit_function_end(void) {
    /* Ensure the function is terminated */
    if (!unreachable) ir_ret_void();
    fprintf(ll_out, "}\n");
    cur_proc = NULL;
}

/* =========================================================================
 * 9.  RUNTIME DECLARATIONS
 * ========================================================================= */

static void emit_runtime_decls(void) {
    fprintf(ll_out, "; ---- Modula-2 runtime (m2runtime.c) ----\n");
    fprintf(ll_out, "declare void @M2_WriteLn()\n");
    fprintf(ll_out, "declare void @M2_WriteInt(i32)\n");
    fprintf(ll_out, "declare void @M2_WriteCard(i32)\n");
    fprintf(ll_out, "declare void @M2_WriteReal(double)\n");
    fprintf(ll_out, "declare void @M2_WriteChar(i8)\n");
    fprintf(ll_out, "declare void @M2_WriteString(ptr)\n");
    fprintf(ll_out, "declare void @M2_ReadInt(ptr)\n");
    fprintf(ll_out, "declare void @M2_ReadCard(ptr)\n");
    fprintf(ll_out, "declare void @M2_ReadReal(ptr)\n");
    fprintf(ll_out, "declare ptr  @M2_New(i32)\n");
    fprintf(ll_out, "declare void @M2_Dispose(ptr)\n");
    fprintf(ll_out, "declare void @M2_Halt()\n");
    fprintf(ll_out, "declare void @M2_Inc(ptr, i32)\n");
    fprintf(ll_out, "declare void @M2_Dec(ptr, i32)\n");
    fprintf(ll_out, "declare void @M2_Trap(i32)\n");
    fprintf(ll_out, "declare void @M2_Transfer(ptr, ptr)\n");
    fprintf(ll_out, "\n");
}

/* =========================================================================
 * 10.  WITH SYSTEM   (mirrors MCP4EXPR.MOD WithSystem)
 *
 * WITH rec DO body END:
 *   Pass 3 emits a 'field' symbol (val = with-depth) before each field
 *   reference inside the WITH scope.  val=1 → outermost active WITH.
 *
 * We store each record's base address in a dedicated ptr alloca so it
 * survives through all nested statements without re-evaluating the base.
 * ========================================================================= */

#define WITH_MAX 8
typedef struct {
    char  base_alloca[64]; /* alloca ptr holding the record base address */
    Stptr rec_type;        /* Structrec of the record                    */
} WithEntry;

static WithEntry with_stack[WITH_MAX];
static int       with_depth = 0;

static void enter_with(Attr rec_attr) {
    assert(with_depth < WITH_MAX);
    /* Emit a ptr alloca in the *current* position (not entry block) so it
       is visible to the body.  Alloca in LLVM dominates all successors. */
    char slot[64]; snprintf(slot, sizeof slot, "%%with_%d", with_depth);
    fprintf(ll_out, "  %s = alloca ptr\n", slot);
    char base[64]; addr_of(rec_attr, base, sizeof base);
    ir("store ptr %s, ptr %s", base, slot);
    with_stack[with_depth].rec_type = rec_attr.type;
    strncpy(with_stack[with_depth].base_alloca, slot,
            sizeof with_stack[0].base_alloca - 1);
    with_depth++;
}

static void exit_with(void) {
    assert(with_depth > 0);
    with_depth--;
}

/* Return an Attr for the base of WITH level depth_val (1 = outermost). */
static Attr use_with(int32_t depth_val) {
    assert(depth_val >= 1 && depth_val <= with_depth);
    WithEntry* e = &with_stack[depth_val - 1];
    char ptr_reg[64]; new_reg(ptr_reg, sizeof ptr_reg);
    ir("%s = load ptr, ptr %s", ptr_reg, e->base_alloca);
    Attr a; memset(&a, 0, sizeof a);
    a.mode     = ATTR_PTREG;
    a.type     = e->rec_type;
    a.byte_off = 0;
    strncpy(a.reg, ptr_reg, sizeof a.reg - 1);
    return a;
}

/* =========================================================================
 * 10b.  FORWARD DECLARATIONS  (mutual recursion between expr and stmt)
 * ========================================================================= */

static Attr expression(void);
static Attr factor(void);
static Attr term(void);
static Attr simple_expression(void);
static void statement(void);
static void stat_seq(Symbol term1);
static void stat_seq2(Symbol t1, Symbol t2);
static void stat_seq3(Symbol t1, Symbol t2, Symbol t3);
static void block(Idptr proc);

/* =========================================================================
 * 11.  CALL SYSTEM   (replaces MCP4CALL.MOD)
 * ========================================================================= */

/* Map standard procedure names to runtime calls */
static Attr call_standard_pure(Stpures pname, Attr proc_a) {
    (void)proc_a;
    Attr result = {0}; result.type = NULL;
    switch (pname) {
        case incp: {
            /* INC(v) or INC(v, n) */
            get_symbol(); Attr var = expression();
            char ptr[64]; addr_of(var, ptr, sizeof ptr);
            if (sy == comma) {
                get_symbol();
                Attr amt = load_attr(expression());
                char abuf[64]; attr_value_str(amt, abuf, sizeof abuf);
                char args[256]; snprintf(args, sizeof args, "ptr %s, i32 %s", ptr, abuf);
                ir_call_void("@M2_Inc", args);
            } else {
                char args[128]; snprintf(args, sizeof args, "ptr %s, i32 1", ptr);
                ir_call_void("@M2_Inc", args);
            }
            break;
        }
        case decp: {
            get_symbol(); Attr var = expression();
            char ptr[64]; addr_of(var, ptr, sizeof ptr);
            if (sy == comma) {
                get_symbol();
                Attr amt = load_attr(expression());
                char abuf[64]; attr_value_str(amt, abuf, sizeof abuf);
                char args[256]; snprintf(args, sizeof args, "ptr %s, i32 %s", ptr, abuf);
                ir_call_void("@M2_Dec", args);
            } else {
                char args[128]; snprintf(args, sizeof args, "ptr %s, i32 1", ptr);
                ir_call_void("@M2_Dec", args);
            }
            break;
        }
        case halp:
            ir_call_void("@M2_Halt", "");
            unreachable = 1;
            break;
        case newp: {
            get_symbol(); Attr var = expression();
            char ptr_reg[64]; addr_of(var, ptr_reg, sizeof ptr_reg);
            uint32_t sz = var.type ? type_bytes(var.type->ptr.elemp) : 4;
            char args[64]; snprintf(args, sizeof args, "i32 %u", sz);
            char tmp[64]; new_reg(tmp, sizeof tmp);
            ir("%s = call ptr @M2_New(%s)", tmp, args);
            char tbuf[256]; llvm_type(var.type, tbuf, sizeof tbuf);
            ir("store ptr %s, ptr %s", tmp, ptr_reg);
            break;
        }
        case disp: {
            get_symbol(); Attr var = load_attr(expression());
            char vbuf[64]; attr_value_str(var, vbuf, sizeof vbuf);
            char args[128]; snprintf(args, sizeof args, "ptr %s", vbuf);
            ir_call_void("@M2_Dispose", args);
            break;
        }
        case exlp: {
            /* EXCL(s, e): s := s AND NOT (1 << e) */
            get_symbol(); Attr s_a = expression();
            char s_ptr[64]; addr_of(s_a, s_ptr, sizeof s_ptr);
            get_symbol(); /* comma */
            Attr e_a = load_attr(expression());
            char ebuf[64]; attr_value_str(e_a, ebuf, sizeof ebuf);
            char s_v[64]; new_reg(s_v, sizeof s_v);
            ir_load(s_v, s_a.type, s_ptr);
            char bit[64], notb[64], res[64];
            new_reg(bit, sizeof bit); new_reg(notb, sizeof notb); new_reg(res, sizeof res);
            ir("%s = shl i32 1, %s",        bit,  ebuf);
            ir("%s = xor i32 %s, -1",       notb, bit);
            ir_binop(res, "and", s_a.type ? s_a.type : g_cardptr, s_v, notb);
            ir_store(s_a.type, res, s_ptr);
            break;
        }
        case inlp: {
            /* INCL(s, e): s := s OR (1 << e) */
            get_symbol(); Attr s_a = expression();
            char s_ptr[64]; addr_of(s_a, s_ptr, sizeof s_ptr);
            get_symbol(); /* comma */
            Attr e_a = load_attr(expression());
            char ebuf[64]; attr_value_str(e_a, ebuf, sizeof ebuf);
            char s_v[64]; new_reg(s_v, sizeof s_v);
            ir_load(s_v, s_a.type, s_ptr);
            char bit[64], res[64];
            new_reg(bit, sizeof bit); new_reg(res, sizeof res);
            ir("%s = shl i32 1, %s", bit, ebuf);
            ir_binop(res, "or", s_a.type ? s_a.type : g_cardptr, s_v, bit);
            ir_store(s_a.type, res, s_ptr);
            break;
        }
        case trsp:
            /* TRANSFER(VAR p1, p2: PROCESS): coroutine switch — runtime call */
            ir_call_void("@M2_Transfer", ""); /* arguments skipped for now */
            break;
        default:
            break;
    }
    return result;
}

static Attr call_standard_func(Stfuncs fname, Attr proc_a) {
    (void)proc_a;
    Attr result = {0};

    /* ADR, HIGH, SIZE need the unloaded designator (address/type) */
    if (fname == adrf || fname == higf || fname == sizf) {
        get_symbol(); /* consume lparent */
        Attr raw = expression(); /* NOT load_attr — keep as lvalue */
        char dst[64]; new_reg(dst, sizeof dst);
        switch (fname) {
            case adrf: {
                /* ADR(v): address of v as INTEGER (ADDRESS subrange) */
                char ptr[64]; addr_of(raw, ptr, sizeof ptr);
                ir("%s = ptrtoint ptr %s to i32", dst, ptr);
                result.mode = ATTR_REG; result.type = g_addrptr;
                strncpy(result.reg, dst, sizeof result.reg - 1);
                break;
            }
            case higf:
                /* HIGH(a): compile-time upper bound of array index type */
                if (raw.type && raw.type->form == arrays && raw.type->arr.ixp) {
                    result = make_const_attr(g_cardptr,
                                            (int32_t)raw.type->arr.ixp->sr.max);
                } else {
                    result = make_const_attr(g_cardptr, 0);
                }
                break;
            case sizf:
                /* SIZE(T or v): byte size of type */
                result = make_const_attr(g_cardptr, (int32_t)type_bytes(raw.type));
                break;
            default: break;
        }
        return result;
    }

    /* VAL(T, x) — first argument is a type name, second is the value */
    if (fname == valf) {
        get_symbol(); /* consume lparent */
        /* First arg: type identifier — read but only use for type annotation */
        Attr type_arg = expression(); /* namesy of the target type */
        Stptr target_type = type_arg.type;
        get_symbol(); /* comma */
        Attr val_arg = load_attr(expression());
        /* Emit a type coercion (bitcast/trunc/zext as needed) */
        char dst[64]; new_reg(dst, sizeof dst);
        char vbuf[64]; attr_value_str(val_arg, vbuf, sizeof vbuf);
        char ttbuf[64]; llvm_type(target_type, ttbuf, sizeof ttbuf);
        char stbuf[64]; llvm_type(val_arg.type, stbuf, sizeof stbuf);
        if (target_type && val_arg.type && target_type != val_arg.type) {
            /* Primitive coercion: trunc if narrower, zext if wider, bitcast else */
            uint32_t dst_sz = type_bytes(target_type);
            uint32_t src_sz = type_bytes(val_arg.type);
            if      (dst_sz < src_sz) ir("%s = trunc %s %s to %s", dst, stbuf, vbuf, ttbuf);
            else if (dst_sz > src_sz) ir("%s = zext %s %s to %s",  dst, stbuf, vbuf, ttbuf);
            else                       ir("%s = bitcast %s %s to %s", dst, stbuf, vbuf, ttbuf);
            result.mode = ATTR_REG; result.type = target_type;
            strncpy(result.reg, dst, sizeof result.reg - 1);
        } else {
            result = val_arg; /* same representation */
        }
        return result;
    }

    /* All other standard functions: single loaded argument */
    get_symbol(); /* consume lparent */
    Attr arg = load_attr(expression());
    char vbuf[64]; attr_value_str(arg, vbuf, sizeof vbuf);
    char dst[64]; new_reg(dst, sizeof dst);
    switch (fname) {
        case absf: {
            /* ABS(x): x >= 0 ? x : -x */
            char cmp[64]; new_reg(cmp, sizeof cmp);
            char neg[64]; new_reg(neg, sizeof neg);
            if (arg.type == g_realptr) {
                ir("%s = fneg double %s", neg, vbuf);
                ir("%s = fcmp ogt double %s, 0.0", cmp, vbuf);
            } else {
                ir_icmp(cmp, "sge", g_intptr, vbuf, "0");
                ir_binop(neg, "sub", g_intptr, "0", vbuf);
            }
            ir("%s = select i1 %s, i32 %s, i32 %s", dst, cmp, vbuf, neg);
            result.mode = ATTR_REG; result.type = arg.type ? arg.type : g_intptr;
            strncpy(result.reg, dst, sizeof result.reg - 1);
            break;
        }
        case oddf: {
            /* ODD(x): (x AND 1) != 0 */
            char and_dst[64]; new_reg(and_dst, sizeof and_dst);
            ir("%s = and i32 %s, 1", and_dst, vbuf);
            ir_icmp(dst, "ne", g_cardptr, and_dst, "0");
            result.mode = ATTR_REG; result.type = g_boolptr;
            strncpy(result.reg, dst, sizeof result.reg - 1);
            break;
        }
        case ordf:
            /* ORD(x): zero-extend char/bool/enum to i32 */
            if (arg.type == g_charptr)
                ir("%s = zext i8 %s to i32", dst, vbuf);
            else if (arg.type == g_boolptr)
                ir("%s = zext i1 %s to i32", dst, vbuf);
            else
                ir("%s = bitcast i32 %s to i32", dst, vbuf);
            result.mode = ATTR_REG; result.type = g_cardptr;
            strncpy(result.reg, dst, sizeof result.reg - 1);
            break;
        case chrf:
            /* CHR(n): truncate to i8 */
            ir("%s = trunc i32 %s to i8", dst, vbuf);
            result.mode = ATTR_REG; result.type = g_charptr;
            strncpy(result.reg, dst, sizeof result.reg - 1);
            break;
        case fltf:
            /* FLOAT(n): integer to double */
            ir("%s = sitofp i32 %s to double", dst, vbuf);
            result.mode = ATTR_REG; result.type = g_realptr;
            strncpy(result.reg, dst, sizeof result.reg - 1);
            break;
        case trcf:
            /* TRUNC(r): double to integer */
            ir("%s = fptosi double %s to i32", dst, vbuf);
            result.mode = ATTR_REG; result.type = g_intptr;
            strncpy(result.reg, dst, sizeof result.reg - 1);
            break;
        case capf: {
            /* CAP(c): 'a'..'z' → 'A'..'Z' */
            char clo[64], chi[64], cin[64], sub[64];
            new_reg(clo, sizeof clo); new_reg(chi, sizeof chi);
            new_reg(cin, sizeof cin); new_reg(sub, sizeof sub);
            ir("%s = icmp sge i8 %s, 97",  clo, vbuf); /* >= 'a' */
            ir("%s = icmp sle i8 %s, 122", chi, vbuf); /* <= 'z' */
            ir("%s = and i1 %s, %s",       cin, clo, chi);
            ir("%s = sub i8 %s, 32",       sub, vbuf);
            ir("%s = select i1 %s, i8 %s, i8 %s", dst, cin, sub, vbuf);
            result.mode = ATTR_REG; result.type = g_charptr;
            strncpy(result.reg, dst, sizeof result.reg - 1);
            break;
        }
        default:
            result = arg;
            break;
    }
    return result;
}

/* User-defined procedure / function call */
static Attr proc_func_call(Attr proc_a) {
    /* proc_a.mode == ATTR_PROC or a variable holding a procedure pointer */
    Attr result = {0};

    if (!nptr) return result; /* safety */
    Idptr ip = (Idptr)nptr;  /* already set by caller's Designator */

    /* Standard procedures */
    if (ip->proc.isstandard) {
        if (ip->klass == pures)
            return call_standard_pure(ip->proc.pname, proc_a);
        else
            return call_standard_func(ip->proc.fname, proc_a);
    }

    /* Build argument string — VAR params pass a pointer, value params pass value */
    char args_buf[2048] = "";
    Idptr param = ip->idtyp ? ip->idtyp->proc.fstparam : NULL;
    int first = 1;
    /* sy is already lparent; read args until rparent */
    while (sy != rparent && sy != eop) {
        get_symbol();
        if (sy == rparent) break;
        int is_var = param && (param->var.vkind == varparam);
        Attr arg = expression(); /* lvalue Attr from designator */
        if (!first) strncat(args_buf, ", ", sizeof args_buf - strlen(args_buf) - 1);
        if (is_var) {
            /* VAR param: pass pointer to the storage location */
            char ptr[64]; addr_of(arg, ptr, sizeof ptr);
            char pair[320]; snprintf(pair, sizeof pair, "ptr %s", ptr);
            strncat(args_buf, pair, sizeof args_buf - strlen(args_buf) - 1);
        } else {
            Attr val = load_attr(arg);
            char vbuf[64];  attr_value_str(val, vbuf, sizeof vbuf);
            char tbuf[256]; llvm_type(val.type, tbuf, sizeof tbuf);
            char pair[320]; snprintf(pair, sizeof pair, "%s %s", tbuf, vbuf);
            strncat(args_buf, pair, sizeof args_buf - strlen(args_buf) - 1);
        }
        first = 0;
        if (sy == comma) get_symbol();
        param = param ? param->var.vlink : NULL;
    }
    get_symbol(); /* consume rparent */

    char fname[128]; mangle_proc(ip, fname, sizeof fname);

    if (ip->klass == funcs && ip->idtyp && ip->idtyp->proc.rkind == funcs) {
        /* Function: has return value */
        Stptr ret_type = ip->idtyp->proc.funcp;
        char dst[64]; new_reg(dst, sizeof dst);
        ir_call(dst, ret_type, fname, args_buf);
        result.mode = ATTR_REG;
        result.type = ret_type;
        strncpy(result.reg, dst, sizeof result.reg - 1);
    } else {
        /* Procedure: void */
        ir_call_void(fname, args_buf);
    }
    return result;
}

/* =========================================================================
 * 12.  EXPRESSION SYSTEM   (replaces MCP4EXPR.MOD)
 *
 *  Recursive descent mirrors the original MCP4EXPR hierarchy:
 *    expression → simple_expression (rel-op simple_expression)?
 *    simple_expression → ('+' | '-')? term (('+' | '-' | OR) term)*
 *    term → factor (('*' | '/' | DIV | MOD | AND) factor)*
 *    factor → designator | constant | '(' expression ')' | NOT factor
 * ========================================================================= */

static Attr designator(void);

static Attr factor(void) {
    Attr a = {0};
    if (sy == lparent) {
        get_symbol();
        a = expression();
        get_symbol(); /* rparent */
    } else if (sy == notsy) {
        get_symbol();
        a = load_attr(factor());
        char vbuf[64]; attr_value_str(a, vbuf, sizeof vbuf);
        if (a.type && a.type->form == sets) {
            /* Bitwise NOT for sets */
            char dst[64]; new_reg(dst, sizeof dst);
            ir_binop(dst, "xor", a.type, vbuf, "-1");
            a.mode = ATTR_REG; strncpy(a.reg, dst, sizeof a.reg - 1);
        } else {
            /* Boolean NOT: xor i1 x, 1 */
            if (a.mode == ATTR_CONST) { a.cval = !a.cval; }
            else {
                char dst[64]; new_reg(dst, sizeof dst);
                ir("%s = xor i1 %s, 1", dst, vbuf);
                a.mode = ATTR_REG; a.type = g_boolptr;
                strncpy(a.reg, dst, sizeof a.reg - 1);
            }
        }
    } else if (sy == namesy || sy == field) {
        a = designator();
        if (sy == lparent) {
            get_symbol();
            a = proc_func_call(a);
        }
    } else {
        /* Constant (anycon) */
        assert(sy == anycon);
        a.type = cstPtr;
        if (cstPtr && cstPtr->form == reals) {
            a.mode = ATTR_RCONST;
            a.rval = *((double*)cString); /* Lilith stored real in heap */
        } else {
            a.mode = ATTR_CONST;
            a.cval = (int32_t)val;
        }
        get_symbol();
    }
    return a;
}

static Attr term(void) {
    Attr a = factor();
    while (sy >= andsy && sy <= modsy) {
        Symbol op = sy;
        Attr la = load_attr(a);
        char lbuf[64]; attr_value_str(la, lbuf, sizeof lbuf);

        if (op == andsy) {
            /* Short-circuit AND: branch on left operand */
            char lbl_eval[32], lbl_skip[32], lbl_end[32];
            new_label(lbl_eval, sizeof lbl_eval);
            new_label(lbl_skip, sizeof lbl_skip);
            new_label(lbl_end,  sizeof lbl_end);
            ir_condbr(lbuf, lbl_eval, lbl_skip);
            ir_label(lbl_eval);
            get_symbol(); Attr b = load_attr(factor());
            char rbuf[64]; attr_value_str(b, rbuf, sizeof rbuf);
            /* result of AND = b (if we reached here, a was true) */
            ir_br(lbl_end);
            ir_label(lbl_skip);
            ir_br(lbl_end);
            ir_label(lbl_end);
            char dst[64]; new_reg(dst, sizeof dst);
            ir("%s = phi i1 [ %s, %%%s ], [ 0, %%%s ]",
               dst, rbuf, lbl_eval, lbl_skip);
            a.mode = ATTR_REG; a.type = g_boolptr;
            strncpy(a.reg, dst, sizeof a.reg - 1);
            continue;
        }

        get_symbol();
        Attr b = load_attr(factor());
        char rbuf[64]; attr_value_str(b, rbuf, sizeof rbuf);
        char dst[64]; new_reg(dst, sizeof dst);

        int is_real = (a.type == g_realptr || b.type == g_realptr);
        switch (op) {
            case times:
                if (is_real) ir_binop(dst, "fmul", g_realptr, lbuf, rbuf);
                else if (a.type == g_intptr) ir_binop(dst, "mul", g_intptr, lbuf, rbuf);
                else ir_binop(dst, "mul", g_cardptr, lbuf, rbuf);
                break;
            case slash: /* set XOR or float DIV */
                if (a.type && a.type->form == sets) ir_binop(dst, "xor", a.type, lbuf, rbuf);
                else ir_binop(dst, "fdiv", g_realptr, lbuf, rbuf);
                break;
            case divsy:
                if (a.type == g_intptr) ir_binop(dst, "sdiv", g_intptr, lbuf, rbuf);
                else ir_binop(dst, "udiv", g_cardptr, lbuf, rbuf);
                break;
            case modsy:
                if (a.type == g_intptr) ir_binop(dst, "srem", g_intptr, lbuf, rbuf);
                else ir_binop(dst, "urem", g_cardptr, lbuf, rbuf);
                break;
            default: break;
        }
        a.mode = ATTR_REG;
        strncpy(a.reg, dst, sizeof a.reg - 1);
    }
    return a;
}

static Attr simple_expression(void) {
    int negate = (sy == minus);
    if (sy == plus || sy == minus) get_symbol();

    Attr a = term();

    if (negate) {
        a = load_attr(a);
        char vbuf[64]; attr_value_str(a, vbuf, sizeof vbuf);
        char dst[64]; new_reg(dst, sizeof dst);
        if (a.type == g_realptr) ir("%s = fneg double %s", dst, vbuf);
        else if (a.mode == ATTR_CONST) { a.cval = -a.cval; goto done_neg; }
        else ir("%s = sub i32 0, %s", dst, vbuf);
        a.mode = ATTR_REG; strncpy(a.reg, dst, sizeof a.reg - 1);
    }
done_neg:

    while (sy == plus || sy == minus || sy == orsy) {
        Symbol op = sy;

        if (op == orsy) {
            /* Short-circuit OR */
            Attr la = load_attr(a);
            char lbuf[64]; attr_value_str(la, lbuf, sizeof lbuf);
            char lbl_skip[32], lbl_eval[32], lbl_end[32];
            new_label(lbl_eval, sizeof lbl_eval);
            new_label(lbl_skip, sizeof lbl_skip);
            new_label(lbl_end,  sizeof lbl_end);
            ir_condbr(lbuf, lbl_skip, lbl_eval);
            ir_label(lbl_eval);
            get_symbol(); Attr b = load_attr(term());
            char rbuf[64]; attr_value_str(b, rbuf, sizeof rbuf);
            ir_br(lbl_end);
            ir_label(lbl_skip);
            ir_br(lbl_end);
            ir_label(lbl_end);
            char dst[64]; new_reg(dst, sizeof dst);
            ir("%s = phi i1 [ 1, %%%s ], [ %s, %%%s ]",
               dst, lbl_skip, rbuf, lbl_eval);
            a.mode = ATTR_REG; a.type = g_boolptr;
            strncpy(a.reg, dst, sizeof a.reg - 1);
            continue;
        }

        Attr la = load_attr(a);
        char lbuf[64]; attr_value_str(la, lbuf, sizeof lbuf);
        get_symbol();
        Attr b = load_attr(term());
        char rbuf[64]; attr_value_str(b, rbuf, sizeof rbuf);
        char dst[64]; new_reg(dst, sizeof dst);
        int is_real = (a.type == g_realptr || b.type == g_realptr);
        if (op == plus) {
            if (is_real) ir_binop(dst, "fadd", g_realptr, lbuf, rbuf);
            else if (a.type && a.type->form == sets) ir_binop(dst, "or", a.type, lbuf, rbuf);
            else if (a.type == g_intptr) ir_binop(dst, "add", g_intptr, lbuf, rbuf);
            else ir_binop(dst, "add", g_cardptr, lbuf, rbuf);
        } else { /* minus */
            if (is_real) ir_binop(dst, "fsub", g_realptr, lbuf, rbuf);
            else if (a.type && a.type->form == sets) {
                /* set difference: a AND (NOT b) */
                char nb[64]; new_reg(nb, sizeof nb);
                ir_binop(nb, "xor", a.type, rbuf, "-1");
                ir_binop(dst, "and", a.type, lbuf, nb);
            } else if (a.type == g_intptr) ir_binop(dst, "sub", g_intptr, lbuf, rbuf);
            else ir_binop(dst, "sub", g_cardptr, lbuf, rbuf);
        }
        a.mode = ATTR_REG; strncpy(a.reg, dst, sizeof a.reg - 1);
    }
    return a;
}

static Attr expression(void) {
    Attr a = simple_expression();
    if (sy == insy) {
        /* x IN S → (S >> x) AND 1 */
        Attr la = load_attr(a);
        char lbuf[64]; attr_value_str(la, lbuf, sizeof lbuf);
        get_symbol();
        Attr b = load_attr(simple_expression());
        char rbuf[64]; attr_value_str(b, rbuf, sizeof rbuf);
        char shr[64]; new_reg(shr, sizeof shr);
        char mask[64]; new_reg(mask, sizeof mask);
        char cmp[64];  new_reg(cmp,  sizeof cmp);
        ir_binop(shr,  "lshr", g_cardptr, rbuf, lbuf);
        ir_binop(mask, "and",  g_cardptr, shr, "1");
        ir_icmp(cmp,   "ne",   g_cardptr, mask, "0");
        a.mode = ATTR_REG; a.type = g_boolptr;
        strncpy(a.reg, cmp, sizeof a.reg - 1);
        return a;
    }
    if (sy < eql || sy > leq) return a;

    Symbol op = sy;
    Attr la = load_attr(a);
    char lbuf[64]; attr_value_str(la, lbuf, sizeof lbuf);
    get_symbol();
    Attr b = load_attr(simple_expression());
    char rbuf[64]; attr_value_str(b, rbuf, sizeof rbuf);
    char dst[64]; new_reg(dst, sizeof dst);

    int is_real = (a.type == g_realptr);
    int is_uint  = (a.type == g_cardptr);
    const char* pred = "eq";
    if (is_real) {
        switch (op) {
            case eql: pred="oeq"; break; case neq: pred="one"; break;
            case grt: pred="ogt"; break; case geq: pred="oge"; break;
            case lss: pred="olt"; break; case leq: pred="ole"; break;
            default: break;
        }
        ir_fcmp(dst, pred, lbuf, rbuf);
    } else {
        switch (op) {
            case eql: pred="eq";  break;
            case neq: pred="ne";  break;
            case grt: pred = is_uint ? "ugt" : "sgt"; break;
            case geq: pred = is_uint ? "uge" : "sge"; break;
            case lss: pred = is_uint ? "ult" : "slt"; break;
            case leq: pred = is_uint ? "ule" : "sle"; break;
            default: break;
        }
        ir_icmp(dst, pred, a.type, lbuf, rbuf);
    }
    a.mode = ATTR_REG; a.type = g_boolptr;
    strncpy(a.reg, dst, sizeof a.reg - 1);
    return a;
}

/* Designator: resolves namesy/field tokens to an Attr, then applies
   subscript ([]), dereference (^), and field selection (.) */
static Attr designator(void) {
    Attr a = {0};
    if (sy == namesy) {
        a = attr_from_id(nptr, 0 /* cur_level unused: single-process port */);
        get_symbol();
    } else {
        /* sy == field: WITH-statement field access.
           val = WITH depth (1 = outermost active WITH scope in this block).
           Pass 3 resolves which WITH scope owns this field and records it. */
        a = use_with(val);
        get_symbol();
    }

    while (sy == lbrack || sy == arrow || sy == period) {
        if (sy == lbrack) {
            /* Array subscript */
            get_symbol();
            Stptr arr_type = a.type;
            Attr idx = load_attr(expression());
            /* Adjust for non-zero lower bound: effective_idx = raw_idx - lo */
            uint32_t lo = arr_type->arr.ixp->sr.min;
            if (lo != 0) {
                idx = load_attr(idx);
                char ibuf[64]; attr_value_str(idx, ibuf, sizeof ibuf);
                char adj[64]; new_reg(adj, sizeof adj);
                char lo_str[32]; snprintf(lo_str, sizeof lo_str, "%u", lo);
                ir_binop(adj, "sub", g_cardptr, ibuf, lo_str);
                idx.mode = ATTR_REG;
                strncpy(idx.reg, adj, sizeof idx.reg - 1);
            }
            char base[64]; addr_of(a, base, sizeof base);
            char ibuf[64]; attr_value_str(idx, ibuf, sizeof ibuf);
            char gep[64]; new_reg(gep, sizeof gep);
            ir_gep_array(gep, arr_type, base, ibuf);
            a.mode = ATTR_PTREG;
            a.type = arr_type->arr.elp;
            a.byte_off = 0;
            strncpy(a.reg, gep, sizeof a.reg - 1);
            assert(sy == rbrack); get_symbol();
        } else if (sy == arrow) {
            /* Pointer dereference */
            get_symbol();
            a = load_attr(a);
            a.mode    = ATTR_PTREG;
            a.type    = a.type ? a.type->ptr.elemp : NULL;
            a.byte_off = 0;
        } else { /* period → record field selection */
            get_symbol();
            assert(sy == namesy && nptr->klass == fields);
            Idptr fp = nptr;
            char base[64]; addr_of(a, base, sizeof base);
            /* fldaddr is the Lilith word offset (1 word = 2 bytes).
               Byte-offset GEP handles fixed and variant record parts
               without needing to enumerate struct field indices. */
            int32_t byte_off = (int32_t)(fp->fldaddr) * 2;
            char gep[64];
            if (byte_off == 0) {
                strncpy(gep, base, sizeof gep - 1);
            } else {
                new_reg(gep, sizeof gep);
                ir("%s = getelementptr i8, ptr %s, i32 %d", gep, base, byte_off);
            }
            a.mode     = ATTR_PTREG;
            a.type     = fp->idtyp;
            a.byte_off = 0;
            strncpy(a.reg, gep, sizeof a.reg - 1);
            get_symbol();
        }
    }
    return a;
}

/* =========================================================================
 * 13.  STATEMENT SYSTEM   (replaces MCP4MAIN.MOD)
 *
 *  Control flow translation to LLVM IR basic blocks:
 *
 *   IF          → compare + br + phi-free merge via explicit labels
 *   WHILE       → header block + body block + exit block
 *   REPEAT      → body block (condition at bottom)
 *   FOR         → pre-header + cond + body + step + exit
 *   LOOP/EXIT   → body block (EXIT targets a pre-named exit label)
 *   CASE        → switch instruction
 *   WITH        → transparent (adjusts designator base)
 *   RETURN      → ret (value or void)
 *   Assignment  → load rhs, store to lhs alloca
 * ========================================================================= */

/* --- Nested LOOP exit target (mirrors LoopSys in MCP4MAIN.MOD) --- */
static char loop_exit_lbl[32] = "";  /* current innermost LOOP exit */

static void emit_if(void) {
    char end_lbl[32]; new_label(end_lbl, sizeof end_lbl);
    for (;;) {
        Attr cond = load_attr(expression());
        char cbuf[64]; attr_value_str(cond, cbuf, sizeof cbuf);
        char then_lbl[32], next_lbl[32];
        new_label(then_lbl, sizeof then_lbl);
        new_label(next_lbl, sizeof next_lbl);
        ir_condbr(cbuf, then_lbl, next_lbl);

        ir_label(then_lbl);
        stat_seq3(endsy, elsifsy, elsesy);
        if (!unreachable) ir_br(end_lbl);

        ir_label(next_lbl);
        if (sy == endsy) break;
        if (sy == elsesy) { get_symbol(); stat_seq(endsy); break; }
        /* sy == elsifsy */
        get_symbol();
    }
    get_symbol(); /* consume endsy */
    ir_label(end_lbl);
}

static void emit_while(void) {
    char cond_lbl[32], body_lbl[32], exit_lbl[32];
    new_label(cond_lbl, sizeof cond_lbl);
    new_label(body_lbl, sizeof body_lbl);
    new_label(exit_lbl, sizeof exit_lbl);

    ir_br(cond_lbl);
    ir_label(cond_lbl);
    Attr cond = load_attr(expression());
    char cbuf[64]; attr_value_str(cond, cbuf, sizeof cbuf);
    ir_condbr(cbuf, body_lbl, exit_lbl);

    ir_label(body_lbl);
    stat_seq(endsy);
    if (!unreachable) ir_br(cond_lbl);

    get_symbol(); /* endsy */
    ir_label(exit_lbl);
}

static void emit_repeat(void) {
    char body_lbl[32], exit_lbl[32];
    new_label(body_lbl, sizeof body_lbl);
    new_label(exit_lbl, sizeof exit_lbl);

    ir_br(body_lbl);
    ir_label(body_lbl);
    stat_seq(untilsy);
    get_symbol(); /* untilsy */

    Attr cond = load_attr(expression());
    char cbuf[64]; attr_value_str(cond, cbuf, sizeof cbuf);
    ir_condbr(cbuf, exit_lbl, body_lbl);
    ir_label(exit_lbl);
}

static void emit_for(void) {
    /* FOR ctrl := lo TO hi [BY step] DO body END */
    assert(sy == namesy);
    Attr ctrl = attr_from_id(nptr, 0);
    char ctrl_ptr[64]; addr_of(ctrl, ctrl_ptr, sizeof ctrl_ptr);
    get_symbol(); /* comma → becomes */
    get_symbol(); /* lo expression */
    Attr lo = load_attr(expression());
    char lobuf[64]; attr_value_str(lo, lobuf, sizeof lobuf);
    ir_store(ctrl.type, lobuf, ctrl_ptr);

    /* hi */
    get_symbol(); /* tosy */
    Attr hi = load_attr(expression());
    char hi_alloca[64]; new_reg(hi_alloca, sizeof hi_alloca);
    /* cache hi in a local so it is evaluated once */
    fprintf(ll_out, "  %s = alloca i32\n", hi_alloca);
    char hibuf[64]; attr_value_str(hi, hibuf, sizeof hibuf);
    ir("store i32 %s, ptr %s", hibuf, hi_alloca);

    int32_t step = 1;
    int     step_is_const = 1;
    char    step_alloca[64] = "";

    if (sy == bysy) {
        get_symbol();
        Attr st = load_attr(expression());
        if (st.mode == ATTR_CONST) {
            step = st.cval;
        } else {
            step_is_const = 0;
            snprintf(step_alloca, sizeof step_alloca, "%%for_step_%u", lbl_ctr++);
            fprintf(ll_out, "  %s = alloca i32\n", step_alloca);
            char stv[64]; attr_value_str(st, stv, sizeof stv);
            ir("store i32 %s, ptr %s", stv, step_alloca);
        }
    }

    char cond_lbl[32], body_lbl[32], exit_lbl[32];
    new_label(cond_lbl, sizeof cond_lbl);
    new_label(body_lbl, sizeof body_lbl);
    new_label(exit_lbl, sizeof exit_lbl);

    ir_br(cond_lbl);
    ir_label(cond_lbl);
    char cur[64]; new_reg(cur, sizeof cur);
    ir_load(cur, ctrl.type, ctrl_ptr);
    char hiv[64]; new_reg(hiv, sizeof hiv);
    ir("%s = load i32, ptr %s", hiv, hi_alloca);
    char cmp[64]; new_reg(cmp, sizeof cmp);

    if (step_is_const) {
        const char* pred = (step >= 0) ? "sle" : "sge";
        ir_icmp(cmp, pred, g_intptr, cur, hiv);
    } else {
        /* Runtime step: select sle or sge based on step sign */
        char step_v[64]; new_reg(step_v, sizeof step_v);
        ir("%s = load i32, ptr %s", step_v, step_alloca);
        char sgt0[64], cle[64], cge[64];
        new_reg(sgt0, sizeof sgt0);
        new_reg(cle,  sizeof cle);
        new_reg(cge,  sizeof cge);
        ir("%s = icmp sgt i32 %s, 0", sgt0, step_v);
        ir_icmp(cle, "sle", g_intptr, cur, hiv);
        ir_icmp(cge, "sge", g_intptr, cur, hiv);
        ir("%s = select i1 %s, i1 %s, i1 %s", cmp, sgt0, cle, cge);
    }
    ir_condbr(cmp, body_lbl, exit_lbl);

    ir_label(body_lbl);
    stat_seq(endsy);
    /* Increment control variable by step */
    char inc[64]; new_reg(inc, sizeof inc);
    if (step_is_const) {
        char step_str[32]; snprintf(step_str, sizeof step_str, "%d", step);
        ir_binop(inc, "add", ctrl.type, cur, step_str);
    } else {
        char step_v[64]; new_reg(step_v, sizeof step_v);
        ir("%s = load i32, ptr %s", step_v, step_alloca);
        ir_binop(inc, "add", ctrl.type, cur, step_v);
    }
    ir_store(ctrl.type, inc, ctrl_ptr);
    if (!unreachable) ir_br(cond_lbl);

    get_symbol(); /* endsy */
    ir_label(exit_lbl);
}

static void emit_loop(void) {
    char body_lbl[32], exit_lbl[32];
    new_label(body_lbl, sizeof body_lbl);
    new_label(exit_lbl, sizeof exit_lbl);

    char prev_exit[32];
    strncpy(prev_exit, loop_exit_lbl, sizeof prev_exit);
    strncpy(loop_exit_lbl, exit_lbl, sizeof loop_exit_lbl);

    ir_br(body_lbl);
    ir_label(body_lbl);
    stat_seq(endsy);
    if (!unreachable) ir_br(body_lbl);

    get_symbol(); /* endsy */
    ir_label(exit_lbl);
    strncpy(loop_exit_lbl, prev_exit, sizeof loop_exit_lbl);
}

static void emit_case(void) {
    /* CASE sel OF arm1 | arm2 | ... [ELSE body] END
     *
     * Single-pass comparison-chain: for each arm emit icmp/condbr checks
     * that branch to the arm body on match or fall through to the next arm.
     * Each check label is forward-declared; LLVM resolves at function end. */
    Attr sel = load_attr(expression());
    char sbuf[64]; attr_value_str(sel, sbuf, sizeof sbuf);
    get_symbol(); /* ofsy — the "OF" keyword */

    char end_lbl[32]; new_label(end_lbl, sizeof end_lbl);

    /* Single-pass comparison-chain approach:
     * For each arm, emit icmp/condbr checks before the arm body.
     * Each failed check falls through to the next arm's checks.
     * No forward-reference buffering needed. */
    for (;;) {
        if (sy == endsy || sy == eop) break;

        if (sy == elsesy) {
            get_symbol();
            stat_seq(endsy);
            if (!unreachable) ir_br(end_lbl);
            break;
        }

        /* sy is the first anycon token of the current arm's label list */
        char arm_lbl[32]; new_label(arm_lbl, sizeof arm_lbl);

        /* Emit comparison chain; each failed check jumps to the next check */
        while (sy != colon && sy != eop) {
            int32_t lo_v = (int32_t)val;
            get_symbol();           /* advance past lo anycon */
            int32_t hi_v = lo_v;
            if (sy == range) {
                get_symbol();       /* skip range token */
                hi_v = (int32_t)val;
                get_symbol();       /* advance past hi anycon */
            }
            if (sy == comma) get_symbol();

            char chk_next[32]; new_label(chk_next, sizeof chk_next);
            if (lo_v == hi_v) {
                char cmp[64]; new_reg(cmp, sizeof cmp);
                ir("%s = icmp eq i32 %s, %d", cmp, sbuf, lo_v);
                ir_condbr(cmp, arm_lbl, chk_next);
            } else {
                char clo[64], chi[64], cin[64];
                new_reg(clo, sizeof clo);
                new_reg(chi, sizeof chi);
                new_reg(cin, sizeof cin);
                ir("%s = icmp sle i32 %d, %s", clo, lo_v, sbuf);
                ir("%s = icmp sle i32 %s, %d", chi, sbuf, hi_v);
                ir("%s = and i1 %s, %s",       cin, clo, chi);
                ir_condbr(cin, arm_lbl, chk_next);
            }
            ir_label(chk_next);
        }

        /* No label matched — fall through to next arm or else/trap */
        char no_match[32]; new_label(no_match, sizeof no_match);
        ir_br(no_match);

        get_symbol(); /* colon */

        ir_label(arm_lbl);
        stat_seq3(ofsy, elsesy, endsy);
        if (!unreachable) ir_br(end_lbl);

        ir_label(no_match);
        if (sy == ofsy) get_symbol(); /* consume | arm separator */
    }

    if (sy != endsy && sy != eop) {
        ir_call_void("@M2_Trap", "i32 4");
        ir_br(end_lbl);
    }
    get_symbol(); /* endsy */
    ir_label(end_lbl);
}

static void emit_with(void) {
    /* WITH rec DO body END
       1. Evaluate the record designator to get its base address.
       2. Push onto the with-stack (stores base in a ptr alloca).
       3. Process the body — 'field' tokens inside will call use_with().
       4. Pop the with-stack on exit. */
    Attr rec = designator();
    enter_with(rec);
    stat_seq(endsy);
    get_symbol(); /* endsy */
    exit_with();
}

static void emit_return(void) {
    if (cur_proc && cur_proc->klass == funcs &&
        cur_proc->idtyp && cur_proc->idtyp->proc.rkind == funcs) {
        get_symbol(); /* lparent */
        Attr val_a = load_attr(expression());
        char vbuf[64]; attr_value_str(val_a, vbuf, sizeof vbuf);
        get_symbol(); /* rparent */
        ir_ret(cur_proc->idtyp->proc.funcp, vbuf);
    } else {
        ir_ret_void();
    }
}

static void emit_assignment(void) {
    Attr dst = designator();
    get_symbol(); /* expression follows */
    Attr src = expression();
    store_attr(dst, src);
}

static void statement(void) {
    switch (sy) {
        case becomes:   get_symbol(); emit_assignment(); break;
        case call:      get_symbol(); { Attr p = designator(); get_symbol(); proc_func_call(p); } break;
        case ifsy:      get_symbol(); emit_if();     break;
        case casesy:    get_symbol(); emit_case();   break;
        case loopsy:    get_symbol(); emit_loop();   break;
        case whilesy:   get_symbol(); emit_while();  break;
        case repeatsy:  get_symbol(); emit_repeat(); break;
        case withsy:    get_symbol(); emit_with();   break;
        case exitsy:    get_symbol(); ir_br(loop_exit_lbl); break;
        case returnsy:  get_symbol(); emit_return(); break;
        case forsy:     get_symbol(); emit_for();    break;
        default: break; /* empty statement */
    }
}

static void stat_seq(Symbol t1) {
    do { statement(); } while (sy != t1 && sy != eop);
}
static void stat_seq2(Symbol t1, Symbol t2) {
    do { statement(); } while (sy != t1 && sy != t2 && sy != eop);
}
static void stat_seq3(Symbol t1, Symbol t2, Symbol t3) {
    do { statement(); } while (sy != t1 && sy != t2 && sy != t3 && sy != eop);
}

/* =========================================================================
 * 14.  BLOCK TRAVERSAL   (mirrors MCP4MAIN.MOD Block procedure)
 *
 *  The IL1 stream for a block is:
 *    {proceduresy nptr}*    — nested procedures (recursed first)
 *    [codesy]               — CODE procedure body (inline assembly)
 *    [beginsy statseq]      — executable body
 *    endblock
 * ========================================================================= */

static void block(Idptr proc) {
    /* Recurse into nested procedure blocks first */
    while (sy == proceduresy) {
        Idptr nested = nptr;
        get_symbol();
        emit_function_begin(nested);
        block(nested);
        emit_function_end();
    }

    if (proc) emit_function_begin(proc);

    if (sy == codesy) {
        /* CODE procedure: inline M-code body — not translatable to LLVM IR.
           Emit a stub that calls a hand-written C replacement instead. */
        ir("; CODE procedure — provide hand-written replacement");
        ir_ret_void();
        get_symbol();
    } else {
        if (sy == beginsy) {
            get_symbol();
            stat_seq(endblock);
        }
        if (proc && proc->klass == pures && !unreachable)
            ir_ret_void();
    }
    assert(sy == endblock);
    get_symbol(); /* endblock */
}

/* =========================================================================
 * 15.  MODULE-LEVEL GLOBALS
 *
 *  Emitted before any function definitions.
 * ========================================================================= */

static void emit_module_globals(Idptr mod) {
    if (!mod || !mod->proc.globalmodule) return;
    Idptr vp = mod->proc.globvarp;
    while (vp) {
        char gname[128];
        mangle_var_id(vp, gname, sizeof gname);
        char tbuf[256]; llvm_type(vp->idtyp, tbuf, sizeof tbuf);
        fprintf(ll_out, "%s = global %s zeroinitializer\n", gname, tbuf);
        vp = vp->var.vlink;
    }
    fprintf(ll_out, "\n");
}

/* =========================================================================
 * 16.  ENTRY POINT
 * ========================================================================= */

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: mcp4llvm <il1-file> <asc-file> <output.ll>\n");
        fprintf(stderr, "  il1-file  IL1 binary from Pass 3\n");
        fprintf(stderr, "  asc-file  ASCII spelling table from Pass 1\n");
        fprintf(stderr, "  output    LLVM IR text (.ll)\n");
        return 1;
    }

    il1_in = fopen(argv[1], "rb");
    if (!il1_in) { perror(argv[1]); return 1; }

    asc_in = fopen(argv[2], "rb");
    if (!asc_in) { perror(argv[2]); fclose(il1_in); return 1; }

    ll_out = fopen(argv[3], "w");
    if (!ll_out) { perror(argv[3]); fclose(il1_in); fclose(asc_in); return 1; }

    /* Target triple for current host (adjust for cross-compilation) */
    fprintf(ll_out, "; ModuleID = 'modula2'\n");
    fprintf(ll_out, "target datalayout = "
            "\"e-m:e-p270:32:32-p271:32:32-p272:64:64"
            "-i64:64-f80:128-n8:16:32:64-S128\"\n");
    fprintf(ll_out, "target triple = \"x86_64-pc-linux-gnu\"\n\n");

    emit_runtime_decls();

    /* Drive the top-level block (module body, proc == NULL) */
    get_symbol();
    block(NULL);

    fclose(il1_in);
    fclose(ll_out);
    return 0;
}
