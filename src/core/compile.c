/*
 * compile.c
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
 * compile.c - relic_compile_source() / relic_compile_file()
 *
 * The lex -> parse -> typecheck -> emit pipeline, extracted out of
 * tigryxri's main.c so it can be reused by anything that needs to turn
 * already-preprocessed Relic source into an IRBuffer: the native CLI
 * (main.c calls this instead of inlining the pipeline), and - not yet
 * written - the in-browser tigryxwc compiler.
 *
 * This file does NOT know about the .tgx preprocessor. Callers resolve
 * #import / class-lowering first, either by spawning tigryxpp (native, see
 * main.c) or by calling tigryx_pp_run() in-process (Emscripten). By the
 * time source reaches relic_compile_source(), it is plain .tp.
 */
#include "tigryx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fill *out_diag (if non-NULL). Any of file_id/col/end_col may legitimately
 * be -1 ("unknown") - not every stage can attribute an error to a precise
 * token (see the per-stage callers below for what's actually reliable).
 * `file_path` may be NULL (unknown) even when `file_id` is not: it's
 * resolved by the caller from the still-in-scope Lexer's file table (see
 * relic_compile_source), since IRBuffer::source_files[] - the only other
 * place a file_id could be turned back into a path - is never populated
 * after a failed compile. */
static void diag_fill(RelicDiag *out_diag, RelicDiagStage stage, const char *message,
                      int line, int file_id, const char *file_path, int col, int end_col) {
    if(!out_diag) return;
    out_diag->stage    = stage;
    out_diag->file_id  = file_id;
    out_diag->line     = line;
    out_diag->col      = col;
    out_diag->end_col  = end_col;
    snprintf(out_diag->message, sizeof(out_diag->message), "%.*s",
             (int)sizeof(out_diag->message) - 1, message ? message : "");
    snprintf(out_diag->file_path, sizeof(out_diag->file_path), "%.*s",
             (int)sizeof(out_diag->file_path) - 1, file_path ? file_path : "");
}

/* Resolve a lexer-relative file_id to its path string, or NULL if it
 * doesn't name a real entry (id < 0, or out of range - the latter
 * shouldn't happen, but a garbage id must not read out of bounds). */
static const char *lexer_file_path(const Lexer *lx, int file_id) {
    if(file_id < 0 || file_id >= lx->nfiles) return NULL;
#ifdef RELIC_IA16
    /* files[] is far on ia16; bounce into a static near buffer. Callers
     * use the result immediately (diagnostic printing). */
    static char fbuf[RELIC_MAX_SOURCE_PATH];
    relic_far_str(fbuf, LXFILE(lx, file_id), sizeof fbuf);
    return fbuf;
#else
    return lx->files[file_id];
#endif
}

