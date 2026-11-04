/*
 * parser.c
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
 * parser.c - Relic recursive descent parser
 * Consumes a Token array from the lexer, produces an AST rooted at ND_PROGRAM.
 */
#include "tigryx.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Parser helpers
 * ---------------------------------------------------------------------- */
static Token *cur(Parser *p)  { return &p->tokens[p->pos]; }
static Token *peek(Parser *p) { return &p->tokens[p->pos < p->ntokens-1 ? p->pos+1 : p->pos]; }

static Token *advance(Parser *p) {
    Token *t = &p->tokens[p->pos];
    if(p->pos < p->ntokens-1) p->pos++;
    return t;
}

static bool check(Parser *p, TokenKind k) { return cur(p)->kind == k; }

static bool eat(Parser *p, TokenKind k) {
    if(check(p, k)) { advance(p); return true; }
    return false;
}

static bool expect(Parser *p, TokenKind k) {
    if(eat(p, k)) return true;
    snprintf(p->err, sizeof(p->err), "line %d: expected '%s', got '%s'",
             cur(p)->line, relic_token_name(k), relic_token_name(cur(p)->kind));
    p->err_line = cur(p)->line;
    return false;
}

static Node *alloc_node(Parser *p, NodeKind kind) {
    if(p->npool >= p->capacity) {
        int new_capacity = p->capacity ? p->capacity * 2 : 4096;
        Node **new_pool = realloc(p->pool, new_capacity * sizeof(Node*));
        if(!new_pool) {
            snprintf(p->err, sizeof(p->err), "out of memory reallocating node pool");
            return NULL;
        }
        p->pool = new_pool;
        p->capacity = new_capacity;
    }
    /* Each node is heap-allocated individually so that growing the pool
     * (a Node** index array) never invalidates Node* references already
     * stored in the AST (left/right/cond/children[]). */
    Node *n = calloc(1, sizeof(Node));
    if(!n) {
        snprintf(p->err, sizeof(p->err), "out of memory allocating node");
        return NULL;
    }
    n->kind    = kind;
    n->line    = cur(p)->line;
    n->file_id = cur(p)->file_id;
    n->col     = cur(p)->col;
    p->pool[p->npool++] = n;
    return n;
}

static bool add_child(Parser *p, Node *parent, Node *child) {
    /* Children stored in a simple realloc'd array - we use the pool for children ptrs too */
    (void)p;
    Node **old = parent->children;
    int    cnt = parent->nchildren;
    Node **new_children = realloc(old, (cnt+1) * sizeof(Node*));
    if(!new_children) { return false; }
    parent->children = new_children;
    parent->children[cnt] = child;
    parent->nchildren = cnt+1;
    return true;
}

/* Insert a child at index 0 (used to prepend an injected `self` param /
 * ctor `var self` decl in native class desugaring). */
static bool prepend_child(Node *parent, Node *child) {
    Node **nc = realloc(parent->children,
                      (size_t)(parent->nchildren + 1) * sizeof(Node*));
    if(!nc) return false;
    memmove(&nc[1], &nc[0], (size_t)parent->nchildren * sizeof(Node*));
    nc[0] = child;
    parent->children = nc;
    parent->nchildren++;
    return true;
}

/* Append a method node to a class's members[] list (ND_CLASS_DECL only). */
static bool add_member(Node *cls, Node *m) {
    Node **nm = realloc(cls->members,
                      (size_t)(cls->nmembers + 1) * sizeof(Node*));
    if(!nm) return false;
    cls->members = nm;
    cls->members[cls->nmembers] = m;
    cls->nmembers++;
    return true;
}

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
static Node *parse_decl(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_expr(Parser *p);
static Type *parse_type(Parser *p);

/* Whole-subtree search used by constructor desugaring: whether to inject
 * `var self:` / `return self` is decided by scanning the body AST (a hit
 * at any nesting depth counts). */
static bool node_tree_has(Node *n, bool (*pred)(Node *)) {
    if(!n) return false;
    if(pred(n)) return true;
    if(node_tree_has(n->left, pred))  return true;
    if(node_tree_has(n->right, pred)) return true;
    if(node_tree_has(n->cond, pred))  return true;
    for(int i=0;i<n->nchildren;i++)
        if(node_tree_has(n->children[i], pred)) return true;
    return false;
}
static bool pred_var_self(Node *n) {
    return n->kind == ND_VAR_DECL && strcmp(n->sval, "self") == 0;
}
static bool pred_return(Node *n) { return n->kind == ND_RETURN; }

/* -------------------------------------------------------------------------
 * Type parser
 * ---------------------------------------------------------------------- */
static Type *alloc_type(TypeKind kind) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = kind;
    return t;
}

