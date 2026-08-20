/* sav — the VerkOS language.
 *
 * A systems language whose rules come from the defects that have cost this
 * project the most time. Four of them, and each is a rule the compiler
 * enforces rather than a sentence in a document nobody rereads:
 *
 *   1. A claim must depend on what it claims. `requires` and `ensures` are
 *      part of a function's signature, and a clause that would read the same
 *      whether the function worked or not is a compile error. The recurring
 *      defect in this repository's whole log is a report that does not depend
 *      on what it reports; here it does not compile.
 *
 *   2. Nothing reaches the world except through a capability. There is no
 *      inline assembly and no free `write`: a system call can only be made
 *      through a capability value, and capability values are only handed to
 *      `entry fn main` by the compiler-written entry code. What a program can
 *      do is therefore visible in its signature.
 *
 *   3. Capabilities survive compilation. The set of system calls a program can
 *      possibly make is written into the executable as a note, and stam reads
 *      it and refuses the rest. The language's promise stops being a promise
 *      at the kernel boundary and becomes a fact about the process.
 *
 *   4. Device memory is a type, not a qualifier, and an address is never a
 *      constant. `dev *u32` accesses happen exactly as written — one
 *      instruction, the declared width, never merged with its neighbour —
 *      because GCC merging two stores cost this kernel a boot. And no global
 *      initialiser may contain an address, because a table of absolute
 *      pointers is what stopped resolving when the kernel moved to the high
 *      half. savc emits PC-relative addressing only, and the executables it
 *      writes contain zero relocations.
 *
 * The compiler is written in C because nothing else exists yet; it emits an
 * aarch64 ELF executable directly, with no assembler, no linker and no libc
 * between the source and the bytes. Reserved for later: `kvist`.
 */
#ifndef SAV_H
#define SAV_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;

/* ---- diagnostics --------------------------------------------------------
 * Every error names a file, a line and a column, and says what to do. A
 * diagnostic that does not tell the reader what would have been right is the
 * same defect as a report that does not depend on what it reports. */
void die_at(const char *file, int line, int col, const char *fmt, ...);
void note(const char *fmt, ...);
extern const char *src_name;   /* file being compiled, for diagnostics */
extern const char *src_text;   /* its bytes, so a line can be shown */

void *xmalloc(size_t n);
char *xstrndup(const char *s, size_t n);

/* ---- tokens ------------------------------------------------------------ */
typedef enum {
    T_EOF, T_IDENT, T_INT, T_STR, T_CHAR,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK,
    T_COMMA, T_SEMI, T_COLON, T_DOT, T_DOTDOT, T_ARROW,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_BANG,
    T_SHL, T_SHR,
    T_LT, T_LE, T_GT, T_GE, T_EQEQ, T_NE,
    T_ANDAND, T_OROR,
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_AT,
    /* keywords */
    T_FN, T_ENTRY, T_STRUCT, T_CONST, T_VAR, T_LET, T_IF, T_ELSE, T_WHILE,
    T_FOR, T_IN, T_RETURN, T_BREAK, T_CONTINUE, T_NEEDS, T_GIVES,
    T_ASSERT, T_AS, T_DEV, T_TRUE, T_FALSE, T_SIZE, T_USE
} TokKind;

typedef struct Token {
    TokKind kind;
    int line, col;
    char *text;      /* identifiers, string bodies */
    size_t len;      /* for strings, which may contain NUL */
    i64 ival;
} Token;

typedef struct Lexer {
    const char *p;
    int line, col;
    Token tok;       /* current */
    Token ahead;     /* one token of lookahead, valid when have_ahead */
    int have_ahead;
} Lexer;

void lex_init(Lexer *lx, const char *text);
void lex_next(Lexer *lx);
Token *lex_peek(Lexer *lx);
const char *tok_name(TokKind k);

/* ---- types ------------------------------------------------------------- */
typedef enum { TY_VOID, TY_BOOL, TY_INT, TY_PTR, TY_SLICE, TY_ARRAY, TY_STRUCT, TY_CAP } TypeKind;

typedef struct Type Type;
typedef struct Field { char *name; Type *type; int offset; } Field;

struct Type {
    TypeKind kind;
    int size, align;
    /* TY_INT */
    int bits, is_signed;
    /* TY_PTR, TY_SLICE, TY_ARRAY */
    Type *elem;
    int is_dev;            /* pointer into device memory (rule 4) */
    i64 nelem;             /* TY_ARRAY */
    /* TY_STRUCT */
    char *name;
    Field *fields; int nfields;
    /* TY_CAP */
    int cap_id;
};

Type *ty_void(void); Type *ty_bool(void);
Type *ty_int(int bits, int is_signed);
Type *ty_ptr(Type *elem, int is_dev);
Type *ty_slice(Type *elem);
Type *ty_array(Type *elem, i64 n);
const char *ty_str(Type *t);          /* for diagnostics; static buffer per call */
int ty_same(Type *a, Type *b);
int ty_is_int(Type *t);

/* ---- capabilities -------------------------------------------------------
 * A capability is a type whose values only the compiler can make, and whose
 * methods are the only way to reach a system call. The set below is what stam
 * implements; each method names the call it becomes, and the manifest written
 * into the executable is built from exactly these numbers — not from a list
 * kept beside them, which would be a second thing to forget to update. */