bool relic_compile_source(const char *src, int src_len, const char *display_path,
                          IRBuffer *ir, int *out_main_fn_id, RelicDiag *out_diag) {
    if(out_main_fn_id) *out_main_fn_id = -1;

    int initial_capacity = 4096;
    Token *tokens = calloc((size_t)initial_capacity, sizeof(Token));
    if(!tokens) { diag_fill(out_diag, RELIC_DIAG_LEX, "out of memory", 0, -1, NULL, -1, -1); return false; }

    /* relic_lex() may realloc() the token array as it grows past
     * initial_capacity, which can move it - always free lx.tokens (the
     * current pointer), never the original `tokens` value, or a program
     * with more than 4096 tokens frees a block realloc() already moved. */
    Lexer lx = {0};
    lx.tokens   = tokens;
    lx.capacity = initial_capacity;
#ifdef RELIC_IA16
    /* lx.files is a far DOS block - release alongside lx.tokens. */
#define LX_FILES_FREE() relic_far_free(lx.files)
#else
#define LX_FILES_FREE() ((void)0)
#endif
    if(!relic_lex(&lx, src, src_len, display_path)) {
        /* Prefer the structured error position (lx_err records where the
         * fault was recognised - for unterminated comment/string that's the
         * construct's start, which can be many lines before the scanning
         * position the error is finally detected at). Fall back to the
         * lexer's current position for errors that don't set it (OOM). */
        int err_line = lx.err_line > 0 ? lx.err_line : lx.line;
        int err_file = lx.err_file_id >= 0 ? lx.err_file_id : lx.cur_file;
        int col = lx.err_col > 0 ? lx.err_col : lx.pos - lx.line_start + 1;
        if(col < 1) col = 1;
        diag_fill(out_diag, RELIC_DIAG_LEX, lx.err, err_line, err_file,
                 lexer_file_path(&lx, err_file), col, col);
        free(lx.tokens); LX_FILES_FREE();
        return false;
    }

    Parser *parser = calloc(1, sizeof(Parser));
    if(!parser) { diag_fill(out_diag, RELIC_DIAG_PARSE, "out of memory", 0, -1, NULL, -1, -1); free(lx.tokens); LX_FILES_FREE(); return false; }
    /* Pool grows lazily inside the parser; no need to pre-size it. */
    Node *root = relic_parse(parser, lx.tokens, lx.ntokens);
    #define RELIC_FREE_PARSER() do { \
        for(int _i=0; _i<parser->npool; _i++) { \
            if(parser->pool[_i]) { free(parser->pool[_i]->children); free(parser->pool[_i]); } \
        } \
        free(parser->pool); free(parser); \
    } while(0)
    if(!root) {
        /* parser->err_line is only ever set by expect() (see parser.c), so
         * it's stale/0 for most of the ~15 error sites - but parser->pos
         * hasn't moved past the offending token in any of them, so the
         * token it's still pointing at carries the right line/file/col
         * regardless of which site failed. */
        const Token *bad = (parser->pos >= 0 && parser->pos < parser->ntokens)
                          ? &parser->tokens[parser->pos] : NULL;
        int bad_file = bad ? bad->file_id : -1;
        diag_fill(out_diag, RELIC_DIAG_PARSE, parser->err,
                 bad ? bad->line : parser->err_line,
                 bad_file, lexer_file_path(&lx, bad_file),
                 bad ? bad->col : -1,
                 bad ? bad->end_col : -1);
        RELIC_FREE_PARSER();
        free(lx.tokens); LX_FILES_FREE();
        return false;
    }

    TypeChecker *tc = calloc(1, sizeof(TypeChecker));
    if(!tc) { diag_fill(out_diag, RELIC_DIAG_TYPECHECK, "out of memory", 0, -1, NULL, -1, -1); RELIC_FREE_PARSER(); free(lx.tokens); LX_FILES_FREE(); return false; }
    if(!relic_typecheck(tc, root, ir)) {
        diag_fill(out_diag, RELIC_DIAG_TYPECHECK, tc->err, tc->err_line, tc->err_file,
                 lexer_file_path(&lx, tc->err_file), tc->err_col, -1);
        free(tc); RELIC_FREE_PARSER(); free(lx.tokens); LX_FILES_FREE();
        return false;
    }

    IREmitter *em = calloc(1, sizeof(IREmitter));
    if(!em) { diag_fill(out_diag, RELIC_DIAG_EMIT, "out of memory", 0, -1, NULL, -1, -1); free(tc); RELIC_FREE_PARSER(); free(lx.tokens); LX_FILES_FREE(); return false; }
    if(!relic_emit(em, root, ir, &tc->syms, tc, lx.tokens)) {
        /* em->err_line is never set (see ir.c), but cur_line/cur_file_id
         * track the AST node being emitted - since emission is single-pass
         * and errors return immediately, they still point at the offending
         * node when relic_emit() fails. Column isn't tracked there. */
        diag_fill(out_diag, RELIC_DIAG_EMIT, em->err, em->cur_line,
                 em->cur_file_id, lexer_file_path(&lx, em->cur_file_id), -1, -1);
        free(em); free(tc); RELIC_FREE_PARSER(); free(lx.tokens); LX_FILES_FREE();
        return false;
    }

    /* Find main() */
    int main_fn_id = -1;
    for(int i=0; i<tc->syms.nsyms; i++) {
        if(strcmp(tc->syms.syms[i].name, "main")==0 && tc->syms.syms[i].kind==SYM_FN) {
            main_fn_id = tc->syms.syms[i].fn_id; break;
        }
    }
    if(out_main_fn_id) *out_main_fn_id = main_fn_id;

    /* Copy the lexer's file table onto the IR so PC->file attribution
     * (file_for_pc, stamped by the emitter) is interpretable after this
     * function returns - relic_line_to_pc() and the future .tlx debug
     * section (and @assert traps) both key off ir->source_files[]. */
    /* On ia16 the destination table is far-allocated on demand;
     * a degenerate program could reach here without any emit() call. */
    if(!relic_ir_ensure_aux(ir)) {
        diag_fill(out_diag, RELIC_DIAG_EMIT, "out of memory", 0, -1, NULL, -1, -1);
        free(em->patches); free(em->labels); free(em);
        free(tc); RELIC_FREE_PARSER(); free(lx.tokens); LX_FILES_FREE();
        return false;
    }
    int ncopy = lx.nfiles < RELIC_MAX_SOURCE_FILES ? lx.nfiles : RELIC_MAX_SOURCE_FILES;
    for(int i = 0; i < ncopy; i++) {
        /* Both tables may be far (ia16): bounce through a near buffer. */
        char pb[RELIC_MAX_SOURCE_PATH];
        relic_far_str(pb, LXFILE(&lx, i), sizeof pb);
        relic_far_write((u8 RELIC_FAR *)ir->source_files,
                        (u32)i * RELIC_MAX_SOURCE_PATH, pb, (u32)strlen(pb) + 1);
    }
    ir->nsource_files = ncopy;

    /* Compile warnings collected during typecheck (e.g. uninitialised
     * class fields). Printed here - after source_files is populated - so
     * file_id resolves to a path. Non-fatal by definition. */
    for(int i = 0; i < ir->nwarnings; i++) {
        RelicWarning *wn = &ir->warnings[i];
        const char *fp = "?";
        if(wn->file_id >= 0 && wn->file_id < ir->nsource_files) {
            static char wpb[RELIC_MAX_SOURCE_PATH];
            relic_far_str(wpb, IRSRCFILE(ir, wn->file_id), sizeof wpb);
            fp = wpb;
        }
        if(wn->col > 0)
            fprintf(stderr, "%s:%d:%d: warning: %s\n", fp, wn->line, wn->col, wn->message);
        else
            fprintf(stderr, "%s:%d: warning: %s\n", fp, wn->line, wn->message);
    }
    if(ir->warnings_dropped > 0)
        fprintf(stderr, "note: %d more warning(s) suppressed\n", ir->warnings_dropped);

    free(em->patches); free(em->labels); free(em);
    free(tc);
    RELIC_FREE_PARSER();
    #undef RELIC_FREE_PARSER
    free(lx.tokens); LX_FILES_FREE();
    #undef LX_FILES_FREE
    return true;
}

bool relic_compile_file(const char *input_path, IRBuffer *ir,
                        int *out_main_fn_id, RelicDiag *out_diag) {
    FILE *f = fopen(input_path, "rb");
    if(!f) {
        diag_fill(out_diag, RELIC_DIAG_READ, "cannot open input file", 0, -1, input_path, -1, -1);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = malloc((size_t)sz + 1);
    if(!src) { fclose(f); diag_fill(out_diag, RELIC_DIAG_READ, "out of memory", 0, -1, input_path, -1, -1); return false; }
    size_t nread = fread(src, 1, (size_t)sz, f);
    src[nread] = 0;
    fclose(f);

    bool ok = relic_compile_source(src, (int)nread, input_path, ir, out_main_fn_id, out_diag);
    free(src);
    return ok;
}