static Type *parse_type(Parser *p) {
    /* function pointer: *fn(...) -> type */
    if(check(p, TK_STAR) && peek(p)->kind == TK_FN) {
        advance(p); advance(p);
        if(!expect(p, TK_LPAREN)) return NULL;
        Type *ft = alloc_type(TY_FNPTR);
        int nparams = 0;
        Type **params = NULL;
        while(!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
            Type *pt = parse_type(p);
            if(!pt) return NULL;
            params = realloc(params, (nparams+1)*sizeof(Type*));
            params[nparams++] = pt;
            if(!eat(p, TK_COMMA)) break;
        }
        if(!expect(p, TK_RPAREN)) return NULL;
        ft->params  = params;
        ft->nparams = nparams;
        if(eat(p, TK_ARROW)) {
            ft->inner = parse_type(p);
            if(!ft->inner) return NULL;
        } else {
            ft->inner = alloc_type(TY_VOID);
        }
        return ft;
    }

    /* pointer: *type */
    if(check(p, TK_STAR)) {
        advance(p);
        Type *inner = parse_type(p);
        if(!inner) return NULL;
        Type *t = alloc_type(TY_PTR);
        t->inner = inner;
        return t;
    }

    /* reference: ref type
     * Same runtime representation as *T but the type checker guarantees:
     *   - the value is non-null
     *   - pointer arithmetic is not permitted
     *   - it cannot be created from an integer literal
     * Created by `&lvalue` or by widening from a ptr after a null-check
     * (currently via `cast`; a future `as_ref` builtin will do the check). */
    if(check(p, TK_REF)) {
        advance(p);
        Type *inner = parse_type(p);
        if(!inner) return NULL;
        Type *t = alloc_type(TY_REF);
        t->inner = inner;
        return t;
    }

    /* static array: [N]type */
    if(check(p, TK_LBRACKET)) {
        advance(p);
        if(!check(p, TK_INT)) {
            snprintf(p->err, sizeof(p->err), "line %d: array size must be integer literal", cur(p)->line);
            return NULL;
        }
        u32 count = (u32)cur(p)->val.ival;
        advance(p);
        if(!expect(p, TK_RBRACKET)) return NULL;
        Type *inner = parse_type(p);
        if(!inner) return NULL;
        Type *t = alloc_type(TY_ARRAY);
        t->inner = inner;
        t->count = count;
        return t;
    }

    /* base types */
    TokenKind k = cur(p)->kind;
    advance(p);
    switch(k) {
        case TK_BOOL: return alloc_type(TY_BOOL);
        case TK_I8:   return alloc_type(TY_I8);
        case TK_U8:   return alloc_type(TY_U8);
        case TK_I16:  return alloc_type(TY_I16);
        case TK_U16:  return alloc_type(TY_U16);
        case TK_I32:  return alloc_type(TY_I32);
        case TK_U32:  return alloc_type(TY_U32);
        case TK_F32:  return alloc_type(TY_F32);
        case TK_F64:  return alloc_type(TY_F64);
        case TK_IDENT: {
            Type *t = alloc_type(TY_STRUCT);
            Token *prev = &p->tokens[p->pos-1];
            int l = prev->len < RELIC_MAX_IDENT-1 ? prev->len : RELIC_MAX_IDENT-1;
            memcpy(t->name, prev->start, l);
            t->name[l] = 0;
            return t;
        }
        default:
            snprintf(p->err, sizeof(p->err), "line %d: expected type, got '%s'",
                     cur(p)->line, relic_token_name(k));
            return NULL;
    }
}

/* -------------------------------------------------------------------------
 * Expression parsing (Pratt / precedence climbing)
 * ---------------------------------------------------------------------- */
