# Modula-2 Multi-Pass Compiler (m2cmp)

A self-hosted, 4-pass Modula-2 compiler targeting the **Lilith** computer's M-code instruction set, originally developed at ETH Zurich (~1981–1982). The compiler is written entirely in Modula-2.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Compilation Pipeline](#compilation-pipeline)
3. [Pass 1 — Lexical & Syntax Analysis](#pass-1--lexical--syntax-analysis)
4. [Pass 2 — Declaration Analysis](#pass-2--declaration-analysis)
5. [Pass 3 — Body / Semantic Analysis](#pass-3--body--semantic-analysis)
6. [Pass 4 — Code Generation](#pass-4--code-generation)
7. [Core Data Structures](#core-data-structures)
8. [Symbol File System](#symbol-file-system)
9. [M-Code Instruction Set](#m-code-instruction-set)
10. [Error Handling Strategy](#error-handling-strategy)
11. [Module Index](#module-index)
12. [Compiler Limits & Restrictions](#compiler-limits--restrictions)

---

## Architecture Overview

The compiler follows a classic multi-pass design where each pass reads output produced by the previous one. This separation keeps each pass simple, focused, and independently testable.

```
Source (.MOD / .DEF)
        │
        ▼
┌───────────────────┐
│  Pass 1           │  Lexical scanning + recursive-descent parsing
│  MCP1MAIN.MOD     │──► IL1 file (token stream)
│                   │──► ASCII file (identifier spellings)
└───────────────────┘
        │
        ▼
┌───────────────────┐
│  Pass 2           │  Symbol-table construction + type system
│  MCP2MAIN.MOD     │──► IL2 file (type-annotated IL1)
└───────────────────┘
        │
        ▼
┌───────────────────┐
│  Pass 3           │  Semantic checking + expression analysis
│  MCP3MAIN.MOD     │──► IL1 (rewritten, fully resolved)
└───────────────────┘
        │
        ▼
┌───────────────────┐
│  Pass 4           │  M-code generation
│  MCP4MAIN.MOD     │──► OBJ file (M-code + relocation)
│                   │──► REF file (debug info)
└───────────────────┘
        │
        ▼
  .SYM  .OBJ  .REF  .LST
```

The **MCBase** module coordinates pass scheduling and maintains the global compilation state (`compstat`). Each pass is launched as a separate run of the compiler binary with a different pass selector.

---

## Compilation Pipeline

| Stage  | Main Module    | Reads         | Writes              |
|--------|---------------|---------------|---------------------|
| Pass 1 | MCP1MAIN.MOD  | Source (.MOD) | IL1, ASCII          |
| Pass 2 | MCP2MAIN.MOD  | IL1, ASCII    | IL2                 |
| Pass 3 | MCP3MAIN.MOD  | IL1, IL2      | IL1 (rewritten)     |
| Pass 4 | MCP4MAIN.MOD  | IL1, IL2      | OBJ, REF            |

---

## Pass 1 — Lexical & Syntax Analysis

**Entry point:** `MCP1MAIN.MOD`

### Responsibilities

- Scans source characters and produces a token stream.
- Parses the full Modula-2 grammar using **recursive descent**.
- Resolves keywords and identifiers via hash tables.
- Writes the `IL1` interpass file and the `ASCII` identifier pool.

### Key Modules

| Module        | Role |
|---------------|------|
| `MCP1IO`      | Character-level input; `GetSy()` returns the next symbol; `HashIdent()` maps names to spelling indices (`Spellix`). |
| `MCP1Iden`    | `InitIdTables()` populates hash tables with Modula-2 reserved words and predefined identifiers. |
| `MCP1REAL`    | Parses and stores floating-point literal constants. |

### Parsing Strategy

The parser uses **symbol-set-based error recovery**: every grammar rule receives a `fsys` (follow set) parameter expressed as a `BITSET`. On an unexpected token the parser emits an error, skips tokens until a member of `fsys ∪ first_set` is found, and resumes normally. This allows compilation to continue despite syntax errors.

```
Symset operations
  Set1(sy), Set2(sy1,sy2), Set3(sy1,sy2,sy3) — construct sets
  InSet(sy, s)   — fast membership test
  AddSet, SubSet, InclSet — set arithmetic
```

### Grammar Highlights

| Construct         | Notes |
|-------------------|-------|
| Definition module | `DEFINITION MODULE` … `END`; exports list. |
| Implementation    | `IMPLEMENTATION MODULE` … `END`. |
| Block             | Type / const / var / proc declarations + statement sequence. |
| Types             | Simple (ident, subrange, enum), Array, Record, Set, Pointer, Procedure. |
| Expressions       | Full precedence hierarchy; set constructors `{a, b..c}`. |
| Statements        | `IF/ELSIF/ELSE`, `LOOP/EXIT`, `WHILE`, `REPEAT`, `FOR`, `CASE`, `WITH`, assignment, call. |
| CODE procedures   | Inline octal constants (`0..377B`) for machine code blocks. |

### IL1 File Content

A compact binary token stream containing:

- Symbol kinds (`sy` field)
- Spelling indices (`spix`) for identifiers
- Literal values (integers, reals, strings, sets)
- Structural markers (`endblock`, module/procedure boundaries)

---

## Pass 2 — Declaration Analysis

**Entry point:** `MCP2MAIN.MOD`

### Responsibilities

- Builds the **symbol table** from declarations.
- Resolves types and checks type consistency.
- Handles **separate compilation** (reads/writes `.SYM` symbol files).
- Produces the `IL2` file — IL1 annotated with type and identifier pointers.

### Key Modules

| Module        | Role |
|---------------|------|
| `MCP2IO`      | `GetSy()` reads IL1; `PutSy()` / `PutWord()` write IL2; skip helpers skip over unknown structures. |
| `MCP2IDEN`    | Scope management: `MarkScope` / `ReleaseScope`; `SearchId` / `SearchInBlock` / `ExportSearch`; `MsEntry` registers items in module scope list. |
| `MCP2REFE`    | Forward-reference tracking across modules: `Reference()`, `EndReference()`. |
| `MCSYMFIL`    | Symbol file serialization / deserialization for separate compilation. |

### Symbol Table Entry — `Identrec`

```
Identrec
  name      : Spellix          (* index into ASCII pool *)
  link      : Idptr            (* next in chain *)
  klass     : Idclass          (* const | type | var | field | pure | func | mod | … *)
  globmodp  : Idptr            (* enclosing global module *)

  CASE klass OF
    consts : cvalue: Constval; idtyp: Stptr
    types  : idtyp: Stptr
    vars   : vaddr, vlevel; vkind: Varkind; state: Kindvar
    fields : fldaddr: CARDINAL
    pures/
    funcs  : procnum, plev, varlength, locp: Idptr
           + isstandard, codeproc, codeentry/codelength
    mods   : impp, expp (import/export lists)
           + modulekey[0..2]: CARDINAL  (* version tracking *)
  END
```

### Type Structure — `Structrec`

```
Structrec
  form : Structform   (* enums | bools | chars | ints | cards | words |
                         subranges | reals | pointers | sets |
                         proctypes | arrays | records | hides | opens *)
  size : CARDINAL     (* size in target words *)
  stidp: Idptr        (* defining identifier *)

  CASE form OF
    arrays    : elp (element type), ixp (index type), dyn (dynamic?)
    records   : fieldp (field list), tagp (variant tag)
    proctypes : fstparam (parameter list), rkind, funcp (return type)
    pointers  : elemp (pointed-to type)
    sets      : basep (base type)
    subranges : scalp (base scalar), min, max
    enums     : fcstp (first constant), cstnr (count)
  END
```

### Scope Management

Scopes are maintained as a stack. Each scope corresponds to a procedure or module body:

```
MarkScope(id)    — push new scope
ReleaseScope     — pop scope, resolve pending forward references
SearchId(name)   — linear search from innermost scope outward
```

### Constant Evaluation

`ConstantVal()` evaluates constant expressions at compile time using recursive descent. It supports all Modula-2 constant operators, overflow detection, and type compatibility rules (`intcar` compatibility).

### Module Keys

Each compiled definition module receives a 3-word key (`modulekey[0..2]`). Implementation modules verify their keys match; a mismatch prevents compilation.

---

## Pass 3 — Body / Semantic Analysis

**Entry point:** `MCP3MAIN.MOD`

### Responsibilities

- Validates all **executable code** for semantic correctness.
- Type-checks expressions, assignments, and procedure calls.
- Rewrites the `IL1` file with fully resolved references for Pass 4.

### Key Modules

| Module     | Role |
|------------|------|
| `MCP3IO`   | Reads IL1 + IL2; writes resolved IL1. `InitSave/ResetSave/ReleaseSave` optimize output buffering. |
| `MCP3IDEN` | Scope tracking for bodies: `MarkProcScope/ReleaseProcScope`, `MarkWithScope/ReleaseWithScope`, `FieldIndex`. |

### Expression System

```
Attribut = RECORD
  mode : Attributmode   (* const | var | expr *)
  atp  : Stptr          (* type of expression *)
  aval : Constval       (* value, if mode = const *)
END
```

Expression analysis functions:

| Function             | Description |
|----------------------|-------------|
| `Expression()`       | Full expression (relational operators) |
| `SimpleExpression()` | Additive / unary operators |
| `Term()`             | Multiplicative operators |
| `Factor()`           | Literals, variables, function calls, `(expr)` |
| `Selector()`         | `[]`, `.`, `^` |
| `SetConstructor()`   | `{a, b..c}` |

### Standard Functions & Procedures

| Identifier          | Category |
|---------------------|----------|
| `HIGH`, `SIZE`, `TSIZE`, `ADR` | Intrinsic queries |
| `ODD`, `ABS`, `CAP` | Scalar operations |
| `FLOAT`, `TRUNC`, `ORD`, `CHR`, `VAL` | Type conversions |
| `INC`, `DEC`        | In-place increment / decrement |
| `NEW`, `DISPOSE`    | Dynamic allocation |
| `INCL`, `EXCL`      | Set mutation |
| `NEWPROCESS`, `TRANSFER` | Coroutine support |
| `HALT`              | Abnormal termination |

### Type Compatibility Rules

| Check              | Function        | Used for |
|--------------------|-----------------|----------|
| Expression compat  | `ExprComp()`    | Operands of binary operators |
| Assignment compat  | `AssignComp()`  | Right-hand side vs. variable type |
| ADDRESS compat     | `AddressComp()` | Pointer / ADDRESS coercions |
| Parameter compat   | `ParamCheck()`  | Actual vs. formal parameters |
| Variant analysis   | `VariantAnalyse()` | Record variant sizing |

---

## Pass 4 — Code Generation

**Entry point:** `MCP4MAIN.MOD`

### Responsibilities

- Translates the semantically validated IR into **M-code** for the Lilith virtual machine.
- Manages the expression stack, load modes, and address modes.
- Applies optional **range checking** and **arithmetic overflow checking**.
- Emits OBJ (relocatable M-code) and REF (debug information) files.

### Key Modules

| Module        | Role |
|---------------|------|
| `MCP4GLOB`    | Global state: `loadAddress`, `level`, `loadCount`, `spPosition`, `blockNptr`. |
| `MCP4ATTR`    | Attribute system; load/store code generation; `Load()`, `LoadAddr()`, `Store()`, `Assign()`. |
| `MCP4CODE`    | Low-level emitter: `Emit()`, `Emit2()`, jump patching (`MarkShort/UpdateShort`, `MarkLong/UpdateLong`), string literals, block entry/return. |
| `MCP4EXPR`    | `Designator()`, `Expression()`, `ExpressionAndLoad()`; WITH-statement scope. |
| `MCP4CALL`    | `ProcFuncCall()` — complete call-site code generation. |

### Address Modes — `AtMode`

```
globalMod        — global variable
localMod         — local / parameter variable
loadedMod        — value already on evaluation stack
addrLoadedMod    — address already on evaluation stack
externalMod      — imported variable
indexMod         — array element (word-indexed)
byteIndexMod     — array element (byte-indexed)
doubleIndexMod   — array element (double-word-indexed)
absolutMod       — absolute address
constantMod      — compile-time constant (single word)
doubleConstMod   — compile-time constant (double word / REAL)
stringConstMod   — string literal
procedureMod     — procedure value
illegalMod       — error sentinel
```

### Statement Code Generation

| Statement   | Code pattern |
|-------------|-------------|
| Assignment  | Evaluate RHS → `Load()` → `Store()` to LHS |
| `IF`        | Evaluate condition → conditional forward jump; patch on `END` |
| `CASE`      | `ENTC`/`EXC` jump table; dense packing; range check if enabled |
| `LOOP`      | Save loop PC; `EXIT` generates forward jump patched at `END LOOP` |
| `WHILE`     | Conditional forward jump over body + unconditional backward jump |
| `REPEAT`    | Body then conditional backward jump |
| `FOR`       | `FOR1` (init) + `FOR2` (step+test); handles positive/negative step |
| `WITH`      | `EnterWith()` saves record address; body; `ExitWith()` restores |
| `RETURN`    | Load return value (functions) → `GenBlockReturn()` |

### Jump Optimization

Backward branch distance is estimated before emission:

- **Short jump (`JPB`):** 1-byte signed offset, range −256 … +256.
- **Long jump (`JP`):** 2-word full address.

If the estimate is wrong the emitter iterates until stable.

---

## Core Data Structures

### `Constval` — Compile-Time Value

```
Constval = RECORD
  CASE str: Structform OF
    arrays  : svalue: Stringptr        (* string constant *)
   |reals   : rvalue: POINTER TO REAL  (* double-precision float *)
  ELSE
    value : CARDINAL                   (* integer / boolean / char / set *)
  END
END
```

### `Idclass` — Identifier Classification

```
Idclass = { consts, types, vars, fields, pures, funcs,
            mods, unknown, indrct }
```

### `Varkind` — Parameter Passing Convention

```
Varkind = { noparam, valparam, varparam, copyparam }
```

### `Kindvar` — Variable Storage Class

```
Kindvar = { global, local, absolute, separate }
```

---

## Symbol File System

Symbol files (`.SYM`) enable **separate compilation**: a definition module is compiled once and its type information is saved; implementation modules or client modules read it back without re-parsing the source.

### Symbol File Symbols (`SymFileSymbols`)

```
endfileSS, unitSS, endunitSS,
importSS, exportSS,
constSS, normalconstSS, realconstSS, stringconstSS,
typSS, arraytypSS, recordtypSS, settypSS, pointertypSS, hiddentypSS,
varSS, procSS, funcSS,
identSS,
periodSS, colonSS, rangeSS,
lparentSS, rparentSS, lbracketSS, rbracketSS,
caseSS, ofSS, elseSS, endSS
```

Module keys (`modulekey[0..2]: CARDINAL`) guard against stale symbol files: any mismatch between the stored key and the current compilation aborts with a symbol error (`symerrs`).

---

## M-Code Instruction Set

Selected instructions generated by Pass 4 (see `MCMNEMON.DEF` for full list):

| Category     | Mnemonics |
|--------------|-----------|
| Load         | `LI` (immediate), `LLW` (local word), `LGW` (global word), `LSW` (stack-relative) |
| Store        | `STORE`, `SLW` (store local), `SGW` (store global) |
| Arithmetic   | `ADD`, `SUB`, `MUL`, `DIVV`, `MOD`, `SHL`, `SHR` |
| Logic        | `ANDD`, `ORR`, `XORR`, `NOTT`, `INN` (set membership) |
| Comparison   | `EQL`, `NEQ`, `LSS`, `LEQ`, `GTR`, `GEQ` |
| Jump         | `JP` (unconditional), `JPFC` (jump-on-false), `JPB` (backward) |
| Block memory | `MOV`, `BBLT` (block move) |
| Procedures   | `ENTR` (entry), `RTN` (return), `CL` (call) |
| Loop         | `FOR1` (loop init), `FOR2` (loop step + test) |
| Case         | `ENTC` (case entry), `EXC` (case exit) |
| Runtime      | `TRAP` (runtime check), `CHKS` (range check) |

---

## Error Handling Strategy

| Flag       | Meaning | Effect |
|------------|---------|--------|
| `globerrs` | Unrecoverable global error | Stop entire compilation |
| `passerrs` | Error in current pass | Report and proceed to next pass |
| `symerrs`  | Symbol file inconsistency | Block dependent passes |

Each pass contributes distinct error categories:

- **Pass 1:** Unexpected tokens, missing delimiters (recovered via symbol sets).
- **Pass 2:** Unknown identifiers, type mismatches, scope violations, forward-reference failures.
- **Pass 3:** Expression type errors, invalid assignments, bad actual parameters, illegal standard-procedure usage.
- **Pass 4:** Range violations (emits `CHKS`/`TRAP` if checking enabled); bad code structure.

---

## Module Index

### Compiler Passes

| File             | Description |
|------------------|-------------|
| `MCP1MAIN.MOD`   | Pass 1 top-level: scanner + recursive-descent parser |
| `MCP2MAIN.MOD`   | Pass 2 top-level: declaration analysis + symbol table |
| `MCP3MAIN.MOD`   | Pass 3 top-level: body analysis + semantic checks |
| `MCP4MAIN.MOD`   | Pass 4 top-level: M-code emission |

### Pass 1 Support

| File             | Description |
|------------------|-------------|
| `MCP1IO.DEF/MOD` | Lexical I/O, symbol reading, identifier hashing |
| `MCP1Iden.DEF/MOD` | Keyword and identifier table initialisation |
| `MCP1REAL.DEF/MOD` | Floating-point literal parsing |

### Pass 2 Support

| File              | Description |
|-------------------|-------------|
| `MCP2IO.DEF/MOD`  | IL1 reader + IL2 writer |
| `MCP2IDEN.DEF/MOD`| Scope stack, identifier search, scope mark/release |
| `MCP2REFE.DEF/MOD`| Cross-module forward-reference tracking |

### Pass 3 Support

| File              | Description |
|-------------------|-------------|
| `MCP3IO.DEF/MOD`  | IL reader/writer with save-point optimisation |
| `MCP3IDEN.DEF/MOD`| Body-level scope (proc, module, WITH scopes) |

### Pass 4 Support

| File               | Description |
|--------------------|-------------|
| `MCP4GLOB.DEF/MOD` | Global state variables for code generation |
| `MCP4ATTR.DEF/MOD` | Attribute / address-mode system; load/store helpers |
| `MCP4CODE.DEF/MOD` | Low-level instruction emitter; jump patching; string pool |
| `MCP4EXPR.DEF/MOD` | Expression + designator code generation |
| `MCP4CALL.DEF/MOD` | Procedure / function call code generation |

### Shared Infrastructure

| File                | Description |
|---------------------|-------------|
| `MCBASE.DEF/MOD`    | Core type definitions (`Identrec`, `Structrec`, `Constval`); pass coordinator |
| `MCPUBLIC.DEF/MOD`  | Compilation status flags; interpass file handles |
| `MCSYMFIL.DEF/MOD`  | Symbol file read/write for separate compilation |
| `MCSYM.MOD`         | Symbol table utilities |
| `MCOPERAT.DEF/MOD`  | Operator encoding / decoding |
| `MCMNEMON.DEF/MOD`  | M-code mnemonic definitions |
| `MCFILENA.DEF/MOD`  | File name construction helpers |
| `MCINIT.MOD`        | Module initialisation sequencing |
| `MCQLIST.MOD`       | Queue-list utilities |
| `MCLIST.MOD`        | Linked-list utilities |
| `CONVERSI.DEF/MOD`  | Numeric conversion routines |
| `DECODE.MOD`        | M-code disassembler / decoder |
| `DECOMACH.DEF/MOD`  | Decoding machine support |
| `OPTIONS.DEF/MOD`   | Compiler command-line option parsing |
| `NEWSTREA.DEF/MOD`  | Stream I/O abstraction |
| `WRITESTR.DEF/MOD`  | String output utilities |
| `FILELOOK.DEF/MOD`  | File lookup / search path resolution |
| `FILENAME.DEF/MOD`  | File name manipulation |
| `FILEPOOL.DEF/MOD`  | File handle pool management |
| `parser.mod`        | Standalone parser module |

### Documentation & Grammar

| File          | Description |
|---------------|-------------|
| `GS-M2.bnf`   | BNF grammar for Modula-2 as accepted by this compiler |
| `MODNOTES.TXT`| Development notes |
| `INTERP.DOC`  | Interpreter / M-code documentation |
| `M2M-PC.DOC`  | M2M PC-port documentation |
| `COMP.txt`    | Compilation notes |

---

## Compiler Limits & Restrictions

| Parameter              | Limit |
|------------------------|-------|
| Maximum nesting level  | 15 (`levmax`) |
| Maximum module name    | 24 characters |
| Maximum module priority| 15 |
| Module key width       | 3 × `CARDINAL` |
| Array index type       | `CARDINAL` range |
| Real numbers           | `REAL` (separate from `INTEGER` / `CARDINAL`) |
| CODE procedure opcode  | 0 … 255 (`0..377B`) |

### Supported Language Features

- Separate compilation (`.DEF` + `.MOD` pairs)
- Qualified import / export
- Variant records (tagged discriminated unions)
- Dynamic arrays with dimension information (`HIGH`)
- Nested procedures and nested modules
- `CODE` procedures (inline M-code)
- Full set of standard functions and procedures
- Set types with bitwise operations
- Pointer types with forward references
- Module initialisation sequences
- Coroutine support (`NEWPROCESS`, `TRANSFER`)
- `SYSTEM` module integration (`ADDRESS`, `WORD`, `ADR`, `TSIZE`)
