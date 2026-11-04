/*
 * typecheck.c
 *
 * Copyright (c) 2026 Scintillae Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * typecheck.c - Relic type checker and symbol table
 */
#include "tigryx.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#pragma GCC diagnostic ignored "-Wformat-truncation"

/* -------------------------------------------------------------------------
 * Type utilities
 * ---------------------------------------------------------------------- */
static bool type_is_int(Type *t) {
    return t && (t->kind==TY_I8||t->kind==TY_U8||t->kind==TY_I16||
                 t->kind==TY_U16||t->kind==TY_I32||t->kind==TY_U32);
}

/* NOTE: type_is_float means specifically f32, NOT "any floating type".
 * It gates f32 codegen decisions (OP_FADD vs OP_ADD and friends) at ~20
 * call sites, so widening it to include f64 would silently route doubles
 * through the single-precision opcodes. Use type_is_f64 / type_is_fp. */
static bool type_is_float(Type *t) {
    return t && t->kind == TY_F32;
}

static bool type_is_f64(Type *t) {
    return t && t->kind == TY_F64;
}

/* "is some floating-point type" - for questions that genuinely do not
 * care about width (e.g. is this numeric at all). */
static bool type_is_fp(Type *t) {
    return type_is_float(t) || type_is_f64(t);
}

static bool type_is_numeric(Type *t) {
    return type_is_int(t) || type_is_fp(t);
}

static bool RELIC_UNUSED type_is_signed(Type *t) {
    return t && (t->kind==TY_I8||t->kind==TY_I16||t->kind==TY_I32);
}

static int type_size(Type *t) {
    if(!t) return 0;
    switch(t->kind) {
        case TY_BOOL: case TY_I8: case TY_U8: return 1;
        case TY_I16: case TY_U16: return 2;
        case TY_I32: case TY_U32: return 4;
        case TY_F32: return 4;
        case TY_F64: return 8;
        case TY_PTR: case TY_REF: case TY_FNPTR: return 4;
        case TY_ARRAY: return type_size(t->inner) * (int)t->count;
        case TY_STRUCT: return 0;       /* resolved later per-struct */
        default: return 0;
    }
}

static bool type_equal(Type *a, Type *b) {
    if(!a || !b) return a==b;
    if(a->kind != b->kind) return false;
    switch(a->kind) {
        case TY_PTR:
        case TY_REF:   return type_equal(a->inner, b->inner);
        case TY_ARRAY: return a->count==b->count && type_equal(a->inner, b->inner);
        case TY_STRUCT: return strcmp(a->name, b->name)==0;
        case TY_FNPTR: {
            if(a->nparams != b->nparams) return false;
            if(!type_equal(a->inner, b->inner)) return false;
            for(int i=0;i<a->nparams;i++)
                if(!type_equal(a->params[i], b->params[i])) return false;
            return true;
        }
        default: return true;
    }
}

/* True iff `dst` is a [N]u8 array and `src_node` is a string literal,
 * permitting the syntax sugar `var buf: [N]u8 = "hello"` (definition
 * only, never assignment). The IR emitter handles the actual byte copy. */
static bool is_string_array_init(Type *dst, Node *src_node) {
    if(!dst || !src_node) return false;
    if(dst->kind != TY_ARRAY) return false;
    if(!dst->inner || dst->inner->kind != TY_U8) return false;
    return src_node->kind == ND_STRING_LIT;
}

static bool type_assignable(Type *dst, Type *src) {
    if(type_equal(dst, src)) return true;
    /* integer widening: any int -> any int (compiler emits cast) */
    if(type_is_int(dst) && type_is_int(src)) return true;
    /* bool <-> int */
    if(dst->kind==TY_BOOL && type_is_int(src)) return true;
    /* *T -> *u8 implicit */
    if(dst->kind==TY_PTR && dst->inner && dst->inner->kind==TY_U8 &&
       src->kind==TY_PTR) return true;
    /* ref<T> -> ptr<T> (widening: lose non-null guarantee, allow arith) */
    if(dst->kind==TY_PTR && src->kind==TY_REF &&
       type_equal(dst->inner, src->inner)) return true;
    /* ref<T> -> *u8 implicit (same escape-hatch as ptr<T> -> *u8) */
    if(dst->kind==TY_PTR && dst->inner && dst->inner->kind==TY_U8 &&
       src->kind==TY_REF) return true;
    /* ref<T> -> ref<T> already covered by type_equal. There is
     * deliberately no implicit ptr<T> -> ref<T> rule: that would erase
     * the non-null guarantee. Promote with `cast(ref T, p)` after a
     * null check. */
    return false;
}

/* Contextual literal typing for f64. An unsuffixed float literal keeps
 * its exact double in Node.fval, so in an f64 context it IS an f64 -
 * retyping it is literal typing, not an implicit f32->f64 promotion of a
 * stored value (those still require cast(f64, x)). Returns true if src
 * was retyped. `cast(f64, 0.1)` deliberately does NOT take this path -
 * it converts the f32 value, giving the widened (inexact) result. */

/* True for expression nodes whose value is derived entirely from float
 * literals: a literal itself, a unary +/- over one, or a float builtin
 * call (@sqrt(4.0)) whose every argument is likewise literal-derived.
 * The last case matters for `deq(@sqrt(2.0), ...)` - the builtin's own
 * 'F' check defaults it to f32, but with all-literal inputs it carries
 * no stored-f32 value and can adopt the f64 context wholesale. */
static bool is_adoptable_lit(Node *n) {
    if(!n) return false;
    if(n->kind == ND_FLOAT_LIT) return true;
    if(n->kind == ND_UNOP && (n->op == TK_MINUS || n->op == TK_PLUS))
        return is_adoptable_lit(n->left);
    if(n->kind == ND_BUILTIN_CALL && n->type &&
       n->type->kind == TY_F32 && n->nchildren > 0) {
        for(int i=0;i<n->nchildren;i++)
            if(!is_adoptable_lit(n->children[i])) return false;
        return true;
    }
    /* All-literal float arithmetic adopts wholesale, so
     * `var d: f64 = 1.0/3.0` divides in double precision rather than
     * computing f32 and widening. Restricted to arithmetic ops -
     * comparisons produce bool, which is not float-derived. */
    if(n->kind == ND_BINOP && n->type && n->type->kind == TY_F32 &&
       (n->op == TK_PLUS || n->op == TK_MINUS ||
        n->op == TK_STAR || n->op == TK_SLASH)) {
        return is_adoptable_lit(n->left) && is_adoptable_lit(n->right);
    }
    return false;
}
static void adopt_lit_rec(Node *n, Type *dst) {
    if(!n) return;
    if(n->kind == ND_FLOAT_LIT) { n->type = dst; return; }
    if(n->kind == ND_UNOP) { adopt_lit_rec(n->left, dst); n->type = dst; return; }
    if(n->kind == ND_BINOP) {
        adopt_lit_rec(n->left, dst);
        adopt_lit_rec(n->right, dst);
        n->type = dst;
        return;
    }
    if(n->kind == ND_BUILTIN_CALL) {
        for(int i=0;i<n->nchildren;i++) adopt_lit_rec(n->children[i], dst);
        n->type = dst;
    }
}
static bool adopt_f64_lit(Type *dst, Node *src) {
    if(dst && dst->kind==TY_F64 && src && is_adoptable_lit(src)) {
        adopt_lit_rec(src, dst);
        return true;
    }
    return false;
}

static Type *type_alloc(TypeKind k) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = k;
    return t;
}

static Type *type_ptr(Type *inner) {
    Type *t = type_alloc(TY_PTR);
    t->inner = inner;
    return t;
}

static Type *type_ref(Type *inner) {
    Type *t = type_alloc(TY_REF);
    t->inner = inner;
    return t;
}

/* -------------------------------------------------------------------------
 * Symbol table
 * ---------------------------------------------------------------------- */
static void sym_push_scope(SymTable *st) { st->scope++; }

static void sym_pop_scope(SymTable *st) {
    /* Remove all symbols at current scope */
    int s = st->scope;
    int n = st->nsyms;
    while(n > 0 && st->syms[n-1].scope == s) n--;
    st->nsyms = n;
    st->scope--;
}

Symbol *sym_lookup(SymTable *st, const char *name) {
    for(int i = st->nsyms-1; i >= 0; i--)
        if(strcmp(st->syms[i].name, name)==0) return &st->syms[i];
    return NULL;
}

/* Look up a name only among scope-0 (global) symbols. Scans forwards from
 * the start of the symbol table so that the original global definition
 * is found even if a later (popped) scope re-used the name. */
Symbol *sym_lookup_global(SymTable *st, const char *name) {
    for(int i = 0; i < st->nsyms; i++) {
        if(st->syms[i].scope != 0) continue;
        if(strcmp(st->syms[i].name, name) == 0) return &st->syms[i];
    }
    return NULL;
}

static Symbol *sym_define(SymTable *st, const char *name, SymKind kind, Type *type, int line) {
    if(st->nsyms >= RELIC_MAX_SYMBOLS) {
        snprintf(st->err, sizeof(st->err),
                 "line %d: symbol table full (limit %d). Raise RELIC_MAX_SYMBOLS in include/tigryx.h and rebuild.",
                 line, RELIC_MAX_SYMBOLS);
        return NULL;
    }
    Symbol *s = &st->syms[st->nsyms++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%.*s", RELIC_MAX_IDENT - 1, name);
    s->kind   = kind;
    s->type   = type;
    s->scope  = st->scope;
    s->offset = 0;
    s->fn_id  = -1;
    return s;
}

/* -------------------------------------------------------------------------
 * Error helper
 * ---------------------------------------------------------------------- */
/* `node` is the AST node the error is attributed to (always `n` or `d` at
 * every call site below) - taking the node rather than a bare line number
 * means every typecheck error carries file identity too, without having
 * to plumb a second argument through 44 call sites individually. */
#define TC_ERR(tc, node, ...) do { \
    char _tc_msg[220]; \
    snprintf(_tc_msg, sizeof(_tc_msg), __VA_ARGS__); \
    snprintf((tc)->err, sizeof((tc)->err), "line %d: %s", (node)->line, _tc_msg); \
    (tc)->err_line = (node)->line; \
    (tc)->err_file = (node)->file_id; \
    (tc)->err_col  = (node)->col; \
    return false; \
} while(0)

#define TC_ERR_NULL(tc, node, ...) do { \
    char _tc_msg[1024]; \
    snprintf(_tc_msg, sizeof(_tc_msg), __VA_ARGS__); \
    snprintf((tc)->err, sizeof((tc)->err), "line %d: %s", (node)->line, _tc_msg); \
    (tc)->err_line = (node)->line; \
    (tc)->err_file = (node)->file_id; \
    (tc)->err_col  = (node)->col; \
    return NULL; \
} while(0)

/* -------------------------------------------------------------------------
 * Struct layout helper
 * ---------------------------------------------------------------------- */
/* tc-aware type size that resolves struct layouts recursively. Needed
 * so that field offsets for nested structs (e.g. Rect { top_left:Point,
 * bottom_right:Point }) are computed correctly. */
static int tc_type_size(TypeChecker *tc, Type *t) {
    if(!t) return 0;
    if(t->kind == TY_STRUCT) {
        for(int i=0;i<tc->nstructs;i++) {
            if(strcmp(tc->structs[i].name, t->name) != 0) continue;
            Node *decl = tc->structs[i].decl;
            int sz = 0;
            for(int j=0;j<decl->nchildren;j++)
                sz += tc_type_size(tc, decl->children[j]->type);
            return sz;
        }
        return 0;
    }
    if(t->kind == TY_ARRAY) return tc_type_size(tc, t->inner) * (int)t->count;
    return type_size(t);
}