static Node *parse_primary(Parser *p) {
    Token *t = cur(p);

    /* Integer literal */
    if(t->kind == TK_INT) {
        Node *n = alloc_node(p, ND_INT_LIT);
        if(!n) return NULL;
        n->ival = t->val.ival;
        advance(p);
        return n;
    }

    /* Float literal */
    if(t->kind == TK_FLOAT) {
        Node *n = alloc_node(p, ND_FLOAT_LIT);
        if(!n) return NULL;
        n->fval = t->val.fval;
        /* A 'd' suffix pre-types the literal f64 so typecheck keeps the
         * exact double in fval; unsuffixed literals get TY_F32 from
         * typecheck (or adopt f64 from an f64 context). */
        if(t->f64lit) n->type = alloc_type(TY_F64);
        advance(p);
        return n;
    }

    /* Char literal */
    if(t->kind == TK_CHAR) {
        Node *n = alloc_node(p, ND_CHAR_LIT);
        if(!n) return NULL;
        n->ival = t->val.ival;
        advance(p);
        return n;
    }

    /* String literal */
    if(t->kind == TK_STRING) {
        Node *n = alloc_node(p, ND_STRING_LIT);
        if(!n) return NULL;
        /* store token index - emitter will intern from raw source */
        n->soff = p->pos;
        /* Pre-compute decoded length (resolving escape sequences).
         * Token text includes opening and closing quotes; skip them. */
        int dlen = 0;
        const char *s = t->start + 1;          /* skip opening quote */
        const char *e = t->start + t->len - 1; /* skip closing quote */
        while(s < e) {
            if(*s == '\\' && s + 1 < e) {
                if(s[1] == 'x' && s + 3 < e) { s += 4; }
                else { s += 2; }
            } else { s++; }
            dlen++;
        }
        n->ival = dlen;
        advance(p);
        return n;
    }

    /* Bool literals */
    if(t->kind == TK_TRUE || t->kind == TK_FALSE) {
        Node *n = alloc_node(p, ND_BOOL_LIT);
        if(!n) return NULL;
        n->bval = (t->kind == TK_TRUE);
        advance(p);
        return n;
    }

    /* Grouped expression */
    if(t->kind == TK_LPAREN) {
        advance(p);
        Node *n = parse_expr(p);
        if(!n) return NULL;
        if(!expect(p, TK_RPAREN)) return NULL;
        return n;
    }

    /* cast(type, expr) */
    if(t->kind == TK_CAST) {
        Node *n = alloc_node(p, ND_CAST);
        if(!n) return NULL;
        advance(p);
        if(!expect(p, TK_LPAREN)) return NULL;
        n->type = parse_type(p);
        if(!n->type) return NULL;
        if(!expect(p, TK_COMMA)) return NULL;
        n->left = parse_expr(p);
        if(!n->left) return NULL;
        if(!expect(p, TK_RPAREN)) return NULL;
        return n;
    }

    /* Builtin call: @ident(...) */
    if(t->kind == TK_AT) {
        advance(p);
        if(!check(p, TK_IDENT)) {
            snprintf(p->err, sizeof(p->err), "line %d: expected builtin name after '@'", cur(p)->line);
            return NULL;
        }
        Node *n = alloc_node(p, ND_BUILTIN_CALL);
        if(!n) return NULL;
        Token *nm = cur(p);
        int l = nm->len < RELIC_MAX_IDENT-1 ? nm->len : RELIC_MAX_IDENT-1;
        memcpy(n->sval, nm->start, l); n->sval[l] = 0;
        advance(p);
        if(!expect(p, TK_LPAREN)) return NULL;
        while(!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
            Node *arg = parse_expr(p);
            if(!arg) return NULL;
            if(!add_child(p, n, arg)) return NULL;
            if(!eat(p, TK_COMMA)) break;
        }
        if(!expect(p, TK_RPAREN)) return NULL;
        return n;
    }

    /* Identifier */
    if(t->kind == TK_IDENT) {
        /* `new Class(args)` - constructor call. `new` is contextual, not a
         * keyword: it is special only when an identifier and '(' follow.
         * Any other use stays a plain identifier. */
        if(t->len == 3 && memcmp(t->start, "new", 3) == 0) {
            Token *t1 = p->pos + 1 < p->ntokens ? &p->tokens[p->pos + 1] : t;
            Token *t2 = p->pos + 2 < p->ntokens ? &p->tokens[p->pos + 2] : t;
            if(t1->kind == TK_IDENT && t2->kind == TK_LPAREN) {
                Node *n = alloc_node(p, ND_NEW);
                if(!n) return NULL;
                advance(p); /* `new` */
                int l = t1->len < RELIC_MAX_IDENT-1 ? t1->len : RELIC_MAX_IDENT-1;
                memcpy(n->sval, t1->start, l); n->sval[l] = 0;
                advance(p); /* class name */
                advance(p); /* `(` */
                while(!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
                    Node *arg = parse_expr(p);
                    if(!arg) return NULL;
                    if(!add_child(p, n, arg)) return NULL;
                    if(!eat(p, TK_COMMA)) break;
                }
                if(!expect(p, TK_RPAREN)) return NULL;
                return n;
            }
        }
        Node *n = alloc_node(p, ND_IDENT);
        if(!n) return NULL;
        int l = t->len < RELIC_MAX_IDENT-1 ? t->len : RELIC_MAX_IDENT-1;
        memcpy(n->sval, t->start, l); n->sval[l] = 0;
        advance(p);
        return n;
    }

    snprintf(p->err, sizeof(p->err), "line %d: unexpected token '%s' in expression",
             t->line, relic_token_name(t->kind));
    return NULL;
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    if(!n) return NULL;
    while(true) {
        if(check(p, TK_DOT)) {
            advance(p);
            if(!check(p, TK_IDENT)) {
                snprintf(p->err, sizeof(p->err), "line %d: expected field name", cur(p)->line);
                return NULL;
            }
            Node *fa = alloc_node(p, ND_FIELD_ACCESS);
            if(!fa) return NULL;
            fa->left = n;
            Token *fn_tok = cur(p);
            int l = fn_tok->len < RELIC_MAX_IDENT-1 ? fn_tok->len : RELIC_MAX_IDENT-1;
            memcpy(fa->sval, fn_tok->start, l); fa->sval[l]=0;
            advance(p);
            n = fa;
        } else if(check(p, TK_LBRACKET)) {
            advance(p);
            Node *idx = alloc_node(p, ND_INDEX);
            if(!idx) return NULL;
            idx->left  = n;
            idx->right = parse_expr(p);
            if(!idx->right) return NULL;
            if(!expect(p, TK_RBRACKET)) return NULL;
            n = idx;
        } else if(check(p, TK_LPAREN)) {
            advance(p);
            Node *call = alloc_node(p, ND_CALL);
            if(!call) return NULL;
            call->left = n;
            while(!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
                Node *arg = parse_expr(p);
                if(!arg) return NULL;
                if(!add_child(p, call, arg)) return NULL;
                if(!eat(p, TK_COMMA)) break;
            }
            if(!expect(p, TK_RPAREN)) return NULL;
            n = call;
        } else break;
    }
    return n;
}

static Node *parse_unary(Parser *p) {
    TokenKind k = cur(p)->kind;
    if(k==TK_MINUS || k==TK_BANG || k==TK_TILDE || k==TK_STAR || k==TK_AMPERSAND) {
        Node *n = alloc_node(p, ND_UNOP);
        if(!n) return NULL;
        n->op = k;
        advance(p);
        n->left = parse_unary(p);
        if(!n->left) return NULL;
        return n;
    }
    return parse_postfix(p);
}

/* Binary operators by precedence level */
static int binop_prec(TokenKind k) {
    switch(k) {
        case TK_OR:      return 1;
        case TK_AND:     return 2;
        case TK_PIPE:    return 3;
        case TK_CARET:   return 4;
        case TK_AMPERSAND: return 5;
        case TK_EQ: case TK_NEQ: return 6;
        case TK_LT: case TK_LE: case TK_GT: case TK_GE: return 7;
        case TK_LSHIFT: case TK_RSHIFT: return 8;
        case TK_PLUS: case TK_MINUS: return 9;
        case TK_STAR: case TK_SLASH: case TK_PERCENT: return 10;
        default: return 0;
    }
}

