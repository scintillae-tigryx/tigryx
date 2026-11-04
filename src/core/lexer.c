/*
 * lexer.c
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
 * lexer.c - Relic tokeniser
 */
#include "tigryx.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef RELIC_SOFTFP
/*
 * Soft-float builds still link the platform libc, whose strtod is x87
 * code (DJGPP: exception-7 -> emu387).  On FPU-less hardware that works
 * but is extremely slow per literal, and under emulators that do not
 * model the coprocessor trap (DOSBox) it returns garbage.
 *
 * Literals parse via softfp_strtod (src/softfp/softfp_strtod.c): a
 * correctly-rounded decimal->double implemented on a small big-integer,
 * so an f64 literal compiled ON a soft-float host is bit-identical to one
 * compiled on an FPU host: correctly rounded, not few-ulp.
 */
double softfp_strtod(const char *s);
#endif

/* -------------------------------------------------------------------------
 * Keyword table
 * ---------------------------------------------------------------------- */
static const struct { const char *word; TokenKind kind; } kw_table[] = {
    { "bool",     TK_BOOL     },
    { "break",    TK_BREAK    },
    { "cast",     TK_CAST     },
    { "class",    TK_CLASS    },
    { "const",    TK_CONST    },
    { "continue", TK_CONTINUE },
    { "else",     TK_ELSE     },
    { "f32",      TK_F32      },
    { "f64",      TK_F64      },
    { "false",    TK_FALSE    },
    { "fn",       TK_FN       },
    { "for",      TK_FOR      },
    { "if",       TK_IF       },
    { "i8",       TK_I8       },
    { "i16",      TK_I16      },
    { "i32",      TK_I32      },
    { "in",       TK_IN       },
    { "match",    TK_MATCH    },
    { "ref",      TK_REF      },
    { "return",   TK_RETURN   },
    { "struct",   TK_STRUCT   },
    { "true",     TK_TRUE     },
    { "u8",       TK_U8       },
    { "u16",      TK_U16      },
    { "u32",      TK_U32      },
    { "var",      TK_VAR      },
    { "while",    TK_WHILE    },
    { NULL,       TK_EOF      }
};

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static bool is_alpha(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static bool is_digit(char c) { return c>='0'&&c<='9'; }
static bool is_alnum(char c) { return is_alpha(c)||is_digit(c); }
static bool is_hex(char c)   { return is_digit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }

static char lx_peek(Lexer *lx)        { return lx->pos < lx->len ? lx->src[lx->pos] : 0; }
static char lx_peek2(Lexer *lx)       { return lx->pos+1 < lx->len ? lx->src[lx->pos+1] : 0; }
static char lx_advance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if(c=='\n') { lx->line++; lx->line_start = lx->pos; }
    return c;
}
static bool lx_match(Lexer *lx, char c) { if(lx_peek(lx)==c){lx_advance(lx);return true;}return false; }

/* Record a lex error: message (with the historical "line N:" prefix) plus
 * the structured location fields. The line/file/col are captured at the
 * point the fault is *recognised* - callers pass the construct's start
 * line, since scanning may already have run to EOF by the time the error
 * is raised (unterminated comment/string). */
static void lx_err(Lexer *lx, int line, int col, const char *fmt, ...) {
    int n = snprintf(lx->err, sizeof(lx->err), "line %d: ", line);
    if(n < 0) n = 0;
    if(n >= (int)sizeof(lx->err)) n = (int)sizeof(lx->err) - 1;
    va_list ap; va_start(ap, fmt);
    vsnprintf(lx->err + n, sizeof(lx->err) - (size_t)n, fmt, ap);
    va_end(ap);
    lx->err_line    = line;
    lx->err_file_id = lx->cur_file;
    lx->err_col     = col;
}

/* Intern a source file path into the lexer's small file table, returning
 * its id (an existing entry's id if already interned). File counts are
 * tiny in practice (a handful of #import targets), so linear search is
 * fine. Saturates at RELIC_MAX_SOURCE_FILES rather than growing/failing -
 * running out just means later files share the last slot's id, degrading
 * file-attribution accuracy rather than the compile itself. */