static int struct_field_offset(TypeChecker *tc, const char *struct_name, const char *field, Type **out_type) {
    for(int i=0;i<tc->nstructs;i++) {
        if(strcmp(tc->structs[i].name, struct_name)!=0) continue;
        Node *decl = tc->structs[i].decl;
        int off = 0;
        for(int j=0;j<decl->nchildren;j++) {
            Node *f = decl->children[j];
            if(strcmp(f->sval, field)==0) {
                if(out_type) *out_type = f->type;
                return off;
            }
            off += tc_type_size(tc, f->type);
        }
        return -1;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Native class support
 * ---------------------------------------------------------------------- */

/* Find a registered struct/class declaration by name. */
static Node *find_struct_decl(TypeChecker *tc, const char *name) {
    for(int i=0;i<tc->nstructs;i++)
        if(strcmp(tc->structs[i].name, name)==0) return tc->structs[i].decl;
    return NULL;
}

/* The class declaration registered under `name`, or NULL when the name
 * is a plain struct (or unknown). Classes share the structs[] table;
 * the decl's node kind is the only difference. */
static Node *find_class_decl(TypeChecker *tc, const char *name) {
    Node *d = find_struct_decl(tc, name);
    return (d && d->kind == ND_CLASS_DECL) ? d : NULL;
}

/* Search a class's members[] for a method by source-level name (the
 * "name" part of the desugared Class__name impl name). With is_ctor
 * set only constructors match; with it clear only ordinary methods
 * match - constructors and ordinary methods live in separate namespaces.
 * A stored impl name may carry a `$arity` overload suffix by the time
 * this runs, so the comparison is on the base name only. */
static Node *class_find_member(Node *cls, const char *name, int is_ctor) {
    if(!cls) return NULL;
    size_t nl = strlen(name);
    for(int i = 0; i < cls->nmembers; i++) {
        Node *m = cls->members[i];
        const char *p = m->sval + strlen(cls->sval) + 2;
        if(((m->mflags & ND_MF_CTOR) != 0) != is_ctor) continue;
        if(strncmp(p, name, nl) == 0 && (p[nl] == '\0' || p[nl] == '$'))
            return m;
    }
    return NULL;
}

/* The class a desugared method node belongs to, or NULL for an ordinary
 * function. Methods carry ->owner; a name-mangled `Class__m` written by
 * hand in a .tp resolves through the class's members[] too. */
static Node *method_of_class(TypeChecker *tc, Node *fn) {
    if(fn->owner) return fn->owner;
    const char *sep = strstr(fn->sval, "__");
    if(!sep || sep == fn->sval) return NULL;
    char cname[RELIC_MAX_IDENT];
    size_t cn = (size_t)(sep - fn->sval);
    if(cn >= RELIC_MAX_IDENT) return NULL;
    memcpy(cname, fn->sval, cn); cname[cn] = 0;
    Node *cls = find_class_decl(tc, cname);
    if(!cls) return NULL;
    char mname[RELIC_MAX_IDENT];
    snprintf(mname, sizeof(mname), "%s", sep + 2);
    char *dollar = strchr(mname, '$');
    if(dollar) *dollar = 0;
    return (class_find_member(cls, mname, 0) ||
            class_find_member(cls, mname, 1)) ? cls : NULL;
}

/* Allocate a synthetic node for a typecheck-time AST rewrite (method
 * call desugaring). Standalone heap nodes, matching alloc_node's
 * individually-allocated layout in parser.c. */
static Node *tc_new_node(NodeKind k, const Node *ref) {
    Node *n = calloc(1, sizeof(Node));
    if(!n) return NULL;
    n->kind = k;
    if(ref) {
        n->line    = ref->line;
        n->file_id = ref->file_id;
        n->col     = ref->col;
    }
    return n;
}

/* -------------------------------------------------------------------------
 * Out-of-class method definitions
 *
 * A member declared `fn m(...);` inside the class (ND_MF_PROTO) lives only
 * in cls->members[]. A `fn C.m(...) { }` definition (ND_MF_OUTCLASS) is a
 * top-level program child whose ->owner is resolved here. Binding records
 * the definition on the proto's ->right (free on ND_FN_DECL), which is
 * also how duplicate definitions and never-defined protos are detected.
 * ---------------------------------------------------------------------- */

/* The source-level method name of a desugared member fn: ->sval minus
 * the `Class__` prefix and any `$arity` overload suffix. */
static const char *member_src_name(Node *cls, Node *m) {
    return m->sval + strlen(cls->sval) + 2;
}

static bool member_name_eq(const char *a, const char *b) {
    size_t al = 0, bl = 0;
    while(a[al] && a[al] != '$') al++;
    while(b[bl] && b[bl] != '$') bl++;
    return al == bl && strncmp(a, b, al) == 0;
}

/* Param types (injected `self` included) + return type, so a definition
 * must spell the same signature its in-class declaration used. */
static bool fn_sig_equal(Node *a, Node *b) {
    if(a->nchildren != b->nchildren) return false;
    for(int i=0;i<a->nchildren;i++)
        if(!type_equal(a->children[i]->type, b->children[i]->type))
            return false;
    return type_equal(a->type, b->type);
}

/* Bind one `fn C.m` definition to its in-class declaration. Ctor-ness is
 * part of the match (a ctor def only binds ctor declarations). */
static bool bind_outclass_method(TypeChecker *tc, Node *fn) {
    const char *sep = strstr(fn->sval, "__");
    if(!sep || sep == fn->sval)
        TC_ERR(tc, fn, "malformed method name '%s'", fn->sval);
    char cname[RELIC_MAX_IDENT], mname[RELIC_MAX_IDENT];
    size_t cn = (size_t)(sep - fn->sval);
    if(cn >= RELIC_MAX_IDENT) TC_ERR(tc, fn, "method name too long");
    memcpy(cname, fn->sval, cn); cname[cn] = 0;
    snprintf(mname, sizeof(mname), "%s", sep + 2);
    char *dollar = strchr(mname, '$');
    if(dollar) *dollar = 0;

    Node *cls = find_class_decl(tc, cname);
    if(!cls)
        TC_ERR(tc, fn, "fn %s.%s: '%s' is not a class", cname, mname, cname);

    int is_ctor = (fn->mflags & ND_MF_CTOR) != 0;
    Node *name_hit = NULL;
    for(int i=0;i<cls->nmembers;i++) {
        Node *m = cls->members[i];
        if(((m->mflags & ND_MF_CTOR) != 0) != is_ctor) continue;
        if(!member_name_eq(member_src_name(cls, m), mname)) continue;
        if(!name_hit) name_hit = m;
        if(!fn_sig_equal(m, fn)) continue;
        if(!(m->mflags & ND_MF_PROTO))
            TC_ERR(tc, fn, "method '%s.%s' is already defined in the "
                   "class body (line %d)", cname, mname, m->line);
        if(m->right)
            TC_ERR(tc, fn, "method '%s.%s' is already defined "
                   "(first at line %d)", cname, mname, m->right->line);
        m->right  = fn;
        fn->owner = cls;
        return true;
    }
    if(name_hit)
        TC_ERR(tc, fn, "definition of '%s.%s' does not match the "
               "declaration at line %d", cname, mname, name_hit->line);
    TC_ERR(tc, fn, "fn %s.%s: '%s' is not a declared member of '%s'",
           cname, mname, mname, cname);
}

/* For each class: an in-class method body satisfies a matching `;`
 * declaration, and every `;` declaration must end up defined somewhere.
 * Runs after bind_outclass_method has claimed the out-of-class defs. */
static bool resolve_class_members(TypeChecker *tc, Node *cls) {
    for(int i=0;i<cls->nmembers;i++) {
        Node *m = cls->members[i];
        if(m->mflags & ND_MF_PROTO) continue;
        for(int j=0;j<cls->nmembers;j++) {
            Node *pr = cls->members[j];
            if(pr == m || !(pr->mflags & ND_MF_PROTO)) continue;
            if(((m->mflags ^ pr->mflags) & ND_MF_CTOR)) continue;
            if(!member_name_eq(member_src_name(cls, pr),
                               member_src_name(cls, m))) continue;
            if(fn_sig_equal(pr, m) && !pr->right)
                pr->right = m;
        }
    }
    for(int i=0;i<cls->nmembers;i++) {
        Node *m = cls->members[i];
        if((m->mflags & ND_MF_PROTO) && !m->right)
            TC_ERR(tc, m, "method '%s.%s' is declared but never defined",
                   cls->sval, member_src_name(cls, m));
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Forward declaration for mutual recursion
 * ---------------------------------------------------------------------- */
static Type *tc_expr(TypeChecker *tc, Node *n);
static bool  tc_stmt(TypeChecker *tc, Node *n);
static bool  tc_decl(TypeChecker *tc, Node *n);

/* -------------------------------------------------------------------------
 * Constant folding
 *
 * Attempts to evaluate `n` as a compile-time constant expression. On success
 * returns true and writes the folded value's i32 bit-pattern into *out. Float
 * constants store their IEEE-754 bits via reinterpretation, matching how
 * sym->const_val is consumed by the emitter (OP_LDC32 of the raw bits). On
 * failure (the node is not a constant expression — a call, builtin call,
 * deref, address-of, field access, index, string literal, or a reference to
 * a symbol that is not a foldable SYM_CONST) returns false and leaves *out
 * untouched.
 *
 * Must be called AFTER tc_expr(n) has run so n->type is populated. The
 * arithmetic / bitwise / shift / cast semantics mirror emit_expr in ir.c and
 * the VM op implementations in vm.c, so the folded value matches what the VM
 * would compute at runtime.
 * ---------------------------------------------------------------------- */
/* Narrow an integer value to a sub-word type, matching the VM's TRUNC/SX/ZX
 * sequence used by ND_CAST. Sign-extends for signed targets, zero-extends
 * for unsigned, and normalises bool to 0/1. */
static i32 fold_narrow_int(i32 v, Type *t) {
    if(!t) return v;
    switch(t->kind) {
        case TY_BOOL:  return v != 0 ? 1 : 0;
        case TY_I8:    return (i32)(i8)(v & 0xFF);
        case TY_U8:    return (i32)(u8)(v & 0xFF);
        case TY_I16:   return (i32)(i16)(v & 0xFFFF);
        case TY_U16:   return (i32)(u16)(v & 0xFFFF);
        default:       return v;   /* i32/u32/f32-bits/ptr: no narrowing */
    }
}

bool relic_fold_const(TypeChecker *tc, Node *n, i32 *out) {
    if(!n) return false;
    SymTable *st = &tc->syms;

    /* f64 values do not fit the i32 fold channel - refuse rather than
     * silently fold the low word. (const_val / *out are 32-bit.) */
    if(n->type && n->type->kind == TY_F64) return false;

    switch(n->kind) {
        case ND_INT_LIT:
        case ND_CHAR_LIT:
            *out = n->ival;
            return true;

        case ND_BOOL_LIT:
            *out = n->bval ? 1 : 0;
            return true;

        case ND_FLOAT_LIT: {
            /* Const-folds to an f32 bit pattern; n->fval is a double, so
             * the narrowing is deliberate (this is the f32 path). */
            union { float f; i32 i; } cv;
            cv.f = (float)n->fval;
            *out = cv.i;
            return true;
        }

        case ND_IDENT: {
            Symbol *s = sym_lookup(st, n->sval);
            if(s && s->kind == SYM_CONST && s->has_const_val) {
                *out = s->const_val;
                return true;
            }
            return false;
        }

        case ND_UNOP: {
            i32 a;
            if(!relic_fold_const(tc, n->left, &a)) return false;
            bool is_float = n->left->type && n->left->type->kind == TY_F32;
            switch(n->op) {
                case TK_MINUS:
                    if(is_float) {
                        union { float f; i32 i; } cv; cv.i = a;
                        cv.f = -cv.f; *out = cv.i;
                    } else {
                        *out = -a;
                    }
                    return true;
                case TK_TILDE:
                    *out = ~a;
                    return true;
                case TK_BANG:
                    *out = (a == 0) ? 1 : 0;
                    return true;
                default:
                    /* TK_STAR (deref) / TK_AMPERSAND (addr-of): not foldable */
                    return false;
            }
        }

        case ND_BINOP: {
            i32 a, b;
            if(!relic_fold_const(tc, n->left, &a)) return false;
            if(!relic_fold_const(tc, n->right, &b)) return false;
            Type *lt = n->left->type;
            bool is_float = lt && lt->kind == TY_F32;
            bool is_unsigned = lt && (lt->kind == TY_U8 || lt->kind == TY_U16 ||
                                      lt->kind == TY_U32);
            switch(n->op) {
                case TK_PLUS: {
                    if(is_float) {
                        union { float f; i32 i; } x, y, z;
                        x.i = a; y.i = b; z.f = x.f + y.f; *out = z.i;
                    } else *out = a + b;
                    return true;
                }
                case TK_MINUS: {
                    if(is_float) {
                        union { float f; i32 i; } x, y, z;
                        x.i = a; y.i = b; z.f = x.f - y.f; *out = z.i;
                    } else *out = a - b;
                    return true;
                }
                case TK_STAR: {
                    if(is_float) {
                        union { float f; i32 i; } x, y, z;
                        x.i = a; y.i = b; z.f = x.f * y.f; *out = z.i;
                    } else *out = a * b;
                    return true;
                }
                case TK_SLASH: {
                    if(is_float) {
                        if(b == 0) return false;   /* leave to runtime trap */
                        union { float f; i32 i; } x, y, z;
                        x.i = a; y.i = b; z.f = x.f / y.f; *out = z.i;
                    } else {
                        /* OP_DIV uses signed C division; matches vm.c. */
                        if(b == 0) return false;
                        *out = a / b;
                    }
                    return true;
                }
                case TK_PERCENT: {
                    if(b == 0) return false;
                    *out = a % b;
                    return true;
                }
                case TK_AMPERSAND: *out = a & b; return true;
                case TK_PIPE:      *out = a | b; return true;
                case TK_CARET:     *out = a ^ b; return true;
                case TK_LSHIFT:    *out = a << (b & 31); return true;
                /* OP_SHR is a logical (unsigned) shift in the VM regardless of
                 * operand signedness, so fold the same way. */
                case TK_RSHIFT:    *out = (i32)((u32)a >> (b & 31)); return true;
                case TK_EQ:
                    if(is_float) {
                        union { float f; i32 i; } x, y; x.i = a; y.i = b;
                        *out = (x.f == y.f) ? 1 : 0;
                    } else *out = (a == b) ? 1 : 0;
                    return true;
                case TK_NEQ:
                    if(is_float) {
                        union { float f; i32 i; } x, y; x.i = a; y.i = b;
                        *out = (x.f != y.f) ? 1 : 0;
                    } else *out = (a != b) ? 1 : 0;
                    return true;
                case TK_LT:
                    if(is_float) {
                        union { float f; i32 i; } x, y; x.i = a; y.i = b;
                        *out = (x.f <  y.f) ? 1 : 0;
                    } else if(is_unsigned) *out = ((u32)a <  (u32)b) ? 1 : 0;
                    else                   *out = (a <  b) ? 1 : 0;
                    return true;
                case TK_LE:
                    if(is_float) {
                        union { float f; i32 i; } x, y; x.i = a; y.i = b;
                        *out = (x.f <= y.f) ? 1 : 0;
                    } else if(is_unsigned) *out = ((u32)a <= (u32)b) ? 1 : 0;
                    else                   *out = (a <= b) ? 1 : 0;
                    return true;
                case TK_GT:
                    if(is_float) {
                        union { float f; i32 i; } x, y; x.i = a; y.i = b;
                        *out = (x.f >  y.f) ? 1 : 0;
                    } else if(is_unsigned) *out = ((u32)a >  (u32)b) ? 1 : 0;
                    else                   *out = (a >  b) ? 1 : 0;
                    return true;
                case TK_GE:
                    if(is_float) {
                        union { float f; i32 i; } x, y; x.i = a; y.i = b;
                        *out = (x.f >= y.f) ? 1 : 0;
                    } else if(is_unsigned) *out = ((u32)a >= (u32)b) ? 1 : 0;
                    else                   *out = (a >= b) ? 1 : 0;
                    return true;
                case TK_AND: *out = ((a != 0) && (b != 0)) ? 1 : 0; return true;
                case TK_OR:  *out = ((a != 0) || (b != 0)) ? 1 : 0; return true;
                default: return false;
            }
        }

        case ND_CAST: {
            i32 a;
            if(!relic_fold_const(tc, n->left, &a)) return false;
            Type *src = n->left->type;
            bool dst_f = n->type && n->type->kind == TY_F32;
            bool src_f = src && src->kind == TY_F32;
            if(dst_f && !src_f) {
                /* int -> f32 (OP_I2F) */
                union { float f; i32 i; } cv; cv.f = (float)a; *out = cv.i;
                return true;
            } else if(!dst_f && src_f) {
                /* f32 -> int (OP_F2I truncates toward zero), then narrow */
                union { float f; i32 i; } cv; cv.i = a;
                i32 t = (i32)cv.f;
                *out = fold_narrow_int(t, n->type);
                return true;
            } else {
                /* int -> int narrowing (or no-op) */
                *out = fold_narrow_int(a, n->type);
                return true;
            }
        }

        default:
            /* ND_STRING_LIT, ND_CALL, ND_BUILTIN_CALL, ND_INDEX,
             * ND_FIELD_ACCESS, ND_RANGE: not compile-time constants. */
            return false;
    }
}

/* -------------------------------------------------------------------------
 * Expression type-checking
 * ---------------------------------------------------------------------- */
static Type *tc_expr(TypeChecker *tc, Node *n) {
    if(!n) return NULL;
    SymTable *st = &tc->syms;

    switch(n->kind) {

        case ND_INT_LIT:
        case ND_CHAR_LIT: {
            Type *t = type_alloc(TY_I32);
            n->type = t;
            return t;
        }

        case ND_FLOAT_LIT: {
            /* A 'd'-suffixed literal arrives pre-typed f64 by the parser;
             * an f64 context may also have retyped it already. Otherwise
             * unsuffixed literals are f32. */
            if(!n->type) n->type = type_alloc(TY_F32);
            return n->type;
        }

        case ND_BOOL_LIT: {
            Type *t = type_alloc(TY_BOOL);
            n->type = t;
            return t;
        }

        case ND_STRING_LIT: {
            /* String literals have type *u8. */
            Type *t = type_ptr(type_alloc(TY_U8));
            n->type = t;
            return t;
        }

        case ND_IDENT: {
            Symbol *sym = sym_lookup(st, n->sval);
            /* `class C implicit`: inside one of its methods, a bare
             * identifier naming a field resolves through `self` unless a
             * local (param/var/loop var - any scope > 0) shadows it.
             * Checked before the undefined-symbol error: member names
             * never appear in the symbol table at all. Method names only
             * self-qualify in call position, handled in ND_CALL. */
            if(tc->cur_class && tc->cur_class->bval &&
               (!sym || sym->scope == 0)) {
                Type *ft = NULL;
                int off = struct_field_offset(tc, tc->cur_class->sval,
                                              n->sval, &ft);
                if(off >= 0) {
                    Node *selfid = tc_new_node(ND_IDENT, n);
                    if(!selfid) TC_ERR_NULL(tc, n, "out of memory");
                    memcpy(selfid->sval, "self", 5);
                    if(!tc_expr(tc, selfid)) return NULL;
                    n->kind = ND_FIELD_ACCESS;
                    n->left = selfid;
                    n->type = ft;
                    n->ival = off;
                    return ft;
                }
            }
            if(!sym) TC_ERR_NULL(tc, n, "undefined identifier '%s'", n->sval);
            n->type = sym->type;
            return sym->type;
        }

        case ND_CAST: {
            Type *src = tc_expr(tc, n->left);
            if(!src) return NULL;
            /* cast is always allowed between scalars (incl. f32) and pointers.
             * cast(ref T, p) is the explicit (currently unchecked) way to
             * promote a ptr to a ref; users are expected to have verified
             * non-null first. A future `@as_ref(p)` builtin will trap if
             * the source is zero. */
            if(!type_is_int(n->type) && n->type->kind!=TY_PTR &&
               n->type->kind!=TY_REF &&
               n->type->kind!=TY_BOOL && n->type->kind!=TY_U8 &&
               n->type->kind!=TY_F32 && n->type->kind!=TY_F64 &&
               n->type->kind!=TY_FNPTR) {
                TC_ERR_NULL(tc, n, "invalid cast target type");
            }
            return n->type;
        }

        case ND_UNOP: {
            Type *inner = tc_expr(tc, n->left);
            if(!inner) return NULL;
            switch(n->op) {
                case TK_MINUS:
                    if(!type_is_numeric(inner)) TC_ERR_NULL(tc, n, "unary '-' requires numeric operand");
                    n->type = inner;
                    return inner;
                case TK_TILDE:
                    if(!type_is_int(inner)) TC_ERR_NULL(tc, n, "unary '~' requires integer");
                    n->type = inner;
                    return inner;
                case TK_BANG: {
                    Type *t = type_alloc(TY_BOOL);
                    n->type = t;
                    return t;
                }
                case TK_STAR: /* deref */
                    if(inner->kind != TY_PTR && inner->kind != TY_REF)
                        TC_ERR_NULL(tc, n, "dereference of non-pointer");
                    n->type = inner->inner;
                    return inner->inner;
                case TK_AMPERSAND: /* address-of -- always non-null since
                    * the operand is an lvalue with a real address, so we
                    * give it the safer ref<T> type. ref<T> implicitly
                    * widens to ptr<T> via type_assignable, so existing
                    * code that expects *T from &x continues to compile. */
                    n->type = type_ref(inner);
                    return n->type;
                default:
                    TC_ERR_NULL(tc, n, "unknown unary op");
            }
        }

        case ND_BINOP: {
            Type *l = tc_expr(tc, n->left);
            Type *r = tc_expr(tc, n->right);
            if(!l || !r) return NULL;
            switch(n->op) {
                /* arithmetic */
                case TK_PLUS: case TK_MINUS: case TK_STAR:
                case TK_SLASH:
                    if(l->kind == TY_F64 || r->kind == TY_F64) {
                        /* f64 requires both operands f64 - no implicit
                         * promotion of a stored f32/int. Float literals
                         * adopt the width (their value is exact). */
                        adopt_f64_lit(r, n->left);
                        adopt_f64_lit(l, n->right);
                        if(n->left->type->kind != TY_F64 ||
                           n->right->type->kind != TY_F64)
                            TC_ERR_NULL(tc, n, "mixed f64 arithmetic requires explicit cast");
                        n->type = n->left->type;
                        return n->type;
                    }
                    if(type_is_float(l) || type_is_float(r)) {
                        if(l->kind != TY_F32 || r->kind != TY_F32)
                            TC_ERR_NULL(tc, n, "mixed float/int arithmetic requires explicit cast");
                        n->type = l;
                        return l;
                    }
                    if(!type_is_int(l) && l->kind!=TY_PTR)
                        TC_ERR_NULL(tc, n, "arithmetic requires integer or pointer");
                    n->type = l;
                    return l;
                case TK_PERCENT:
                    if(!type_is_int(l)) TC_ERR_NULL(tc, n, "'%%' requires integer");
                    n->type = l;
                    return l;
                /* bitwise */
                case TK_AMPERSAND: case TK_PIPE: case TK_CARET:
                case TK_LSHIFT: case TK_RSHIFT:
                    if(!type_is_int(l)) TC_ERR_NULL(tc, n, "bitwise op requires integer");
                    n->type = l;
                    return l;
                /* comparison -> bool */
                case TK_EQ: case TK_NEQ: case TK_LT: case TK_LE:
                case TK_GT: case TK_GE: {
                    if(l->kind == TY_F64 || r->kind == TY_F64) {
                        adopt_f64_lit(r, n->left);
                        adopt_f64_lit(l, n->right);
                        if(n->left->type->kind != TY_F64 ||
                           n->right->type->kind != TY_F64)
                            TC_ERR_NULL(tc, n, "mixed f64 comparison requires explicit cast");
                    } else if((type_is_float(l) || type_is_float(r)) &&
                              (l->kind != TY_F32 || r->kind != TY_F32))
                        TC_ERR_NULL(tc, n, "mixed float/int comparison requires explicit cast");
                    Type *t = type_alloc(TY_BOOL);
                    n->type = t;
                    return t;
                }
                /* logical -> bool */
                case TK_AND: case TK_OR: {
                    Type *t = type_alloc(TY_BOOL);
                    n->type = t;
                    return t;
                }
                default:
                    TC_ERR_NULL(tc, n, "unknown binary op");
            }
        }

        case ND_INDEX: {
            Type *base = tc_expr(tc, n->left);
            Type *idx  = tc_expr(tc, n->right);
            if(!base || !idx) return NULL;
            if(!type_is_int(idx)) TC_ERR_NULL(tc, n, "array index must be integer");
            if(base->kind == TY_ARRAY) {
                n->type = base->inner;
                return base->inner;
            }
            if(base->kind == TY_PTR || base->kind == TY_REF) {
                n->type = base->inner;
                return base->inner;
            }
            TC_ERR_NULL(tc, n, "subscript of non-array/pointer");
        }

        case ND_FIELD_ACCESS: {
            Type *base = tc_expr(tc, n->left);
            if(!base) return NULL;
            /* auto-deref pointer or reference */
            if(base->kind == TY_PTR || base->kind == TY_REF) base = base->inner;
            if(base->kind != TY_STRUCT)
                TC_ERR_NULL(tc, n, "field access on non-struct");
            Type *ft = NULL;
            int off = struct_field_offset(tc, base->name, n->sval, &ft);
            if(off < 0) TC_ERR_NULL(tc, n, "unknown field '%s' in struct '%s'", n->sval, base->name);
            n->type = ft;
            n->ival = off; /* store byte offset for IR emitter */
            return ft;
        }

        case ND_NEW: {
            /* `new Class(args)` -> a direct call of the desugared
             * constructor fn Class__new. The unmangled name is used so
             * the `name$arity` overload pre-resolution below picks the
             * right overload (ctors can be overloaded by arity). */
            Node *cls = find_class_decl(tc, n->sval);
            if(!cls)
                TC_ERR_NULL(tc, n, "'%s' is not a class", n->sval);
            if(!class_find_member(cls, "new", 1))
                TC_ERR_NULL(tc, n, "class '%s' has no constructor", n->sval);
            char cname[RELIC_MAX_IDENT];
            int rc = snprintf(cname, sizeof(cname), "%s__new", n->sval);
            if(rc <= 0 || rc >= (int)sizeof(cname))
                TC_ERR_NULL(tc, n, "class name too long");
            Node *callee = tc_new_node(ND_IDENT, n);
            if(!callee) TC_ERR_NULL(tc, n, "out of memory");
            memcpy(callee->sval, cname, strlen(cname) + 1);
            n->kind = ND_CALL;
            n->left = callee;
            /* FALLTHROUGH - the node is now an ordinary ND_CALL */
        }

        case ND_CALL: {
            /* Implicit member call inside a `class C implicit` method:
             * a bare `m(args)` where m is a method of C and no local
             * shadows it becomes `self.m(args)` - resolved by the
             * field-access method dispatch right below. Method names
             * only qualify in call position (a bare method name in value
             * position stays a global, matching emit_body_implicit). */
            if(n->left->kind == ND_IDENT && tc->cur_class &&
               tc->cur_class->bval) {
                Symbol *ls = sym_lookup(st, n->left->sval);
                if((!ls || ls->scope == 0) &&
                   class_find_member(tc->cur_class, n->left->sval, 0)) {
                    Node *fa     = tc_new_node(ND_FIELD_ACCESS, n->left);
                    Node *selfid = tc_new_node(ND_IDENT, n->left);
                    if(!fa || !selfid) TC_ERR_NULL(tc, n, "out of memory");
                    memcpy(selfid->sval, "self", 5);
                    memcpy(fa->sval, n->left->sval, strlen(n->left->sval) + 1);
                    fa->left = selfid;
                    n->left = fa;
                }
            }

            /* Method dispatch: `recv.m(args)`. Resolved here, where the
             * receiver's real type is known. The callee becomes the
             * unmangled Class__m name (so overload pre-resolution below
             * still picks the right arity, self included) and the
             * receiver is prepended to the argument list - by reference
             * for value receivers, as-is for pointer/receivers. When
             * `m` is not a method of the receiver's class the callee is
             * left alone so a fn-ptr field call (`x.fp(args)`) still
             * typechecks normally. */
            if(n->left->kind == ND_FIELD_ACCESS) {
                Node *fa = n->left;
                Type *rt = tc_expr(tc, fa->left);
                if(!rt) return NULL;
                const char *sname = NULL;
                int by_ptr = 0;
                if(rt->kind == TY_STRUCT) sname = rt->name;
                else if((rt->kind == TY_PTR || rt->kind == TY_REF) &&
                        rt->inner && rt->inner->kind == TY_STRUCT) {
                    sname = rt->inner->name; by_ptr = 1;
                }
                if(sname) {
                    Node *cls = find_class_decl(tc, sname);
                    if(cls && class_find_member(cls, fa->sval, 0)) {
                        char mname[RELIC_MAX_IDENT];
                        int rc = snprintf(mname, sizeof(mname), "%s__%s",
                                          sname, fa->sval);
                        if(rc <= 0 || rc >= (int)sizeof(mname))
                            TC_ERR_NULL(tc, n, "method name too long");
                        Node *callee = tc_new_node(ND_IDENT, fa);
                        Node *recv   = fa->left;
                        if(!callee) TC_ERR_NULL(tc, n, "out of memory");
                        memcpy(callee->sval, mname, strlen(mname) + 1);
                        if(!by_ptr) {
                            int lval = recv->kind == ND_IDENT ||
                                       recv->kind == ND_INDEX ||
                                       recv->kind == ND_FIELD_ACCESS ||
                                       (recv->kind == ND_UNOP &&
                                        recv->op == TK_STAR);
                            if(!lval)
                                TC_ERR_NULL(tc, n,
                                    "method call on '%s' requires an "
                                    "addressable receiver", sname);
                            Node *addr = tc_new_node(ND_UNOP, fa);
                            if(!addr) TC_ERR_NULL(tc, n, "out of memory");
                            addr->op   = TK_AMPERSAND;
                            addr->left = recv;
                            recv = addr;
                        }
                        Node **nc = calloc((size_t)n->nchildren + 1,
                                           sizeof(Node*));
                        if(!nc) TC_ERR_NULL(tc, n, "out of memory");
                        nc[0] = recv;
                        memcpy(&nc[1], n->children,
                               (size_t)n->nchildren * sizeof(Node*));
                        n->children = nc;
                        n->nchildren++;
                        n->left = callee;
                    }
                }
            }

            /* Overload pre-resolution: if the callee is a bare identifier
             * whose unmangled name is not a SYM_FN (because it has been
             * mangled by arity), rewrite n->left->sval to the matching
             * `name$nargs` BEFORE evaluating it. Without this, tc_expr on
             * the identifier would fail with "undefined identifier" and we
             * would never reach the regular overload resolution below. */
            if(n->left->kind == ND_IDENT) {
                Symbol *probe = sym_lookup(st, n->left->sval);
                if(!probe || probe->kind != SYM_FN) {
                    char mangled[RELIC_MAX_IDENT];
                    int rc = snprintf(mangled, sizeof(mangled), "%s$%d",
                                       n->left->sval, n->nchildren);
                    if(rc > 0 && rc < (int)sizeof(mangled)) {
                        Symbol *msym = sym_lookup(st, mangled);
                        if(msym && msym->kind == SYM_FN) {
                            snprintf(n->left->sval, RELIC_MAX_IDENT, "%s", mangled);
                        }
                    }
                }
            }

            /* n->left is the callee expression */
            Type *callee_type = tc_expr(tc, n->left);
            if(!callee_type) {
                /* If the original (unmangled) name has any overloads, emit a
                 * friendlier error than "undefined identifier". */
                if(n->left->kind == ND_IDENT) {
                    char base[RELIC_MAX_IDENT + 8]; /* Extra space for "$" suffix */
                    snprintf(base, sizeof(base), "%s$", n->left->sval);
                    int nfound = 0;
                    char alts[256]; alts[0] = '\0';
                    for(int si = 0; si < tc->syms.nsyms; si++) {
                        Symbol *s2 = &tc->syms.syms[si];
                        if(s2->kind != SYM_FN) continue;
                        if(strncmp(s2->name, base, strlen(base)) != 0) continue;
                        if(s2->type && s2->type->kind == TY_FNPTR) {
                            char tmp[32];
                            snprintf(tmp, sizeof(tmp), "%s%d",
                                     nfound ? ", " : "", s2->type->nparams);
                            size_t left = sizeof(alts) - strlen(alts) - 1;
                            strncat(alts, tmp, left);
                            nfound++;
                        }
                    }
                    if(nfound > 0) {
                        TC_ERR_NULL(tc, n,
                            "no overload of '%s' takes %d argument(s); available: [%s]",
                            n->left->sval, n->nchildren, alts);
                    }
                }
                return NULL;
            }

            Type *ret = NULL;
            int expected_params = 0;
            Type **param_types  = NULL;

            if(callee_type->kind == TY_FNPTR) {
                ret = callee_type->inner;
                expected_params = callee_type->nparams;
                param_types     = callee_type->params;
            } else {
                /* direct call: left must be ND_IDENT referring to a function */
                if(n->left->kind != ND_IDENT)
                    TC_ERR_NULL(tc, n, "call of non-function");
                Symbol *sym = sym_lookup(st, n->left->sval);
                if(!sym || sym->kind != SYM_FN)
                    TC_ERR_NULL(tc, n, "'%s' is not a function", n->left->sval);
                /* function type stored as FNPTR in sym->type */
                if(sym->type->kind == TY_FNPTR) {
                    ret = sym->type->inner;
                    expected_params = sym->type->nparams;
                    param_types     = sym->type->params;
                } else {
                    ret = sym->type;
                }
            }

            if(n->nchildren != expected_params)
                TC_ERR_NULL(tc, n, "wrong number of arguments: expected %d, got %d",
                            expected_params, n->nchildren);
            for(int i=0;i<n->nchildren;i++) {
                Type *at = tc_expr(tc, n->children[i]);
                if(!at) return NULL;
                if(param_types && !type_assignable(param_types[i], at) &&
                   !adopt_f64_lit(param_types[i], n->children[i]))
                    TC_ERR_NULL(tc, n, "argument %d type mismatch", i+1);
            }
            n->type = ret ? ret : type_alloc(TY_VOID);
            return n->type;
        }

        case ND_BUILTIN_CALL: {
            /* Type-check children expressions first; we'll inspect their
             * resolved types below. */
            Type *arg_types[16] = {0};
            /* Set to TY_F64 when any 'F'-signature argument is f64 - the
             * call is then an f64 overload and its float result is f64. */
            TypeKind call_fw = TY_VOID;
            for(int i=0;i<n->nchildren;i++) {
                Type *at = tc_expr(tc, n->children[i]);
                if(!at) return NULL;
                if(i < 16) arg_types[i] = at;
            }

            /* ----- Builtin signature table ------------------------------
             * Per-slot expected category. The runtime accepts a fairly
             * loose set of types (an integer slot is happy with any int
             * width; a pointer slot accepts pointers, arrays, and strings
             * since all decay to a pointer in memory). The categories
             * below match those rules so the typechecker doesn't reject
             * valid programs.
             *
             *   I = any integer (incl. bool)
             *   F = f32
             *   P = pointer / array / string (anything that is a pointer
             *       to memory at the ABI level)
             *   X = any (skipped — used for slots whose semantics depend
             *       on context, e.g. @mesh's index slot which may legally
             *       be a ptr or 0)
             *
             * arity == -1 means "variadic, skip arity AND type checks"
             * (used by @sprint, the printf-equivalent).
             *
             * @tone is overloaded (1, 2, or 3 args, all integers) and is
             * handled as a special case below the table. */
            typedef struct { const char *name; int arity; const char *sig; } BTSig;
            static const BTSig sigs[] = {
                /* graphics */
                {"window",      -1, NULL},   /* 4 ints (x,y,w,h) or 5 ints (...,flags) — see special case below */
                {"clear",        1, "P"},
                /* x,y,tex,sx,sy,sw,sh,dst_w,dst_h,angle(f32),&tint(RGBA).
                 * src_w/src_h crop the source texture (0 = whole texture);
                 * dst_w/dst_h independently set the on-screen quad size
                 * (0 = same as src_w/src_h), so a texture can be sampled at
                 * its native resolution while being drawn at any size —
                 * needed so HD-upscaled textures don't get UV-cropped when
                 * drawn at their logical (pre-upscale) display size. */
                {"sprite",      11, "IIIIIIIIIFP"},
                {"plot",         3, "IIP"},
                {"flip",         0, ""},
                {"line",         6, "IIIIPI"},
                {"polygon",      3, "PIP"},
                {"polyline",     4, "PIPI"},
                {"ellipse",              8, "IIIIIPII"},
                {"ellipse_filled",       9, "IIIIIPIIP"},
                {"rounded_rectangle",        7, "IIIIIPI"},
                {"rounded_rectangle_filled", 9, "IIIIIPIPI"},
                /* @palette_upload(ptr-or-0, count): ptr slot is X because
                 * count==0 (flat-palette mode) legally passes 0/no pointer. */
                {"palette_upload", 2, "XI"},
                /* Sector-based world renderer (allegro-fixed-dos only). */
                {"wr_palette",   3, "PPI"},
                {"wr_texture",   4, "IIIP"},
                {"wr_flat",      2, "IP"},
                {"wr_test",      3, "III"},
                {"wr_map",       1, "P"},
                {"wr_render",    5, "FFFFF"},
                {"wr_texmap",    4, "PIPI"},
                {"wr_sky",       1, "I"},
                {"wr_flatshade", 5, "IIIII"},
                {"wr_patch",     8, "IIIIIIPI"},
                {"wr_sprite",    2, "PI"},
                {"wr_blockmap",  6, "PIIIII"},
                {"wr_blocked",   4, "IIII"},
                {"wr_blocked_line", 5, "IIIII"},
                {"camera",       6, "PPPFFF"},
                {"texture",      3, "PII"},
                {"light",        3, "PPP"},
                {"fog",          4, "IFFX"},  /* mode(i32), near(f32), far(f32), color(*RGBA or 0) */
                /* @mesh: verts, vcount, idx-or-0, icount, tex_id, rgba.
                 * idx and tex_id slots accept either pointer or integer
                 * (0 = none), so they're marked X. */
                {"mesh",         6, "PIXIXP"},
                {"text",         8, "PIIIIPIP"},
                {"viewport",     4, "IIII"},
                /* @text_measure(text, size, weight, font, out: *TextMetrics) */
                {"text_measure", 5, "PIIPP"},
                {"clip",         4, "IIII"},
                /* @view(x,y,w,h) - render-surface rect for embedded
                 * guests (glViewport for the 3D path); w<=0||h<=0 resets */
                {"view",         4, "IIII"},
                /* @view_capture(x,y,w,h) snapshots that framebuffer
                 * rect; @view_present draws the retained frame scaled
                 * into the given rect. */
                {"view_capture", 4, "IIII"},
                {"view_present", 4, "IIII"},
                /* @draw_list(buf: *u8, len: i32) - retained primitive
                 * buffer; record format in
                 * backends/common/drawlist/drawlist.h */
                {"draw_list",    2, "PI"},
                /* @draw_mode(mode) - blend mode for 2D draws:
                 * 0 = copy, 1 = xor. Returns whether the backend
                 * honoured it (see OP_GFX_DRAW_MODE). */
                {"draw_mode",    1, "I"},
                /* @lines3d(walls,nw,cam_x,cam_z,gs,gc,eye,out,cap,mode)
                 * -> i32 line count, or -1 when unsupported (bytecode
                 * fallback). mode&1 = 5-word records with colour.
                 * See OP_GFX_LINES3D. */
                {"lines3d",     10, "PIIIIIIPII"},
                /* @grid3d(map,mw,mh,cam_x,cam_z,gs,gc,out,cap,mode)
                 * -> i32 columns drawn | 0x40000000, or -1 when
                 * unsupported (bytecode fallback). See OP_GFX_GRID3D. */
                {"grid3d",      10, "PIIIIIIPII"},
                /* input */
                {"key_pressed",  0, ""},
                {"key_held",     1, "I"},
                {"mouse",        1, "P"},
                /* system */
                {"rand",         0, ""},
                {"millis",       0, ""},
                {"exit",         1, "I"},
                {"console",      1, "P"},
                {"snprint",    -1, NULL},   /* variadic: (n, s, fmt, args...) */
                {"assert",      -1, NULL},   /* loose: implementation-defined */
                {"alloc",        1, "I"},
                {"free",         1, "P"},
                {"memcpy",       3, "PPI"},
                {"memset",       3, "PII"},
                {"file_open",    2, "PI"},
                {"file_read",    3, "IPI"},
                {"file_write",   3, "IPI"},
                {"file_close",   1, "I"},
                {"file_seek",    3, "III"},
                {"file_length",  1, "I"},
                {"file_directory",3, "PPI"},
                {"file_path",    2, "PI"},
                {"file_load_png",4, "PPIP"},
                {"file_delete",  1, "P"},
                {"file_mkdir",   1, "P"},
                {"clipboard_set",2, "PI"},
                {"clipboard_get",2, "PI"},
                {"args",         2, "IP"},
                {"is_number",    1, "F"},
                {"sin",          1, "F"},
                {"cos",          1, "F"},
                {"sqrt",         1, "F"},
                {"exp",          1, "F"}, {"log",   1, "F"},
                {"log10",        1, "F"}, {"pow",   2, "FF"},
                {"atan",         1, "F"}, {"atan2", 2, "FF"},
                {"tan",          1, "F"}, {"asin",  1, "F"},
                {"acos",         1, "F"}, {"floor", 1, "F"},
                {"ceil",         1, "F"}, {"round", 1, "F"},
                {"fmod",         2, "FF"},{"fabs",  1, "F"},
                {"sinh",         1, "F"}, {"cosh",  1, "F"},
                {"tanh",         1, "F"},
                {"platform",     1, "P"},
                /* web / http — headers & body slots are X so callers may pass
                 * 0 (no headers / no body) or a pointer. */
                {"http_request", 5, "IPXXI"},
                {"http_poll",    1, "I"},
                {"http_status",  1, "I"},
                {"http_read",    3, "IPI"},
                {"http_close",   1, "I"},
                {"unzip",        2, "PP"},
                {"register",     3, "IPP"},
                /* subprocess management — poll-based, non-blocking.
                 * cwd slot is X so callers may pass 0 (inherit current
                 * directory) or a pointer, like @http_request's headers. */
                {"process_spawn",      2, "PX"},
                {"process_poll",       1, "I"},
                {"process_exit_code",  1, "I"},
                {"process_read_stdout",3, "IPI"},
                {"process_read_stderr",3, "IPI"},
                {"process_kill",       1, "I"},
                {"process_close",      1, "I"},
                /* tiny socket server (TIGRYX_EXPERIMENTAL_SERVER). p is
                 * X so callers may pass 0 (no buffer / no bind addr). */
                {"serve",              4, "IIXI"},
                /* headers/body slots are X: 0 or a pointer. */
                {"serve_reply",        5, "IIXXI"},
                /* embedded-host OS syscalls (OP_SYS_OS; -1 when the
                 * program isn't running under an OS host) */
                {"os_spawn",   1, "P"},  {"os_kill",    1, "I"},
                {"os_status",  1, "I"},  {"os_focus",   1, "I"},
                {"os_count",   0, ""},   {"os_handle",  1, "I"},
                {"os_set_rect",5, "IIIII"},
                {"os_get_rect",2, "IP"},
                {"os_name",    3, "IPI"}, {"os_iconify", 2, "II"},
                {"os_dock",    5, "IIIII"},
                /* sound */
                {"play",         3, "PII"},
                {"music",        3, "PII"},
                {"mixer",        2, "II"},
                /* @tone: overloaded (1,2,3 ints) — see special case below */
                {"tone",        -1, NULL},
                {NULL, 0, NULL}
            };

            const BTSig *sig = NULL;
            for(int i=0; sigs[i].name; i++) {
                if(strcmp(sigs[i].name, n->sval)==0) { sig = &sigs[i]; break; }
            }

            /* Special case: @tone takes 1, 2, or 3 integer arguments. */
            if(strcmp(n->sval, "tone")==0) {
                if(n->nchildren < 1 || n->nchildren > 3) {
                    TC_ERR_NULL(tc, n,
                        "@tone takes 1 to 3 arguments, got %d", n->nchildren);
                }
                for(int i=0;i<n->nchildren && i<16;i++) {
                    Type *at = arg_types[i];
                    if(!(type_is_int(at) || (at && at->kind==TY_BOOL))) {
                        TC_ERR_NULL(tc, n,
                            "@tone argument %d must be an integer", i+1);
                    }
                }
            } else if(strcmp(n->sval, "window")==0) {
                /* @window(x, y, w, h)            -- legacy 4-arg form
                 * @window(x, y, w, h, flags)     -- with U32 flag bits
                 *   bit 0 (1) = stretch / letterboxed scaling (backend-defined)
                 *   bit 1 (2) = experimental coverage-buffer 3D path; honoured
                 *               only by the DOS allegro-dos backend, ignored
                 *               by all others
                 *   bit 2 (4) = disable VSync (present unthrottled); honoured by
                 *               backends with a swap-interval control, ignored
                 *               elsewhere
                 *   bit 3 (8) = global 2x SSAA (DOS allegro-dos only)
                 *   bit 4 (16)= disable user window resizing (Win32/Linux/macOS)
                 *   bit 5 (32)= centre on the primary monitor's work area,
                 *               overriding x,y (Win32/Linux/macOS)
                 *   bit 6 (64)= full-size content view: the drawing surface
                 *               covers the title bar strip and the title text
                 *               is removed (Win32/Linux/macOS)
                 *   bit 10(1024)=force 8-bit paletted colour depth (DOS only);
                 *               if 8bpp cannot be set, the program exits
                 * @window(x, y, w, h, flags, title) -- 6-arg form with a
                 *               C string for the window title. Ignored on
                 *               platforms without a title bar (DOS, web). */
                if(n->nchildren != 4 && n->nchildren != 5 && n->nchildren != 6) {
                    TC_ERR_NULL(tc, n,
                        "@window takes 4, 5, or 6 arguments, got %d", n->nchildren);
                }
                for(int i=0;i<n->nchildren && i<16;i++) {
                    Type *at = arg_types[i];
                    if(i < 5) {
                        if(!(type_is_int(at) || (at && at->kind==TY_BOOL))) {
                            TC_ERR_NULL(tc, n,
                                "@window argument %d must be an integer", i+1);
                        }
                    } else {
                        /* 6th argument (title) must be a string/pointer. */
                        if(!(at && (at->kind==TY_PTR || at->kind==TY_ARRAY ||
                                    type_is_int(at)))) {
                            TC_ERR_NULL(tc, n,
                                "@window argument %d must be a string", i+1);
                        }
                    }
                }
            } else if(sig) {
                /* Arity check (skip if variadic). */
                if(sig->arity >= 0 && n->nchildren != sig->arity) {
                    TC_ERR_NULL(tc, n,
                        "@%s: wrong number of arguments: expected %d, got %d",
                        n->sval, sig->arity, n->nchildren);
                }
                /* Loose per-slot type check. */
                if(sig->sig) {
                    int slots = (int)strlen(sig->sig);
                    int n_check = n->nchildren < slots ? n->nchildren : slots;
                    if(n_check > 16) n_check = 16;
                    /* f64 overload resolution: if ANY 'F' slot argument is
                     * f64 (including a 'd'-suffixed literal), the call is
                     * the f64 overload. Unsuffixed float literals then
                     * adopt f64 per-slot below; a stored f32 mixed in is
                     * an error - no implicit promotion. */
                    for(int i=0;i<n_check;i++)
                        if(sig->sig[i]=='F' && arg_types[i] &&
                           arg_types[i]->kind==TY_F64)
                            call_fw = TY_F64;
                    for(int i=0;i<n_check;i++) {
                        Type *at = arg_types[i];
                        if(!at) continue;
                        char k = sig->sig[i];
                        bool ok = true;
                        const char *want = "any";
                        switch(k) {
                            case 'I':
                                ok = type_is_int(at) || at->kind==TY_BOOL;
                                want = "integer";
                                break;
                            case 'F': {
                                TypeKind want_k =
                                    (call_fw==TY_F64) ? TY_F64 : TY_F32;
                                ok = (at->kind == want_k);
                                if(!ok && n->children[i]->kind==ND_FLOAT_LIT &&
                                   (at->kind==TY_F32 || at->kind==TY_F64)) {
                                    /* float literal adopts the call width */
                                    n->children[i]->type = type_alloc(want_k);
                                    arg_types[i] = n->children[i]->type;
                                    ok = true;
                                }
                                want = (want_k==TY_F64) ? "f64" : "f32";
                                break;
                            }
                            case 'P':
                                ok = (at->kind == TY_PTR ||
                                      at->kind == TY_REF ||
                                      at->kind == TY_ARRAY);
                                want = "pointer";
                                break;
                            case 'X': default:
                                ok = true; break;
                        }
                        if(!ok) {
                            TC_ERR_NULL(tc, n,
                                "@%s argument %d type mismatch (expected %s)",
                                n->sval, i+1, want);
                        }
                    }
                }
            }
            /* unknown builtin: leave to ir.c to report; we still set a
             * default void return type so error recovery doesn't spam. */

            /* Return types for each builtin */
            struct { const char *name; TypeKind ret; } builtins[] = {
                {"clear",       TY_VOID},                 {"sprite",       TY_VOID},{"plot",     TY_VOID}, {"flip",        TY_VOID},
                {"line",         TY_VOID},{"window",   TY_VOID},
                {"polygon",      TY_VOID},{"polyline",  TY_VOID},
                {"camera",       TY_VOID},{"texture",   TY_U16},
                {"palette_upload", TY_VOID},
                {"light",        TY_VOID},{"mesh",      TY_VOID},
                {"fog",          TY_VOID},
                {"text",         TY_VOID},{"viewport",  TY_VOID},
                {"text_measure", TY_VOID},{"clip",      TY_VOID},
                {"view",         TY_VOID},
                {"view_capture", TY_VOID},{"view_present", TY_VOID},
                {"draw_list",    TY_VOID},
                {"draw_mode",    TY_I32}, {"lines3d",    TY_I32},
                {"grid3d",       TY_I32},
                {"key_pressed",  TY_U8},  {"key_held", TY_BOOL},
                {"mouse",        TY_VOID},
                {"gamepad_count",TY_I32}, {"gamepad",  TY_I32},
                {"rand",         TY_U16}, {"millis",   TY_U32},  {"exit",        TY_VOID},
                {"console",      TY_VOID},{"snprint",  TY_PTR},  {"assert",      TY_VOID},
                {"alloc",        TY_PTR}, {"free",     TY_VOID},
                {"memcpy",       TY_I32}, {"memset",   TY_I32},
                {"file_open",    TY_I32}, {"file_read",TY_U32},  {"file_close",  TY_VOID},
                {"file_write",   TY_U32}, {"file_seek", TY_I32},
                {"file_length",  TY_I32}, {"file_directory",TY_I32},
                {"file_path",    TY_I32}, {"file_load_png", TY_I32},
                {"args",     TY_I32},  {"is_number",   TY_I32},
                {"sin",          TY_F32}, {"cos",      TY_F32},
                {"sqrt",         TY_F32},
                {"exp",          TY_F32}, {"log",      TY_F32},
                {"log10",        TY_F32}, {"pow",      TY_F32},
                {"atan",         TY_F32}, {"atan2",    TY_F32},
                {"tan",          TY_F32}, {"asin",     TY_F32},
                {"acos",         TY_F32}, {"floor",    TY_F32},
                {"ceil",         TY_F32}, {"round",    TY_F32},
                {"fmod",         TY_F32}, {"fabs",     TY_F32},
                {"sinh",         TY_F32}, {"cosh",     TY_F32},
                {"tanh",         TY_F32},
                {"http_request", TY_I32}, {"http_poll",   TY_I32},
                {"http_status",  TY_I32}, {"http_read",   TY_I32},
                {"http_close",   TY_VOID},
                {"unzip",        TY_I32},
                {"file_delete",  TY_I32},
                {"file_mkdir",   TY_I32},
                {"clipboard_set",TY_I32},
                {"clipboard_get",TY_I32},
                {"register",     TY_I32},
                {"process_spawn",      TY_I32}, {"process_poll",       TY_I32},
                {"process_exit_code",  TY_I32}, {"process_read_stdout",TY_I32},
                {"process_read_stderr",TY_I32}, {"process_kill",       TY_I32},
                {"process_close",      TY_I32},
                {"serve",              TY_I32}, {"serve_reply",         TY_I32},
                {"os_spawn",   TY_I32}, {"os_kill",    TY_I32},
                {"os_status",  TY_I32}, {"os_focus",   TY_I32},
                {"os_count",   TY_I32}, {"os_handle",  TY_I32},
                {"os_set_rect",TY_I32}, {"os_get_rect",TY_I32},
                {"os_name",    TY_I32},  {"os_iconify", TY_I32},
                {"os_dock",    TY_I32},
                {"tone",         TY_VOID},{"play",     TY_VOID},
                {"mixer",        TY_VOID},
                {"platform",     TY_VOID},
                {"wr_blocked",   TY_I32}, {"wr_blocked_line", TY_I32},
                {NULL, TY_VOID}
            };
            TypeKind rk = TY_VOID;
            for(int i=0; builtins[i].name; i++) {
                if(strcmp(builtins[i].name, n->sval)==0) { rk=builtins[i].ret; break; }
            }
            /* Math builtins are pinned TY_F32 in the table; an f64 call
             * (see call_fw) returns f64 instead. */
            if(rk == TY_F32 && call_fw == TY_F64) rk = TY_F64;
            Type *ret = type_alloc(rk);
            if(rk == TY_PTR) ret->inner = type_alloc(TY_U8);
            n->type = ret;
            return ret;
        }

        default:
            TC_ERR_NULL(tc, n, "unexpected expression node kind %d", n->kind);
    }
}

/* -------------------------------------------------------------------------
 * Statement type-checking
 * ---------------------------------------------------------------------- */
static bool tc_stmt(TypeChecker *tc, Node *n) {
    SymTable *st = &tc->syms;
    if(!n) return true;

    switch(n->kind) {

        case ND_BLOCK:
            sym_push_scope(st);
            for(int i=0;i<n->nchildren;i++)
                if(!tc_stmt(tc, n->children[i])) { sym_pop_scope(st); return false; }
            sym_pop_scope(st);
            return true;

        case ND_STMT_SEQ:
            /* Array-literal desugaring at a single-statement position
             * (if/while/for/match body): scopeless - generated `var`s
             * belong to the enclosing scope. */
            for(int i=0;i<n->nchildren;i++)
                if(!tc_stmt(tc, n->children[i])) return false;
            return true;

        case ND_VAR_DECL: {
            if(!n->type) TC_ERR(tc, n, "var '%s' has no type", n->sval);
            if(n->left) {
                Type *init = tc_expr(tc, n->left);
                if(!init) return false;
                if(!is_string_array_init(n->type, n->left) &&
                   !type_assignable(n->type, init) &&
                   !adopt_f64_lit(n->type, n->left))
                    TC_ERR(tc, n, "type mismatch in initialiser for '%s'", n->sval);
            }
            Symbol *s = sym_define(st, n->sval, SYM_VAR, n->type, n->line);
            if(!s) { strncpy(tc->err, st->err, sizeof(tc->err)-1); tc->err[sizeof(tc->err)-1] = '\0'; return false; }
            return true;
        }

        case ND_CONST_DECL: {
            if(!n->type) TC_ERR(tc, n, "const '%s' has no type", n->sval);
            if(n->left) {
                Type *init = tc_expr(tc, n->left);
                if(!init) return false;
                if(!type_assignable(n->type, init) &&
                   !adopt_f64_lit(n->type, n->left))
                    TC_ERR(tc, n, "type mismatch in initialiser for '%s'", n->sval);
            }
            Symbol *s = sym_define(st, n->sval, SYM_CONST, n->type, n->line);
            if(!s) { strncpy(tc->err, st->err, sizeof(tc->err)-1); tc->err[sizeof(tc->err)-1] = '\0'; return false; }
            // Constant-fold the initializer into const_val when it is a
            // compile-time constant expression (literals, arithmetic, casts,
            // references to already-folded consts). Non-foldable initializers
            // (e.g. function calls) leave has_const_val false so the IR
            // emitter gives the const runtime storage instead.
            if(n->left && relic_fold_const(tc, n->left, &s->const_val)) {
                s->has_const_val = true;
            }
            return true;
        }

        case ND_ASSIGN: {
            /* (placeholder - replaced below) */
            Type *lhs = tc_expr(tc, n->left);
            Type *rhs = tc_expr(tc, n->right);
            if(!lhs || !rhs) return false;
            if(!type_assignable(lhs, rhs) && !adopt_f64_lit(lhs, n->right))
                TC_ERR(tc, n, "assignment type mismatch");
            return true;
        }

        case ND_IF: {
            Type *ct = tc_expr(tc, n->cond);
            if(!ct) return false;
            if(!tc_stmt(tc, n->left)) return false;
            if(n->right && !tc_stmt(tc, n->right)) return false;
            return true;
        }

        case ND_WHILE: {
            Type *ct = tc_expr(tc, n->cond);
            if(!ct) return false;
            return tc_stmt(tc, n->left);
        }

        case ND_FOR: {
            /* cond is ND_RANGE; sval is loop var name.
             * When the name is already a visible variable the loop drives
             * THAT variable (its value is still readable after the loop -
             * see tests/language/BUG5) so its declared type is reused;
             * ir.c likewise reuses the slot and its access width. Only an
             * otherwise-undeclared name gets a fresh i16 induction var. */
            Symbol *outer = sym_lookup(st, n->sval);
            Type *vt = (outer && outer->kind == SYM_VAR && outer->type)
                       ? outer->type : type_alloc(TY_I16);
            /* The emitter needs vt to size the induction slot: a visible
             * GLOBAL `j` (e.g. `var j : i32` at file scope) makes the for-var
             * i32 too, so it can look up n->type rather than guessing i16.
             * Mismatched widths silently corrupt on big-endian hosts. */
            n->type = vt;
            sym_push_scope(st);
            Symbol *s = sym_define(st, n->sval, SYM_VAR, vt, n->line);
            if(!s) { strncpy(tc->err, st->err, sizeof(tc->err)-1); tc->err[sizeof(tc->err)-1] = '\0'; sym_pop_scope(st); return false; }
            Node *range = n->cond;
            if(!tc_expr(tc, range->left) || !tc_expr(tc, range->right)) { sym_pop_scope(st); return false; }
            bool ok = tc_stmt(tc, n->left);
            sym_pop_scope(st);
            return ok;
        }

        case ND_MATCH: {
            Type *mt = tc_expr(tc, n->cond);
            if(!mt) return false;
            for(int i=0;i<n->nchildren;i++) {
                Node *arm = n->children[i];
                if(!arm->bval) { /* not wildcard */
                    Type *pt = tc_expr(tc, arm->cond);
                    if(!pt) return false;
                    /* alternatives of a multi-value arm (`0 | 1 => ...`) */
                    for(int j=0;j<arm->nchildren;j++)
                        if(!tc_expr(tc, arm->children[j])) return false;
                }
                if(!tc_stmt(tc, arm->left)) return false;
            }
            return true;
        }

        case ND_RETURN: {
            if(n->left) {
                Type *rt = tc_expr(tc, n->left);
                if(!rt) return false;
                if(tc->cur_ret_type && !type_assignable(tc->cur_ret_type, rt) &&
                   !adopt_f64_lit(tc->cur_ret_type, n->left))
                    TC_ERR(tc, n, "return type mismatch in '%s'", tc->cur_fn);
            } else {
                if(tc->cur_ret_type && tc->cur_ret_type->kind != TY_VOID)
                    TC_ERR(tc, n, "missing return value in '%s'", tc->cur_fn);
            }
            return true;
        }

        case ND_BREAK: case ND_CONTINUE:
            return true;

        case ND_EXPR_STMT:
            return tc_expr(tc, n->left) != NULL;

        case ND_MATCH_ARM:
            return tc_stmt(tc, n->left);

        default:
            TC_ERR(tc, n, "unexpected statement node kind %d", n->kind);
    }
}

/* -------------------------------------------------------------------------
 * Declaration type-checking (pass 1: register names; pass 2: bodies)
 * ---------------------------------------------------------------------- */
static bool tc_decl(TypeChecker *tc, Node *n) {
    SymTable *st = &tc->syms;

    switch(n->kind) {

        case ND_STRUCT_DECL:
        case ND_CLASS_DECL: {
            /* register struct / class (a class decl's children are its
             * fields only, so it lays out exactly like a struct) */
            if(tc->nstructs >= RELIC_MAX_STRUCTS) TC_ERR(tc, n, "too many structs");
            snprintf(tc->structs[tc->nstructs].name, RELIC_MAX_IDENT, "%s", n->sval);
            tc->structs[tc->nstructs].decl = n;
            tc->nstructs++;
            /* type-check field types */
            for(int i=0;i<n->nchildren;i++)
                if(!n->children[i]->type) TC_ERR(tc, n, "field '%s' has no type", n->children[i]->sval);
            return true;
        }

        case ND_CONST_DECL: {
            Type *vt = tc_expr(tc, n->left);
            if(!vt) return false;
            /* f64 strictness only: a const f64 with an incompatible init
             * would otherwise fold/store at the wrong width (the emitter
             * now stores f64 consts 8 bytes wide). Existing loose paths
             * for other type pairs are unchanged. */
            if((n->type && n->type->kind == TY_F64) ||
               (vt && vt->kind == TY_F64)) {
                if(!type_assignable(n->type, vt) &&
                   !adopt_f64_lit(n->type, n->left))
                    TC_ERR(tc, n, "type mismatch in const '%s'", n->sval);
            }
            Symbol *s = sym_define(st, n->sval, SYM_CONST, n->type, n->line);
            if(!s) { strncpy(tc->err, st->err, sizeof(tc->err)-1); tc->err[sizeof(tc->err)-1] = '\0'; return false; }
            // Constant-fold when possible; otherwise the IR emitter allocates
            // runtime storage for the const (see ir.c global-init loop).
            if(relic_fold_const(tc, n->left, &s->const_val)) {
                s->has_const_val = true;
            }
            return true;
        }

        case ND_VAR_DECL: {
            if(n->left) {
                Type *init = tc_expr(tc, n->left);
                if(!init) return false;
                if(!is_string_array_init(n->type, n->left) &&
                   !type_assignable(n->type, init) &&
                   !adopt_f64_lit(n->type, n->left))
                    TC_ERR(tc, n, "type mismatch in global var '%s'", n->sval);
            }
            Symbol *s = sym_define(st, n->sval, SYM_VAR, n->type, n->line);
            if(!s) { strncpy(tc->err, st->err, sizeof(tc->err)-1); tc->err[sizeof(tc->err)-1] = '\0'; return false; }
            return true;
        }

        case ND_FN_DECL: {
            /* Reuse the symbol registered in pass 1 — do not re-define */
            Symbol *fs = sym_lookup(st, n->sval);
            if(!fs) {
                /* Not found (e.g. global var pass called directly): define now */
                Type *fnt = type_alloc(TY_FNPTR);
                fnt->inner   = n->type;
                fnt->nparams = n->nchildren;
                if(n->nchildren > 0) {
                    fnt->params = calloc(n->nchildren, sizeof(Type*));
                    for(int i=0;i<n->nchildren;i++)
                        fnt->params[i] = n->children[i]->type;
                }
                fs = sym_define(st, n->sval, SYM_FN, fnt, n->line);
                if(!fs) { strncpy(tc->err, st->err, sizeof(tc->err)-1); tc->err[sizeof(tc->err)-1] = '\0'; return false; }
            }
            /* Type-check body */
            snprintf(tc->cur_fn, RELIC_MAX_IDENT, "%s", n->sval);
            tc->cur_ret_type = n->type;
            /* Class context for implicit member lookup / ctor handling.
             * method_of_class also matches hand-written `Class__m`
             * names in .tp input, so both forms behave the same. */
            tc->cur_class = method_of_class(tc, n);
            sym_push_scope(st);
            /* define params. Array-typed params decay to *T so they are
             * passed by reference (matching the emitter's calling
             * convention). */
            for(int i=0;i<n->nchildren;i++) {
                Node *param = n->children[i];
                if(param->type && param->type->kind == TY_ARRAY) {
                    param->type = type_ptr(param->type->inner);
                }
                Symbol *ps = sym_define(st, param->sval, SYM_PARAM, param->type, param->line);
                if(!ps) { strncpy(tc->err, st->err, sizeof(tc->err)-1); tc->err[sizeof(tc->err)-1] = '\0'; sym_pop_scope(st); return false; }
            }
            bool ok = tc_stmt(tc, n->left); /* body block */
            sym_pop_scope(st);
            tc->cur_class = NULL;
            return ok;
        }

        default:
            TC_ERR(tc, n, "unexpected declaration kind %d", n->kind);
    }
}

/* -------------------------------------------------------------------------
 * Uninitialised-field warnings for local struct/class variables.
 *
 * The VM never zero-initialises stack slots, so `var m: Menu` leaves
 * every field holding whatever the stack happened to contain - a bug
 * class that crashes nondeterministically depending on what ran earlier.
 * Globals are zeroed at vm_init, so only function locals are checked.
 *
 * The analysis is deliberately flow-insensitive (a "may" analysis):
 *
 *  - A field written ANYWHERE in the function counts as written; we do
 *    not try to prove ordering within a body.
 *  - `&v` (including the implicit `&self` a desugared `self.m()` call
 *    passes) suppresses all warnings for v: the callee may initialise
 *    fields through the pointer, which we cannot see. The documented
 *    `var self: X; self.init(); return self;` constructor pattern stays
 *    warning-free for this reason.
 *  - Escapes - `return v`, passing `v` by value, copying `v` wholesale -
 *    hand any unwritten fields to the caller, so each never-assigned
 *    field warns once at the first escape (or first read) site.
 * ---------------------------------------------------------------------- */
typedef struct {
    char   name[RELIC_MAX_IDENT];
    Node  *sdecl;         /* ND_STRUCT_DECL / ND_CLASS_DECL */
    int    nfields;
    u8    *written;       /* [nfields] field assigned somewhere */
    Node **first_read;    /* [nfields] first non-lhs read site */
    Node  *first_escape;  /* first by-value escape site */
    int    addr_taken;    /* &v observed: callee may init via pointer */
    int    covered;       /* whole-var `v = expr` observed */
} UWVar;

typedef struct {
    TypeChecker *tc;
    UWVar       *vars;
    int          nvars, cap;
} UWCtx;

static void uw_node(UWCtx *w, Node *n, int lhs);

static void uw_warn(UWCtx *w, Node *n, const char *fmt, ...) {
    IRBuffer *ir = w->tc->ir;
    if(!ir) return;
    if(ir->nwarnings >= RELIC_MAX_WARNINGS) { ir->warnings_dropped++; return; }
    RelicWarning *nw = (RelicWarning*)realloc(ir->warnings,
                        (size_t)(ir->nwarnings + 1) * sizeof(RelicWarning));
    if(!nw) { ir->warnings_dropped++; return; }
    ir->warnings = nw;
    RelicWarning *wn = &ir->warnings[ir->nwarnings++];
    wn->file_id = n ? n->file_id : -1;
    wn->line    = n ? n->line : 0;
    wn->col     = n ? n->col : -1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(wn->message, sizeof(wn->message), fmt, ap);
    va_end(ap);
}

static UWVar *uw_find(UWCtx *w, const char *name) {
    for(int i = 0; i < w->nvars; i++)
        if(strcmp(w->vars[i].name, name) == 0) return &w->vars[i];
    return NULL;
}

static int uw_field_index(UWVar *v, const char *name) {
    for(int i = 0; i < v->sdecl->nchildren; i++)
        if(strcmp(v->sdecl->children[i]->sval, name) == 0) return i;
    return -1;
}

static void uw_register(UWCtx *w, Node *decl, Node *sdecl) {
    if(uw_find(w, decl->sval)) return;      /* shadowing: first wins */
    if(w->nvars >= w->cap) {
        int ncap = w->cap ? w->cap * 2 : 8;
        UWVar *nv = (UWVar*)realloc(w->vars, (size_t)ncap * sizeof(UWVar));
        if(!nv) return;
        w->vars = nv; w->cap = ncap;
    }
    UWVar *v = &w->vars[w->nvars];
    memset(v, 0, sizeof *v);
    snprintf(v->name, RELIC_MAX_IDENT, "%s", decl->sval);
    v->sdecl   = sdecl;
    v->nfields = sdecl->nchildren;
    v->written    = (u8*)calloc((size_t)v->nfields, 1);
    v->first_read = (Node**)calloc((size_t)v->nfields, sizeof(Node*));
    if(!v->written || !v->first_read) {
        free(v->written); free(v->first_read);
        v->written = NULL; v->first_read = NULL;
        return;
    }
    w->nvars++;
}

/* Mark the field (or whole var) a tracked local writes through on the
 * LHS of an assignment. Descends `v.f`, `v.f[i]`, `v.a.b`; the field
 * directly attached to the var is what counts as written. */
static void uw_lval(UWCtx *w, Node *n) {
    while(n) {
        if(n->kind == ND_IDENT) {
            UWVar *v = uw_find(w, n->sval);
            if(v) v->covered = 1;
            return;
        }
        if(n->kind == ND_FIELD_ACCESS) {
            if(n->left && n->left->kind == ND_IDENT) {
                UWVar *v = uw_find(w, n->left->sval);
                if(v) {
                    int fi = uw_field_index(v, n->sval);
                    if(fi >= 0) v->written[fi] = 1;
                }
                return;
            }
            n = n->left;
            continue;
        }
        if(n->kind == ND_INDEX) {
            uw_node(w, n->right, 0);   /* index expr is a normal read */
            n = n->left;
            continue;
        }
        break;
    }
    if(n) uw_node(w, n, 0);            /* unrecognised lhs shape */
}

static void uw_node(UWCtx *w, Node *n, int lhs) {
    if(!n) return;
    switch(n->kind) {
        case ND_VAR_DECL:
            if(n->type && n->type->kind == TY_STRUCT && !n->left) {
                Node *sd = find_struct_decl(w->tc, n->type->name);
                if(sd) uw_register(w, n, sd);
            }
            if(n->left) uw_node(w, n->left, 0);
            return;
        case ND_ASSIGN:
            uw_lval(w, n->left);
            uw_node(w, n->right, 0);
            return;
        case ND_UNOP:
            if(n->op == TK_AMPERSAND) {
                /* `&v` / `&v.f`: the callee may write through the
                 * pointer, so the var is untrackable - flag it and walk
                 * the operand as an lvalue (index exprs still read). */
                Node *b = n->left;
                while(b && (b->kind == ND_FIELD_ACCESS || b->kind == ND_INDEX))
                    b = b->left;
                if(b && b->kind == ND_IDENT) {
                    UWVar *v = uw_find(w, b->sval);
                    if(v) v->addr_taken = 1;
                }
                uw_node(w, n->left, 1);
                return;
            }
            break;
        case ND_RETURN:
            if(n->left) {
                if(n->left->kind == ND_IDENT) {
                    UWVar *v = uw_find(w, n->left->sval);
                    if(v && !v->first_escape) v->first_escape = n;
                }
                uw_node(w, n->left, 0);
            }
            return;
        case ND_CALL:
        case ND_BUILTIN_CALL:
            /* A bare tracked var as an argument is a by-value escape. */
            for(int i = 0; i < n->nchildren; i++) {
                Node *a = n->children[i];
                if(a && a->kind == ND_IDENT) {
                    UWVar *v = uw_find(w, a->sval);
                    if(v && !v->first_escape) v->first_escape = a;
                }
            }
            break;
        case ND_IDENT:
            /* Any other whole-var use is a by-value escape too. */
            if(!lhs) {
                UWVar *v = uw_find(w, n->sval);
                if(v && !v->first_escape) v->first_escape = n;
            }
            return;
        case ND_FIELD_ACCESS:
            if(!lhs && n->left && n->left->kind == ND_IDENT) {
                UWVar *v = uw_find(w, n->left->sval);
                if(v) {
                    int fi = uw_field_index(v, n->sval);
                    if(fi >= 0 && !v->first_read[fi])
                        v->first_read[fi] = n;
                }
            }
            /* The base of a field access is an lvalue chain, not a
             * by-value use - walk it in lhs mode so the trailing ND_IDENT
             * doesn't count as an escape. */
            uw_node(w, n->left, 1);
            return;
        case ND_INDEX:
            /* Same: the array/struct base is an address chain; only the
             * index expression is an ordinary read. */
            uw_node(w, n->left, 1);
            uw_node(w, n->right, 0);
            return;
        default:
            break;
    }
    uw_node(w, n->left, 0);
    uw_node(w, n->right, 0);
    uw_node(w, n->cond, 0);
    for(int i = 0; i < n->nchildren; i++)
        uw_node(w, n->children[i], 0);
}

static void uw_report(UWCtx *w, UWVar *v) {
    if(v->covered || v->addr_taken) return;
    for(int i = 0; i < v->nfields; i++) {
        if(v->written[i]) continue;
        const char *fname = v->sdecl->children[i]->sval;
        if(v->first_read[i]) {
            uw_warn(w, v->first_read[i],
                "field '%s' of '%s' may be uninitialised - never assigned "
                "before it is read", fname, v->name);
        } else if(v->first_escape) {
            uw_warn(w, v->first_escape,
                "field '%s' of '%s' may be uninitialised - never assigned "
                "before '%s' escapes", fname, v->name, v->name);
        }
    }
}

/* Walk one function body for uninitialised local struct fields. */
static void uw_function(TypeChecker *tc, Node *fn) {
    UWCtx w;
    memset(&w, 0, sizeof w);
    w.tc = tc;
    uw_node(&w, fn->left, 0);
    for(int i = 0; i < w.nvars; i++) {
        uw_report(&w, &w.vars[i]);
        free(w.vars[i].written);
        free(w.vars[i].first_read);
    }
    free(w.vars);
}

/* -------------------------------------------------------------------------
 * Entry point
 * Two-pass: first register all struct/fn names, then type-check bodies.
 * ---------------------------------------------------------------------- */
bool relic_typecheck(TypeChecker *tc, Node *root, IRBuffer *ir) {
    memset(tc, 0, sizeof(*tc));
    tc->ir       = ir;
    tc->err_file = -1;
    tc->err_col  = -1;

    /* Pass 1: register struct/class names and function signatures */
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind == ND_STRUCT_DECL || d->kind == ND_CLASS_DECL) {
            if(!tc_decl(tc, d)) return false;
        }
    }

    /* Pass 1a-0: bind out-of-class method definitions (`fn C.m`) to their
     * in-class declarations and resolve `;` member prototypes. Must run
     * after every class is registered (pass 1) and before arity mangling
     * (pass 1a) so undeclared/duplicate/mismatched definitions report the
     * member-level error rather than a generic duplicate-symbol one. */
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind == ND_FN_DECL && (d->mflags & ND_MF_OUTCLASS)) {
            if(!bind_outclass_method(tc, d)) return false;
        }
    }
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind == ND_CLASS_DECL) {
            if(!resolve_class_members(tc, d)) return false;
        }
    }

    /* Pass 1a: identify overloaded function names. A name that appears on
     * more than one FN_DECL is mangled to `name$N` (N = arity) so each
     * overload has a unique identifier in the symbol table. Names that
     * appear exactly once are left alone — this preserves backward
     * compatibility and avoids touching `main`. We first compute the
     * "overloaded" flag for each FN_DECL (against the original names),
     * then mangle in a second sweep so the rename does not perturb the
     * counts. */
    bool *fn_overloaded = NULL;
    if(root->nchildren > 0) {
        fn_overloaded = calloc((size_t)root->nchildren, sizeof(bool));
    }
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind != ND_FN_DECL) continue;
        for(int j=0;j<root->nchildren;j++) {
            if(j == i) continue;
            Node *dj = root->children[j];
            if(dj->kind == ND_FN_DECL && strcmp(dj->sval, d->sval) == 0) {
                fn_overloaded[i] = true;
                break;
            }
        }
    }
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind != ND_FN_DECL || !fn_overloaded[i]) continue;
        char mangled[RELIC_MAX_IDENT];
        int rc = snprintf(mangled, sizeof(mangled), "%s$%d", d->sval, d->nchildren);
        if(rc <= 0 || rc >= (int)sizeof(mangled)) {
            free(fn_overloaded);
            TC_ERR(tc, d, "function name too long to mangle: '%s'", d->sval);
        }
        /* Detect duplicate (same name + same arity): ambiguity the compiler
         * cannot resolve. Scan earlier FN_DECLs that have already been
         * mangled in this sweep. */
        for(int j=0;j<i;j++) {
            Node *dj = root->children[j];
            if(dj->kind == ND_FN_DECL && fn_overloaded[j] &&
               strcmp(dj->sval, mangled) == 0) {
                int prev_line = dj->line;
                free(fn_overloaded);
                TC_ERR(tc, d,
                       "duplicate function '%s' with %d parameter(s) (first declared on line %d)",
                       d->sval, d->nchildren, prev_line);
            }
        }
        snprintf(d->sval, RELIC_MAX_IDENT, "%s", mangled);
    }
    free(fn_overloaded);

    /* Pass 1b: register each function's (possibly mangled) signature. */
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind == ND_FN_DECL) {
            SymTable *st = &tc->syms;
            Type *fnt = type_alloc(TY_FNPTR);
            fnt->inner   = d->type;
            fnt->nparams = d->nchildren;
            if(d->nchildren > 0) {
                fnt->params = calloc(d->nchildren, sizeof(Type*));
                for(int j=0;j<d->nchildren;j++)
                    fnt->params[j] = d->children[j]->type;
            }
            sym_define(st, d->sval, SYM_FN, fnt, d->line);
        }
    }

    /* Pass 2: full check of all declarations */
    for(int i=0;i<root->nchildren;i++) {
        if(!tc_decl(tc, root->children[i])) return false;
    }

    /* Pass 3: uninitialised-field warnings on function bodies. Runs only
     * when everything typechecked - node types are resolved by now. */
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind == ND_FN_DECL && d->left)
            uw_function(tc, d);
    }
    return true;
}