static Node *parse_binop(Parser *p, int min_prec) {
    Node *left = parse_unary(p);
    if(!left) return NULL;
    while(true) {
        int prec = binop_prec(cur(p)->kind);
        if(prec <= min_prec) break;
        Node *n = alloc_node(p, ND_BINOP);
        if(!n) return NULL;
        n->op   = cur(p)->kind;
        advance(p);
        n->left  = left;
        n->right = parse_binop(p, prec);
        if(!n->right) return NULL;
        left = n;
    }
    return left;
}

static Node *parse_expr(Parser *p) { return parse_binop(p, 0); }

/* -------------------------------------------------------------------------
 * Statement parsing
 * ---------------------------------------------------------------------- */
/* Array literal element list `[e0, e1, ...]`. Each leaf element becomes
 * an ND_ASSIGN `recv[i] = ei` appended to `seq`; an element that itself
 * starts with `[` recurses with the index chain extended (so a nested
 * literal produces `recv[i][j] = e` assigns). `recv` is the base lvalue
 * expression, shared by every element's index node - evaluation happens
 * once per element, identical to writing the assigns out by hand. */
static bool parse_arrlit(Parser *p, Node *seq, Node *recv) {
    if(!expect(p, TK_LBRACKET)) return false;
    int idx = 0;
    while(!check(p, TK_RBRACKET) && !check(p, TK_EOF)) {
        Node *ir = alloc_node(p, ND_INDEX);
        if(!ir) return false;
        ir->left  = recv;
        Node *lit = alloc_node(p, ND_INT_LIT);
        if(!lit) return false;
        lit->ival = idx;
        ir->right = lit;
        if(check(p, TK_LBRACKET)) {
            /* nested literal: recv[idx][..] = ... */
            if(!parse_arrlit(p, seq, ir)) return false;
        } else {
            Node *e = parse_expr(p);
            if(!e) return false;
            Node *st = alloc_node(p, ND_ASSIGN);
            if(!st) return false;
            st->op    = TK_ASSIGN;
            st->left  = ir;
            st->right = e;
            if(!add_child(p, seq, st)) return false;
        }
        idx++;
        if(!eat(p, TK_COMMA)) break;
    }
    return expect(p, TK_RBRACKET);
}

static Node *parse_block(Parser *p) {
    Node *n = alloc_node(p, ND_BLOCK);
    if(!n) return NULL;
    if(!expect(p, TK_LBRACE)) return NULL;
    while(!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Node *s = parse_stmt(p);
        if(!s) return NULL;
        /* Array-literal desugaring returns an ND_STMT_SEQ: splice its
         * children into the block so a `var` lands in the enclosing
         * scope. */
        if(s->kind == ND_STMT_SEQ) {
            for(int i=0;i<s->nchildren;i++)
                if(!add_child(p, n, s->children[i])) return NULL;
        } else if(!add_child(p, n, s)) return NULL;
    }
    if(!expect(p, TK_RBRACE)) return NULL;
    return n;
}

static bool is_assign_op(TokenKind k) {
    return k==TK_ASSIGN||k==TK_PLUS_ASSIGN||k==TK_MINUS_ASSIGN||k==TK_STAR_ASSIGN||
           k==TK_SLASH_ASSIGN||k==TK_PERCENT_ASSIGN||k==TK_AMP_ASSIGN||
           k==TK_PIPE_ASSIGN||k==TK_CARET_ASSIGN||k==TK_LSHIFT_ASSIGN||k==TK_RSHIFT_ASSIGN;
}

/* Parse a match-arm body.
 *
 * A braced body is an ordinary block. A statement that owns its own
 * terminator (return / break / continue / a nested control-flow statement /
 * a declaration) is parsed by parse_stmt as normal.
 *
 * A bare assignment or expression body is special: the ',' that separates
 * arms (or the '}' that closes the match) acts as the terminator, so the
 * trailing ';' is optional. This makes
 *     0 => y = 5,
 * parse the same as
 *     0 => { y = 5; },
 * which is what the arrow syntax leads users to expect. */
static Node *parse_match_arm_body(Parser *p) {
    if(check(p, TK_LBRACE)) return parse_block(p);

    /* return / break / continue: parse like parse_stmt but the trailing
     * ';' is optional -- in a match arm the ',' (or closing '}') acts as
     * the terminator, so `0 => return 1,` works without a semicolon.
     * if/while/for/match/var/const still go through parse_stmt since they
     * own their own terminators (braces, or a mandatory ';' for decls). */
    if(cur(p)->kind == TK_RETURN) {
        Node *n = alloc_node(p, ND_RETURN);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_SEMICOLON) && !check(p, TK_COMMA) && !check(p, TK_RBRACE)) {
            n->left = parse_expr(p);
            if(!n->left) return NULL;
        }
        eat(p, TK_SEMICOLON);
        return n;
    }
    if(cur(p)->kind == TK_BREAK) {
        Node *n = alloc_node(p, ND_BREAK);
        if(!n) return NULL;
        advance(p);
        eat(p, TK_SEMICOLON);
        return n;
    }
    if(cur(p)->kind == TK_CONTINUE) {
        Node *n = alloc_node(p, ND_CONTINUE);
        if(!n) return NULL;
        advance(p);
        eat(p, TK_SEMICOLON);
        return n;
    }

    switch(cur(p)->kind) {
        case TK_IF: case TK_WHILE: case TK_FOR: case TK_MATCH:
        case TK_VAR: case TK_CONST:
            return parse_stmt(p);
        default: break;
    }

    Node *lhs = parse_expr(p);
    if(!lhs) return NULL;
    Node *n;
    if(is_assign_op(cur(p)->kind)) {
        n = alloc_node(p, ND_ASSIGN);
        if(!n) return NULL;
        n->op    = cur(p)->kind;
        advance(p);
        n->left  = lhs;
        n->right = parse_expr(p);
        if(!n->right) return NULL;
    } else {
        n = alloc_node(p, ND_EXPR_STMT);
        if(!n) return NULL;
        n->left = lhs;
    }
    eat(p, TK_SEMICOLON);   /* optional: ',' or '}' also terminates */
    return n;
}