static int lexer_intern_file(Lexer *lx, const char *path, int path_len) {
    char pb[RELIC_MAX_SOURCE_PATH];
    if(path_len >= RELIC_MAX_SOURCE_PATH) path_len = RELIC_MAX_SOURCE_PATH - 1;
    memcpy(pb, path, (size_t)path_len);
    pb[path_len] = '\0';
#ifdef RELIC_IA16
    /* File table is far-allocated lazily (8K can't live in the near heap). */
    if(!lx->files) {
        lx->files = (char RELIC_FAR (*)[RELIC_MAX_SOURCE_PATH])
            relic_far_alloc((u32)RELIC_MAX_SOURCE_FILES * RELIC_MAX_SOURCE_PATH);
        if(!lx->files) return -1;   /* degrade to "unknown file" ids */
        relic_far_set((u8 RELIC_FAR *)lx->files, 0, 0,
                      (u32)RELIC_MAX_SOURCE_FILES * RELIC_MAX_SOURCE_PATH);
    }
#endif
    for(int i=0; i<lx->nfiles; i++) {
        char eb[RELIC_MAX_SOURCE_PATH];
        relic_far_str(eb, LXFILE(lx, i), sizeof eb);
        if(strcmp(eb, pb) == 0)
            return i;
    }
    if(lx->nfiles >= RELIC_MAX_SOURCE_FILES) return lx->nfiles - 1;
    int id = lx->nfiles++;
    relic_far_write((u8 RELIC_FAR *)lx->files, (u32)id * RELIC_MAX_SOURCE_PATH,
                    pb, (u32)path_len + 1);
    return id;
}

static int hex_val(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    return c-'A'+10;
}

static Token *push_token(Lexer *lx, TokenKind kind, const char *start, int len, int line) {
    if(lx->ntokens >= lx->capacity) {
        int new_capacity = lx->capacity * 2;
        Token *new_tokens = realloc(lx->tokens, new_capacity * sizeof(Token));
        if(!new_tokens) {
            snprintf(lx->err, sizeof(lx->err), "out of memory reallocating token array");
            return NULL;
        }
        lx->tokens = new_tokens;
        lx->capacity = new_capacity;
    }
    Token *t = &lx->tokens[lx->ntokens++];
    t->kind    = kind;
    t->start   = start;
    t->len     = len;
    t->line    = line;
    t->file_id = lx->cur_file;
    t->col     = (int)(start - &lx->src[lx->line_start]) + 1;
    t->end_col = t->col + len;
    t->f64lit   = 0;
    t->val.ival = 0;
    return t;
}

/* -------------------------------------------------------------------------
 * Escape sequence decoder (in-place, returns decoded char or -1 on error)
 * ---------------------------------------------------------------------- */
static int decode_escape(Lexer *lx) {
    char c = lx_advance(lx);
    switch(c) {
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case '0':  return 0;
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        case 'x': {
            char h0 = lx_peek(lx);
            if (!is_hex(h0)) {
                lx_err(lx, lx->line, lx->pos - lx->line_start + 1, "invalid \\x escape");
                return -1;
            }
            int hi = hex_val(lx_advance(lx));
            char h1 = lx_peek(lx);
            if (!is_hex(h1)) {
                lx_err(lx, lx->line, lx->pos - lx->line_start + 1, "invalid \\x escape");
                return -1;
            }
            int lo = hex_val(lx_advance(lx));
            return hi * 16 + lo;
        }
        default:
            lx_err(lx, lx->line, lx->pos - lx->line_start + 1, "unknown escape '\\%c'", c);
            return -1;
    }
}

/* -------------------------------------------------------------------------
 * Main lex function
 * ---------------------------------------------------------------------- */