typedef struct CapMethod {
    const char *name;
    int sysno;            /* Linux/stam aarch64 call number */
    int nargs;            /* arguments in the source */
    const char *sig;      /* argument shape: 's'=[]u8 'i'=i64 'p'=*u8 'c'=cap */
    char ret;             /* 'i'=i64 'v'=void 's'=[]u8 */
    int fixed_arg;        /* value forced into x0 (fd 1 or 2), or -1 */
} CapMethod;

typedef struct CapDef {
    const char *name;     /* Console, Args, Files, Net, Proc */
    int id;
    CapMethod methods[10];
    int nmethods;
} CapDef;

extern CapDef cap_defs[];
extern int n_cap_defs;
CapDef *cap_by_name(const char *name);
Type *ty_cap(CapDef *d);

/* ---- expressions ------------------------------------------------------- */
typedef enum {
    E_INT, E_BOOL, E_STR, E_VAR, E_CALL, E_METHOD, E_UNARY, E_BINARY,
    E_INDEX, E_FIELD, E_CAST, E_ASSIGN, E_SLICE, E_ADDR, E_DEREF,
    E_SIZE, E_LEN
} ExprKind;

typedef struct Expr Expr;
typedef struct Sym Sym;

struct Expr {
    ExprKind kind;
    int line, col;
    Type *type;             /* filled in by the checker */
    i64 ival;               /* E_INT, E_BOOL */
    char *str; size_t slen; /* E_STR */
    int str_off;            /* offset of the literal in .rodata */
    char *name;             /* E_VAR, E_FIELD, E_CALL, E_METHOD */
    Sym *sym;               /* E_VAR after resolution */
    struct Fn *fn;          /* E_CALL after resolution */
    CapMethod *method;      /* E_METHOD after resolution */
    int op;                 /* TokKind of the operator */
    Expr *a, *b, *c;
    Expr **args; int nargs;
    Field *field;           /* E_FIELD after resolution */
    int is_lvalue;
};

/* ---- statements -------------------------------------------------------- */
typedef enum { S_LET, S_EXPR, S_IF, S_WHILE, S_FOR, S_RETURN, S_BLOCK, S_BREAK, S_CONTINUE, S_ASSERT } StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
    StmtKind kind;
    int line, col;
    /* S_LET */
    char *name; Type *type; Expr *init; Sym *sym; int is_var;
    /* S_IF, S_WHILE, S_RETURN, S_EXPR, S_ASSERT */
    Expr *cond; Expr *expr;
    Stmt *then_s, *else_s, *body;
    /* S_FOR: name over lo..hi */
    Expr *lo, *hi;
    /* S_BLOCK */
    Stmt **stmts; int nstmts;
};

/* ---- symbols ----------------------------------------------------------- */
typedef enum { SYM_LOCAL, SYM_PARAM, SYM_GLOBAL, SYM_CONST, SYM_RESULT } SymKind;
struct Sym {
    SymKind kind;
    char *name;
    Type *type;
    int line, col;
    int offset;        /* locals/params: from x29, negative. globals: .data offset */
    i64 cval;          /* SYM_CONST */
    int reg_index;     /* parameters: which argument register pair */
    struct Sym *next;
};

/* ---- functions --------------------------------------------------------- */
typedef struct Contract {
    Expr *expr;
    int line, col;
    const char *text;     /* the source of the clause, for the failure message */
} Contract;

typedef struct Fn {
    char *name;
    int line, col;
    /* Which file this came from, and its bytes. A diagnostic from the checker
     * or the code generator names the file the function is in, which is not
     * the file savc was invoked on as soon as `use` exists. */
    const char *file;
    const char *text;
    Sym *params; int nparams;
    Type *ret;
    char *ret_name;       /* the returned value's name: `-> n: i64`. A `gives`
                           * clause may only talk about something named. */
    Sym *ret_sym;
    Contract *req; int nreq;
    Contract *ens; int nens;
    Stmt *body;
    int is_entry;
    int frame_size;
    int result_off;       /* where `result` sits while an ensures clause reads it */
    int code_off;         /* filled in by codegen */
    struct Fn *next;
} Fn;

typedef struct Unit {
    Fn *fns;
    Sym *globals;
    Type **structs; int nstructs;
    Fn *entry;
} Unit;

/* Parse `path`, and everything it pulls in with `use`. Every file is parsed
 * once, however many times it is named. */
Unit *parse_unit(const char *path);
void parse_into(Unit *u, const char *path);

/* ---- checking ---------------------------------------------------------- */
void check_unit(Unit *u);

/* ---- code generation --------------------------------------------------- */
typedef struct Buf { u8 *p; size_t n, cap; } Buf;
void buf_put(Buf *b, const void *data, size_t n);
void buf_u32(Buf *b, u32 v);
void buf_align(Buf *b, size_t a);

typedef struct Image {
    Buf text;             /* code, then read-only data */
    Buf data;             /* mutable globals with initialisers */
    size_t bss_size;
    size_t rodata_off;    /* offset of .rodata within text buffer */
    u64 entry;            /* virtual address of _start */
    u64 text_vaddr, data_vaddr;
    u32 *syscalls; int nsyscalls;   /* the manifest: what this program may ask of the kernel */
    const char **capnames; int ncapnames;
} Image;

void gen_unit(Unit *u, Image *img);
void write_elf(Image *img, const char *path);

/* ---- options ----------------------------------------------------------- */
extern int opt_contracts;   /* 1: compile requires/ensures into checks */
extern int opt_verbose;

#endif