static Node *parse_stmt(Parser *p) {
    Token *t = cur(p);

    /* Block */
    if(t->kind == TK_LBRACE) return parse_block(p);

    /* var decl */
    if(t->kind == TK_VAR) {
        Node *n = alloc_node(p, ND_VAR_DECL);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected identifier",cur(p)->line); return NULL; }
        Token *nm = cur(p);
        int l = nm->len < RELIC_MAX_IDENT-1 ? nm->len : RELIC_MAX_IDENT-1;
        memcpy(n->sval, nm->start, l); n->sval[l]=0;
        advance(p);
        if(!expect(p, TK_COLON)) return NULL;
        n->type = parse_type(p);
        if(!n->type) return NULL;
        if(eat(p, TK_ASSIGN)) {
            /* `var name : TYPE = [e0, e1, ...]` array literal: desugars
             * to the bare decl plus one `name[i] = ei` assignment per
             * element. */
            if(check(p, TK_LBRACKET)) {
                Node *seq = alloc_node(p, ND_STMT_SEQ);
                if(!seq) return NULL;
                if(!add_child(p, seq, n)) return NULL;
                Node *recv = alloc_node(p, ND_IDENT);
                if(!recv) return NULL;
                memcpy(recv->sval, n->sval, strlen(n->sval)+1);
                if(!parse_arrlit(p, seq, recv)) return NULL;
                if(!expect(p, TK_SEMICOLON)) return NULL;
                return seq;
            }
            n->left = parse_expr(p);
            if(!n->left) return NULL;
        }
        if(!expect(p, TK_SEMICOLON)) return NULL;
        return n;
    }

    /* const decl */
    if(t->kind == TK_CONST) {
        Node *n = alloc_node(p, ND_CONST_DECL);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected identifier",cur(p)->line); return NULL; }
        Token *nm = cur(p);
        int l = nm->len < RELIC_MAX_IDENT-1 ? nm->len : RELIC_MAX_IDENT-1;
        memcpy(n->sval, nm->start, l); n->sval[l]=0;
        advance(p);
        if(!expect(p, TK_COLON)) return NULL;
        n->type = parse_type(p);
        if(!n->type) return NULL;
        if(!expect(p, TK_ASSIGN)) return NULL;
        n->left = parse_expr(p);
        if(!n->left) return NULL;
        if(!expect(p, TK_SEMICOLON)) return NULL;
        return n;
    }

    /* if */
    if(t->kind == TK_IF) {
        Node *n = alloc_node(p, ND_IF);
        if(!n) return NULL;
        advance(p);
        n->cond = parse_expr(p);
        if(!n->cond) return NULL;
        n->left = parse_block(p);
        if(!n->left) return NULL;
        if(eat(p, TK_ELSE)) {
            if(check(p, TK_IF)) n->right = parse_stmt(p);
            else                n->right = parse_block(p);
            if(!n->right) return NULL;
        }
        return n;
    }

    /* while */
    if(t->kind == TK_WHILE) {
        Node *n = alloc_node(p, ND_WHILE);
        if(!n) return NULL;
        advance(p);
        n->cond = parse_expr(p);
        if(!n->cond) return NULL;
        n->left = parse_block(p);
        if(!n->left) return NULL;
        return n;
    }

    /* for IDENT in expr..expr { } */
    if(t->kind == TK_FOR) {
        Node *n = alloc_node(p, ND_FOR);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected loop var",cur(p)->line); return NULL; }
        Token *nm = cur(p);
        int l = nm->len < RELIC_MAX_IDENT-1 ? nm->len : RELIC_MAX_IDENT-1;
        memcpy(n->sval, nm->start, l); n->sval[l]=0;
        advance(p);
        if(!expect(p, TK_IN)) return NULL;
        Node *range = alloc_node(p, ND_RANGE);
        if(!range) return NULL;
        range->left = parse_expr(p);
        if(!range->left) return NULL;
        if(!expect(p, TK_DOTDOT)) return NULL;
        range->right = parse_expr(p);
        if(!range->right) return NULL;
        n->cond = range;
        n->left = parse_block(p);
        if(!n->left) return NULL;
        return n;
    }

    /* match */
    if(t->kind == TK_MATCH) {
        Node *n = alloc_node(p, ND_MATCH);
        if(!n) return NULL;
        advance(p);
        n->cond = parse_expr(p);
        if(!n->cond) return NULL;
        if(!expect(p, TK_LBRACE)) return NULL;
        while(!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
            Node *arm = alloc_node(p, ND_MATCH_ARM);
            if(!arm) return NULL;
            /* pattern: '_' (wildcard), or one or more values separated by
             * '|'. The first value lives in arm->cond and any alternatives
             * in arm->children[], so `0 | 1 | 2 => body` runs body when the
             * subject equals any of them.
             *
             * Patterns are parsed at a precedence that stops before '|'
             * (TK_PIPE, level 3) so the bar reads as an alternative
             * separator rather than bitwise-or. A pattern that genuinely
             * needs bitwise-or can parenthesise it: `(A | B) => ...`. */
            if(cur(p)->kind == TK_IDENT &&
               cur(p)->len == 1 && cur(p)->start[0] == '_') {
                arm->bval = true; /* wildcard */
                advance(p);
            } else {
                arm->cond = parse_binop(p, binop_prec(TK_PIPE));
                if(!arm->cond) return NULL;
                while(eat(p, TK_PIPE)) {
                    Node *alt = parse_binop(p, binop_prec(TK_PIPE));
                    if(!alt) return NULL;
                    if(!add_child(p, arm, alt)) return NULL;
                }
            }
            if(!expect(p, TK_FAT_ARROW)) return NULL;
            arm->left = parse_match_arm_body(p);
            if(!arm->left) return NULL;
            eat(p, TK_COMMA);
            if(!add_child(p, n, arm)) return NULL;
        }
        if(!expect(p, TK_RBRACE)) return NULL;
        return n;
    }

    /* return */
    if(t->kind == TK_RETURN) {
        Node *n = alloc_node(p, ND_RETURN);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_SEMICOLON)) {
            n->left = parse_expr(p);
            if(!n->left) return NULL;
        }
        if(!expect(p, TK_SEMICOLON)) return NULL;
        return n;
    }

    /* break / continue */
    if(t->kind == TK_BREAK) {
        Node *n = alloc_node(p, ND_BREAK);
        if(!n) return NULL;
        advance(p);
        if(!expect(p, TK_SEMICOLON)) return NULL;
        return n;
    }
    if(t->kind == TK_CONTINUE) {
        Node *n = alloc_node(p, ND_CONTINUE);
        if(!n) return NULL;
        advance(p);
        if(!expect(p, TK_SEMICOLON)) return NULL;
        return n;
    }

    /* Expression statement or assignment */
    {
        Node *lhs = parse_expr(p);
        if(!lhs) return NULL;

        if(is_assign_op(cur(p)->kind)) {
            /* `lvalue = [e0, e1, ...]` array-literal assignment: one
             * `lhs[i] = ei` per element. The parsed lhs node is shared
             * across every generated INDEX - the same evaluation shape
             * as writing the assigns out by hand. Any lvalue works:
             * `a`, `s.arr`, `arr[0]`, `(*p).arr`. */
            if(cur(p)->kind == TK_ASSIGN &&
               p->pos + 1 < p->ntokens &&
               p->tokens[p->pos + 1].kind == TK_LBRACKET) {
                advance(p); /* `=` */
                Node *seq = alloc_node(p, ND_STMT_SEQ);
                if(!seq) return NULL;
                if(!parse_arrlit(p, seq, lhs)) return NULL;
                if(!expect(p, TK_SEMICOLON)) return NULL;
                return seq;
            }
            Node *n = alloc_node(p, ND_ASSIGN);
            if(!n) return NULL;
            n->op   = cur(p)->kind;
            advance(p);
            n->left  = lhs;
            n->right = parse_expr(p);
            if(!n->right) return NULL;
            if(!expect(p, TK_SEMICOLON)) return NULL;
            return n;
        }

        Node *n = alloc_node(p, ND_EXPR_STMT);
        if(!n) return NULL;
        n->left = lhs;
        if(!expect(p, TK_SEMICOLON)) return NULL;
        return n;
    }
}