bool relic_lex(Lexer *lx, const char *src, int len, const char *initial_path) {
    lx->src        = src;
    lx->pos        = 0;
    lx->len        = len;
    lx->line       = 1;
    lx->line_start = 0;
    lx->ntokens    = 0;
    lx->err[0]     = 0;
    lx->err_line   = 0;
    lx->err_file_id= -1;
    lx->err_col    = -1;
    lx->nfiles     = 0;
    lx->cur_file   = -1;
    if(initial_path && *initial_path)
        lx->cur_file = lexer_intern_file(lx, initial_path, (int)strlen(initial_path));

    while(lx->pos < lx->len) {
        /* Skip whitespace */
        char c = lx_peek(lx);
        if(c==' '||c=='\t'||c=='\r'||c=='\n') { lx_advance(lx); continue; }

        /* Line comment */
        if(c=='/' && lx_peek2(lx)=='/') {
            while(lx->pos < lx->len && lx_peek(lx)!='\n') lx_advance(lx);
            continue;
        }

        /* Preprocessor line directive: `#line <N>` resets the current line
         * number to N so that subsequent tokens are reported as if they came
         * from line N of the original source. Emitted by tigryxpp to preserve
         * source-level diagnostics across preprocessing/lowering. Any other
         * `#...` directive is silently treated as a line comment. */
        if(c=='#') {
            int directive_start_pos = lx->pos;
            lx_advance(lx); /* consume '#' */
            /* Match the word "line" */
            if(lx->pos + 4 <= lx->len &&
               lx->src[lx->pos]   == 'l' &&
               lx->src[lx->pos+1] == 'i' &&
               lx->src[lx->pos+2] == 'n' &&
               lx->src[lx->pos+3] == 'e' &&
               (lx->pos + 4 == lx->len ||
                lx->src[lx->pos+4] == ' ' ||
                lx->src[lx->pos+4] == '\t')) {
                lx->pos += 4; /* consume "line" */
                while(lx->pos < lx->len &&
                      (lx_peek(lx)==' '||lx_peek(lx)=='\t')) lx->pos++;
                int n = 0;
                bool got_digit = false;
                while(lx->pos < lx->len && is_digit(lx_peek(lx))) {
                    n = n*10 + (lx->src[lx->pos] - '0');
                    lx->pos++;
                    got_digit = true;
                }
                /* Optional filename: `#line N "path"` (C-compatible form).
                 * tigryxpp emits this whenever the source file changes
                 * (e.g. crossing a #import boundary) so tokens get correct
                 * per-file attribution instead of just a line number that
                 * resets/overlaps across files. A bare `#line N` (no
                 * filename) leaves the current file unchanged. */
                while(lx->pos < lx->len && (lx_peek(lx)==' '||lx_peek(lx)=='\t')) lx->pos++;
                if(lx_peek(lx) == '"') {
                    lx->pos++; /* consume opening quote */
                    int fstart = lx->pos;
                    while(lx->pos < lx->len && lx_peek(lx) != '"' && lx_peek(lx) != '\n') lx->pos++;
                    int flen = lx->pos - fstart;
                    if(lx_peek(lx) == '"') {
                        lx->cur_file = lexer_intern_file(lx, &lx->src[fstart], flen);
                        lx->pos++; /* consume closing quote */
                    }
                }
                /* Skip the rest of the directive line. */
                while(lx->pos < lx->len && lx_peek(lx)!='\n') lx->pos++;
                if(lx->pos < lx->len) lx->pos++; /* consume '\n' */
                if(got_digit) lx->line = n;
                lx->line_start = lx->pos;
                continue;
            }
            /* Unknown directive — rewind and treat as line comment. */
            lx->pos = directive_start_pos;
            while(lx->pos < lx->len && lx_peek(lx)!='\n') lx_advance(lx);
            continue;
        }

        /* Block comment (nestable) */
        if(c=='/' && lx_peek2(lx)=='*') {
            int start_line = lx->line;
            int start_col  = lx->pos - lx->line_start + 1;
            lx_advance(lx); lx_advance(lx);
            int depth = 1;
            while(lx->pos < lx->len && depth > 0) {
                if(lx_peek(lx)=='/' && lx_peek2(lx)=='*') { lx_advance(lx); lx_advance(lx); depth++; }
                else if(lx_peek(lx)=='*' && lx_peek2(lx)=='/') { lx_advance(lx); lx_advance(lx); depth--; }
                else lx_advance(lx);
            }
            if(depth > 0) {
                lx_err(lx, start_line, start_col, "unterminated block comment");
                return false;
            }
            continue;
        }

        int line = lx->line;
        const char *start = &lx->src[lx->pos];
        int tok_col = lx->pos - lx->line_start + 1;
        c = lx_advance(lx);

        /* Numeric literals (integer or float) */
        if(is_digit(c)) {
            i32 val = 0;
            bool is_float = false;
            if(c=='0' && (lx_peek(lx)=='x'||lx_peek(lx)=='X')) {
                lx_advance(lx);
                while(is_hex(lx_peek(lx))) val = val*16 + hex_val(lx_advance(lx));
            } else if(c=='0' && (lx_peek(lx)=='b'||lx_peek(lx)=='B')) {
                lx_advance(lx);
                while(lx_peek(lx)=='0'||lx_peek(lx)=='1') val = val*2 + (lx_advance(lx)-'0');
            } else {
                val = c - '0';
                while(is_digit(lx_peek(lx))) val = val*10 + (lx_advance(lx)-'0');
                /* Float? A '.' followed by a digit (not '..' range), or 'e'/'E', or 'f' suffix. */
                if(lx_peek(lx)=='.' && is_digit(lx_peek2(lx))) {
                    is_float = true;
                    lx_advance(lx); /* '.' */
                    while(is_digit(lx_peek(lx))) lx_advance(lx);
                }
                if(lx_peek(lx)=='e' || lx_peek(lx)=='E') {
                    is_float = true;
                    lx_advance(lx);
                    if(lx_peek(lx)=='+'||lx_peek(lx)=='-') lx_advance(lx);
                    while(is_digit(lx_peek(lx))) lx_advance(lx);
                }
                /* 'f'/'F' forces f32 (also the unsuffixed default), 'd'/'D'
                 * forces f64. A suffix is what makes an integer-looking
                 * literal float: 1d is the double 1.0, like 1f is f32. */
                if(lx_peek(lx)=='f' || lx_peek(lx)=='F' ||
                   lx_peek(lx)=='d' || lx_peek(lx)=='D') {
                    is_float = true;
                    lx_advance(lx);
                }
            }
            int tlen = (int)(&lx->src[lx->pos] - start);
            if(is_float) {
                /* parse via strtof on a temporary nul-terminated copy */
                char buf[64];
                int copy_len = tlen;
                /* drop a trailing 'f'/'F'/'d'/'D' so strtod parses the
                 * mantissa cleanly */
                if(copy_len > 0 && (start[copy_len-1]=='f' || start[copy_len-1]=='F' ||
                                    start[copy_len-1]=='d' || start[copy_len-1]=='D')) copy_len--;
                if(copy_len >= (int)sizeof(buf)) copy_len = sizeof(buf)-1;
                memcpy(buf, start, copy_len); buf[copy_len] = 0;
                Token *t = push_token(lx, TK_FLOAT, start, tlen, line);
                if(!t) return false;
                t->f64lit = (tlen > 0 &&
                             (start[tlen-1]=='d' || start[tlen-1]=='D')) ? 1 : 0;
#ifdef RELIC_SOFTFP
                /* Correctly-rounded soft decimal->double; identical bits
                 * to the libc strtod used on FPU hosts, so f64 literals
                 * compiled on a soft-float host are not a few ulp out. */
                t->val.fval = softfp_strtod(buf);
#else
                /* Full double precision retained; the emitter narrows to f32
                 * only when the target type is f32. Narrowing here would
                 * silently corrupt every f64 literal. */
                t->val.fval = strtod(buf, NULL);
#endif
            } else {
                Token *t = push_token(lx, TK_INT, start, tlen, line);
                if(!t) return false;
                t->val.ival = val;
            }
            continue;
        }

        /* Char literals */
        if(c=='\'') {
            i32 val = 0;
            if(lx_peek(lx)=='\\') { lx_advance(lx); int v = decode_escape(lx); if(v<0) return false; val=v; }
            else if(lx_peek(lx)!='\'') val = lx_advance(lx);
            if(lx_peek(lx)!='\'') {
                lx_err(lx, line, tok_col, "unterminated char literal");
                return false;
            }
            lx_advance(lx);
            int tlen = (int)(&lx->src[lx->pos] - start);
            Token *t = push_token(lx, TK_CHAR, start, tlen, line);
            if(!t) return false;
            t->val.ival = val;
            continue;
        }

        /* String literals */
        if(c=='"') {
            while(lx->pos < lx->len && lx_peek(lx)!='"') {
                if(lx_peek(lx)=='\\') { lx_advance(lx); if(decode_escape(lx)<0) return false; }
                else lx_advance(lx);
            }
            if(lx_peek(lx)!='"') {
                lx_err(lx, line, tok_col, "unterminated string");
                return false;
            }
            lx_advance(lx);
            int tlen = (int)(&lx->src[lx->pos] - start);
            Token *t = push_token(lx, TK_STRING, start, tlen, line);
            if(!t) return false;
            continue;
        }

        /* Identifiers and keywords */
        if(is_alpha(c)) {
            while(is_alnum(lx_peek(lx))) lx_advance(lx);
            int tlen = (int)(&lx->src[lx->pos] - start);
            /* Keyword lookup */
            TokenKind kind = TK_IDENT;
            for(int i=0; kw_table[i].word; i++) {
                if((int)strlen(kw_table[i].word)==tlen && memcmp(kw_table[i].word, start, tlen)==0) {
                    kind = kw_table[i].kind;
                    break;
                }
            }
            if(!push_token(lx, kind, start, tlen, line)) return false;
            continue;
        }

        /* Punctuation and operators */
        TokenKind kind = TK_EOF;
        switch(c) {
            case '(': kind = TK_LPAREN;    break;
            case ')': kind = TK_RPAREN;    break;
            case '{': kind = TK_LBRACE;    break;
            case '}': kind = TK_RBRACE;    break;
            case '[': kind = TK_LBRACKET;  break;
            case ']': kind = TK_RBRACKET;  break;
            case ',': kind = TK_COMMA;     break;
            case ';': kind = TK_SEMICOLON; break;
            case ':': kind = TK_COLON;     break;
            case '@': kind = TK_AT;        break;
            case '~': kind = TK_TILDE;     break;
            case '^': kind = lx_match(lx,'=') ? TK_CARET_ASSIGN  : TK_CARET;   break;
            case '|': kind = lx_match(lx,'|') ? TK_OR : (lx_match(lx,'=') ? TK_PIPE_ASSIGN : TK_PIPE); break;
            case '!': kind = lx_match(lx,'=') ? TK_NEQ  : TK_BANG;   break;
            case '=': kind = lx_match(lx,'=') ? TK_EQ : (lx_match(lx,'>') ? TK_FAT_ARROW : TK_ASSIGN); break;
            case '+': kind = lx_match(lx,'=') ? TK_PLUS_ASSIGN  : TK_PLUS;    break;
            case '-': kind = lx_match(lx,'>') ? TK_ARROW : (lx_match(lx,'=') ? TK_MINUS_ASSIGN : TK_MINUS); break;
            case '*': kind = lx_match(lx,'=') ? TK_STAR_ASSIGN  : TK_STAR;    break;
            case '/': kind = lx_match(lx,'=') ? TK_SLASH_ASSIGN : TK_SLASH;   break;
            case '%': kind = lx_match(lx,'=') ? TK_PERCENT_ASSIGN: TK_PERCENT; break;
            case '&':
                if(lx_match(lx,'&'))     kind = TK_AND;
                else if(lx_match(lx,'='))kind = TK_AMP_ASSIGN;
                else                     kind = TK_AMPERSAND;
                break;
            case '<':
                if(lx_match(lx,'<')) kind = lx_match(lx,'=') ? TK_LSHIFT_ASSIGN : TK_LSHIFT;
                else                 kind = lx_match(lx,'=') ? TK_LE : TK_LT;
                break;
            case '>':
                if(lx_match(lx,'>')) kind = lx_match(lx,'=') ? TK_RSHIFT_ASSIGN : TK_RSHIFT;
                else                 kind = lx_match(lx,'=') ? TK_GE : TK_GT;
                break;
            case '.':
                kind = lx_match(lx,'.') ? TK_DOTDOT : TK_DOT;
                break;
            default:
                lx_err(lx, line, tok_col, "unexpected character '%c'", c);
                return false;
        }
        int tlen = (int)(&lx->src[lx->pos] - start);
        if(!push_token(lx, kind, start, tlen, line)) return false;
    }

    /* EOF sentinel */
    push_token(lx, TK_EOF, &lx->src[lx->pos], 0, lx->line);
    return true;
}

/* -------------------------------------------------------------------------
 * Token name (for error messages and disassembly)
 * ---------------------------------------------------------------------- */
const char *relic_token_name(TokenKind k) {
    static const char *names[] = {
        "EOF","INT","CHAR","STRING","FLOAT","IDENT",
        /* Positionally indexed by TokenKind - keep in lockstep with the
         * enum in tigryx.h or every later name is reported wrongly. */
        "bool","break","cast","class","const","continue","else","false","fn","for","if","f32","f64",
        "i8","i16","i32","in","match","ref","return","struct","true",
        "u8","u16","u32","var","while",
        "(",")","{","}","[","]",",",";",":",".","->","..", "@","&","*","=>",
        "+","-","/","%","!","~","|","^","<<",">>",
        "==","!=","<","<=",">",">=",
        "&&","||",
        "=","+=","-=","*=","/=","%=","&=","|=","^=","<<=",">>="
    };
    if(k >= 0 && k < TK_COUNT) return names[k];
    return "?";
}