/* -------------------------------------------------------------------------
 * Top-level declarations
 * ---------------------------------------------------------------------- */

/* Parse `(params) [-> ret]` onto an already-named ND_FN_DECL (no body).
 * Shared by top-level `fn` decls, class methods (body or `;`) and
 * out-of-class `fn Class.m` definitions so all have the same signature
 * grammar. */
static bool parse_fn_signature(Parser *p, Node *n) {
    if(!expect(p, TK_LPAREN)) return false;
    while(!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
        Node *param = alloc_node(p, ND_PARAM);
        if(!param) return false;
        if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected param name",cur(p)->line); return false; }
        Token *pn = cur(p);
        int l = pn->len < RELIC_MAX_IDENT-1 ? pn->len : RELIC_MAX_IDENT-1;
        memcpy(param->sval, pn->start, l); param->sval[l]=0;
        advance(p);
        if(!expect(p, TK_COLON)) return false;
        param->type = parse_type(p);
        if(!param->type) return false;
        eat(p, TK_COMMA);
        if(!add_child(p, n, param)) return false;
    }
    if(!expect(p, TK_RPAREN)) return false;
    if(eat(p, TK_ARROW)) {
        n->type = parse_type(p);
        if(!n->type) return false;
    } else {
        n->type = alloc_type(TY_VOID);
    }
    return true;
}

/* Signature plus `{ body }`. */
static bool parse_fn_tail(Parser *p, Node *n) {
    if(!parse_fn_signature(p, n)) return false;
    n->left = parse_block(p);
    return n->left != NULL;
}

/* Apply the method desugaring to a method fn whose signature (and
 * optionally body) has been parsed.
 * `cname`/`mname` are the source-level class/method names:
 *   ctor      -> `fn new(...) -> Class` gets ND_MF_CTOR plus an injected
 *                `var self : Class;` at the body top and an implicit
 *                `return self;` at the end when the body has no return
 *                anywhere (skipped for `;` prototypes, which have no body)
 *   non-ctor  -> `self : *Class` injected as the first parameter */
static bool desugar_method(Parser *p, Node *n,
                           const char *cname, const char *mname) {
    if(strcmp(mname, "new") == 0 &&
       n->type && n->type->kind == TY_STRUCT &&
       strcmp(n->type->name, cname) == 0) {
        /* Constructor: returns the class by value, runs on an injected
         * `var self : Class` that is returned implicitly. */
        n->mflags |= ND_MF_CTOR;
        Node *body = n->left;
        if(body && !node_tree_has(body, pred_var_self)) {
            Node *decl = alloc_node(p, ND_VAR_DECL);
            if(!decl) return false;
            memcpy(decl->sval, "self", 5);
            Type *ct = alloc_type(TY_STRUCT);
            if(!ct) return false;
            memcpy(ct->name, cname, strlen(cname)+1);
            decl->type = ct;
            if(!prepend_child(body, decl)) {
                snprintf(p->err, sizeof(p->err), "out of memory");
                return false;
            }
        }
        if(body && !node_tree_has(body, pred_return)) {
            Node *ret = alloc_node(p, ND_RETURN);
            Node *sid = alloc_node(p, ND_IDENT);
            if(!ret || !sid) return false;
            memcpy(sid->sval, "self", 5);
            ret->left = sid;
            if(!add_child(p, body, ret)) {
                snprintf(p->err, sizeof(p->err), "out of memory");
                return false;
            }
        }
    } else {
        /* Ordinary method: `self : *Class` first parameter. */
        Node *sp = alloc_node(p, ND_PARAM);
        if(!sp) return false;
        memcpy(sp->sval, "self", 5);
        Type *ct = alloc_type(TY_STRUCT);
        Type *pt = alloc_type(TY_PTR);
        if(!ct || !pt) return false;
        memcpy(ct->name, cname, strlen(cname)+1);
        pt->inner = ct;
        sp->type = pt;
        if(!prepend_child(n, sp)) {
            snprintf(p->err, sizeof(p->err), "out of memory");
            return false;
        }
    }
    return true;
}

/* `fn name(params) [-> ret] [;|{ body }]` inside a class body. Builds
 * the desugared top-level ND_FN_DECL (`Class__name`, injected `self`
 * param / ctor boilerplate). A `;` after
 * the signature makes it a prototype (ND_MF_PROTO): it records the
 * member's signature for a `fn Class.name` definition elsewhere and is
 * not appended to the program's children (no fn_id, no body). */
static Node *parse_class_method(Parser *p, Node *cls) {
    Node *n = alloc_node(p, ND_FN_DECL);
    if(!n) return NULL;
    advance(p); /* `fn` */
    if(!check(p, TK_IDENT)) {
        snprintf(p->err, sizeof(p->err), "line %d: expected method name", cur(p)->line);
        return NULL;
    }
    Token *mn = cur(p);
    char mname[RELIC_MAX_IDENT];
    {
        int l = mn->len < RELIC_MAX_IDENT-1 ? mn->len : RELIC_MAX_IDENT-1;
        memcpy(mname, mn->start, l); mname[l]=0;
    }
    advance(p);
    int rc = snprintf(n->sval, RELIC_MAX_IDENT, "%s__%s", cls->sval, mname);
    if(rc <= 0 || rc >= RELIC_MAX_IDENT) {
        snprintf(p->err, sizeof(p->err), "line %d: method name too long", mn->line);
        return NULL;
    }
    n->owner  = cls;
    n->mflags = ND_MF_METHOD;
    if(!parse_fn_signature(p, n)) return NULL;
    if(eat(p, TK_SEMICOLON)) {
        n->mflags |= ND_MF_PROTO;
    } else {
        n->left = parse_block(p);
        if(!n->left) return NULL;
    }
    if(!desugar_method(p, n, cls->sval, mname)) return NULL;
    return n;
}

static Node *parse_decl(Parser *p) {
    Token *t = cur(p);

    /* struct */
    if(t->kind == TK_STRUCT) {
        Node *n = alloc_node(p, ND_STRUCT_DECL);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected struct name",cur(p)->line); return NULL; }
        Token *nm = cur(p);
        int l = nm->len < RELIC_MAX_IDENT-1 ? nm->len : RELIC_MAX_IDENT-1;
        memcpy(n->sval, nm->start, l); n->sval[l]=0;
        advance(p);
        if(!expect(p, TK_LBRACE)) return NULL;
        while(!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
            Node *f = alloc_node(p, ND_FIELD);
            if(!f) return NULL;
            if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected field name",cur(p)->line); return NULL; }
            Token *fn_tok = cur(p);
            l = fn_tok->len < RELIC_MAX_IDENT-1 ? fn_tok->len : RELIC_MAX_IDENT-1;
            memcpy(f->sval, fn_tok->start, l); f->sval[l]=0;
            advance(p);
            if(!expect(p, TK_COLON)) return NULL;
            f->type = parse_type(p);
            if(!f->type) return NULL;
            eat(p, TK_COMMA);
            if(!add_child(p, n, f)) return NULL;
        }
        if(!expect(p, TK_RBRACE)) return NULL;
        return n;
    }

    /* fn */
    if(t->kind == TK_FN) {
        Node *n = alloc_node(p, ND_FN_DECL);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected function name",cur(p)->line); return NULL; }
        Token *nm = cur(p);
        char cname[RELIC_MAX_IDENT];
        {
            int l = nm->len < RELIC_MAX_IDENT-1 ? nm->len : RELIC_MAX_IDENT-1;
            memcpy(cname, nm->start, l); cname[l]=0;
        }
        advance(p);

        /* Out-of-class method definition `fn Class.method(params)
         * [-> ret] { body }`. Builds the same
         * desugared Class__method node an in-class method produces; the
         * ->owner link is resolved by the type checker (the class may be
         * declared later in the file or in an imported module). `fn
         * IDENT . IDENT (` is unambiguous at declaration position - a
         * plain `fn name(...)` never has '.' after the name. */
        if(check(p, TK_DOT)) {
            Token *p1 = p->pos + 1 < p->ntokens ? &p->tokens[p->pos + 1] : cur(p);
            Token *p2 = p->pos + 2 < p->ntokens ? &p->tokens[p->pos + 2] : cur(p);
            if(p1->kind == TK_IDENT && p2->kind == TK_LPAREN) {
                advance(p); /* `.` */
                char mname[RELIC_MAX_IDENT];
                {
                    int l = p1->len < RELIC_MAX_IDENT-1 ? p1->len : RELIC_MAX_IDENT-1;
                    memcpy(mname, p1->start, l); mname[l]=0;
                }
                advance(p); /* method name */
                int rc = snprintf(n->sval, RELIC_MAX_IDENT, "%s__%s",
                                  cname, mname);
                if(rc <= 0 || rc >= RELIC_MAX_IDENT) {
                    snprintf(p->err, sizeof(p->err),
                             "line %d: method name too long", nm->line);
                    return NULL;
                }
                n->mflags = ND_MF_METHOD | ND_MF_OUTCLASS;
                if(!parse_fn_signature(p, n)) return NULL;
                n->left = parse_block(p);
                if(!n->left) return NULL;
                if(!desugar_method(p, n, cname, mname)) return NULL;
                return n;
            }
        }
        memcpy(n->sval, cname, strlen(cname)+1);
        if(!parse_fn_tail(p, n)) return NULL;
        return n;
    }

    /* class NAME [implicit] { fields, methods }. The class node keeps
     * only the field children (so it lays out exactly like a struct);
     * methods are desugared to top-level ND_FN_DECLs and
     * recorded in ->members for relic_parse to append to the program
     * right after the class node. */
    if(t->kind == TK_CLASS) {
        Node *n = alloc_node(p, ND_CLASS_DECL);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected class name",cur(p)->line); return NULL; }
        Token *nm = cur(p);
        int l = nm->len < RELIC_MAX_IDENT-1 ? nm->len : RELIC_MAX_IDENT-1;
        memcpy(n->sval, nm->start, l); n->sval[l]=0;
        advance(p);
        if(check(p, TK_IDENT) && cur(p)->len == 8 &&
           memcmp(cur(p)->start, "implicit", 8) == 0) {
            n->bval = true;
            advance(p);
        }
        if(!expect(p, TK_LBRACE)) return NULL;
        while(!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
            if(cur(p)->kind == TK_FN) {
                Node *m = parse_class_method(p, n);
                if(!m) return NULL;
                if(!add_member(n, m)) {
                    snprintf(p->err, sizeof(p->err), "out of memory");
                    return NULL;
                }
            } else if(cur(p)->kind == TK_IDENT) {
                Node *f = alloc_node(p, ND_FIELD);
                if(!f) return NULL;
                Token *fn_tok = cur(p);
                int l = fn_tok->len < RELIC_MAX_IDENT-1 ? fn_tok->len : RELIC_MAX_IDENT-1;
                memcpy(f->sval, fn_tok->start, l); f->sval[l]=0;
                advance(p);
                if(!expect(p, TK_COLON)) return NULL;
                f->type = parse_type(p);
                if(!f->type) return NULL;
                if(!add_child(p, n, f)) return NULL;
            } else {
                snprintf(p->err,sizeof(p->err),"line %d: expected field or method in class body, got '%s'",
                         cur(p)->line, relic_token_name(cur(p)->kind));
                return NULL;
            }
            eat(p, TK_COMMA);
        }
        if(!expect(p, TK_RBRACE)) return NULL;
        return n;
    }

    /* const */
    if(t->kind == TK_CONST) {
        Node *n = alloc_node(p, ND_CONST_DECL);
        if(!n) return NULL;
        advance(p);
        if(!check(p, TK_IDENT)) { snprintf(p->err,sizeof(p->err),"line %d: expected const name",cur(p)->line); return NULL; }
        Token *nm = cur(p);
        int l = nm->len < RELIC_MAX_IDENT-1 ? nm->len : RELIC_MAX_IDENT-1;
        memcpy(n->sval, nm->start, l); n->sval[l]=0;
        advance(p);
        if(!expect(p, TK_COLON)) return NULL;
        n->type = parse_type(p);
        if(!n->type) return NULL;
        if(!expect(p, TK_ASSIGN)) return NULL;
        n->left = parse_expr(p);
        if(!n->left) return NULL;
        if(!expect(p, TK_SEMICOLON)) return NULL;
        return n;
    }

    /* top-level var */
    if(t->kind == TK_VAR) {
        Node *n = parse_stmt(p);   /* parse_stmt handles var_decl + semicolon */
        if(n) n->kind = ND_VAR_DECL; /* already correct kind */
        return n;
    }

    snprintf(p->err, sizeof(p->err), "line %d: expected declaration (fn/struct/class/const/var), got '%s'",
             t->line, relic_token_name(t->kind));
    return NULL;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */
Node *relic_parse(Parser *p, Token *tokens, int ntokens) {
    p->tokens  = tokens;
    p->ntokens = ntokens;
    p->pos     = 0;
    p->npool   = 0;
    p->err[0]  = 0;
    p->err_line= 0;

    Node *prog = alloc_node(p, ND_PROGRAM);
    if(!prog) return NULL;

    while(!check(p, TK_EOF)) {
        Node *d = parse_decl(p);
        if(!d) return NULL;
        if(!add_child(p, prog, d)) return NULL;
        /* Class methods are desugared to top-level fn decls that follow
         * the class node (fields first, then methods in source order).
         * `;` prototypes (ND_MF_PROTO) are member records only: no body,
         * no fn_id - they never become program children. */
        for(int i = 0; i < d->nmembers; i++) {
            if(d->members[i]->mflags & ND_MF_PROTO) continue;
            if(!add_child(p, prog, d->members[i])) return NULL;
        }
    }
    return prog;
}
