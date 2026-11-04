/*
 * debug.c
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
 * debug.c - Interactive line-level debugger for Relic
 *
 * Attached to a RelicVM via vm->dbg. The VM calls our on_break hook
 * before each instruction whenever single-stepping or a PC breakpoint
 * fires; the hook runs an interactive REPL on stdin/stdout until the
 * user resumes.
 *
 * Commands:
 *   b <line>         set breakpoint at source line
 *   b <file>:<line>  set breakpoint at line in file
 *   d <n>            delete breakpoint with index n (from `bl`)
 *   bl               list breakpoints
 *   c                continue
 *   s                step one IR instruction
 *   n                step over (next source line, do not enter calls)
 *   si               step into (next source line, do enter calls)
 *   fin              step out (run until current frame returns)
 *   bt               backtrace
 *   p r<n>           print register
 *   p *<addr>        print 32-bit memory word at addr
 *   l                list source around current line
 *   disas            disassemble current function
 *   q                quit (sets exit_code = 130)
 *   h, ?             help
 */
#include "tigryx.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Source-file cache
 * ---------------------------------------------------------------------- */
typedef struct {
    char  path[512];
    char *buf;
    int   nlines;
    int  *line_off;     /* byte offset of each line (1-indexed; [0] unused) */
    int  *line_len;
} DbgSource;

static void dbg_source_load(DbgSource *s, const char *path) {
    memset(s, 0, sizeof(*s));
    if(!path || !*path) return;
    snprintf(s->path, sizeof(s->path), "%s", path);
    FILE *f = fopen(path, "rb");
    if(!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    s->buf = (char*)malloc((size_t)sz + 1);
    if(!s->buf) { fclose(f); return; }
    size_t n = fread(s->buf, 1, (size_t)sz, f);
    s->buf[n] = 0;
    fclose(f);

    /* Count + index lines. */
    int count = 1;
    for(size_t i=0;i<n;i++) if(s->buf[i]=='\n') count++;
    s->line_off = (int*)calloc((size_t)count + 2, sizeof(int));
    s->line_len = (int*)calloc((size_t)count + 2, sizeof(int));
    if(!s->line_off || !s->line_len) return;
    int line = 1;
    int start = 0;
    s->line_off[1] = 0;
    for(int i=0;i<(int)n;i++) {
        if(s->buf[i] == '\n') {
            s->line_len[line] = i - start;
            line++;
            start = i + 1;
            s->line_off[line] = start;
        }
    }
    s->line_len[line] = (int)n - start;
    s->nlines = line;
}

static void dbg_source_free(DbgSource *s) {
    free(s->buf);
    free(s->line_off);
    free(s->line_len);
    memset(s, 0, sizeof(*s));
}

static void dbg_source_print_range(DbgSource *s, int from, int to, int marker) {
    if(!s->buf) { printf("  (no source available)\n"); return; }
    if(from < 1) from = 1;
    if(to > s->nlines) to = s->nlines;
    for(int ln=from; ln<=to; ln++) {
        char prefix = (ln == marker) ? '>' : ' ';
        int off = s->line_off[ln];
        int len = s->line_len[ln];
        printf("%c %4d  %.*s\n", prefix, ln, len, s->buf + off);
    }
}

/* -------------------------------------------------------------------------
 * Debugger state
 * ---------------------------------------------------------------------- */
typedef struct {
    RelicDebugger dbg;          /* must be first; pointer compatibility */
    IRBuffer     *ir;
    DbgSource     src;

    /* Breakpoint registry (parallel to bitmap, for listing/delete UI). */
    struct { u32 pc; int line; bool active; } bps[256];
    int          nbps;

    /* Last "step over / step into" state. */
    int          step_line;     /* line we started stepping from */
    u32          step_fp;       /* fp at step start */
    bool         step_over;     /* true = step-over, false = step-into */
} Dbg;

static int line_at_pc(IRBuffer *ir, u32 pc) {
    if((int)pc < 0 || (int)pc >= ir->len) return 0;
    return IRLINE_RD(ir, pc);
}

/* Bitmap helpers. We size the bitmap to ir->len bits (rounded up). */
static void bp_bitmap_ensure(Dbg *d) {
    if(d->dbg.bp_bitmap) return;
    int bytes = (d->ir->len + 7) / 8;
    if(bytes < 1) bytes = 1;
    d->dbg.bp_bitmap = (u8*)calloc(1, (size_t)bytes);
    d->dbg.bp_bitmap_bytes = bytes;
}
static void bp_set(Dbg *d, u32 pc) {
    bp_bitmap_ensure(d);
    int byte = (int)(pc >> 3);
    if(byte >= d->dbg.bp_bitmap_bytes) return;
    d->dbg.bp_bitmap[byte] |= (1u << (pc & 7));
}
static void bp_clear(Dbg *d, u32 pc) {
    if(!d->dbg.bp_bitmap) return;
    int byte = (int)(pc >> 3);
    if(byte >= d->dbg.bp_bitmap_bytes) return;
    d->dbg.bp_bitmap[byte] &= ~(1u << (pc & 7));
}

/* -------------------------------------------------------------------------
 * Command parsing helpers
 * ---------------------------------------------------------------------- */
static char *trim(char *s) {
    while(*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while(e > s && isspace((unsigned char)e[-1])) e--;
    *e = 0;
    return s;
}

static bool parse_int(const char *s, long *out) {
    if(!s || !*s) return false;
    char *end = NULL;
    long v;
    if(s[0]=='0' && (s[1]=='x'||s[1]=='X')) v = strtol(s, &end, 16);
    else                                    v = strtol(s, &end, 10);
    if(end == s) return false;
    *out = v;
    return true;
}

/* -------------------------------------------------------------------------
 * Command implementations
 * ---------------------------------------------------------------------- */
static void cmd_help(void) {
    printf(
        "commands:\n"
        "  b <line>         set breakpoint at source line\n"
        "  b <file>:<line>  set breakpoint at line in file\n"
        "  bl               list breakpoints\n"
        "  d <n>            delete breakpoint #n\n"
        "  c                continue\n"
        "  s                step one IR instruction\n"
        "  n                step over (next source line, skip calls)\n"
        "  si               step into (next source line, enter calls)\n"
        "  fin              step out (run until current frame returns)\n"
        "  bt               backtrace\n"
        "  p r<n>           print register r0..r15\n"
        "  p *<addr>        print 32-bit memory word\n"
        "  l [n]            list source (n lines context, default 5)\n"
        "  disas            disassemble current function\n"
        "  q                quit\n"
        "  h, ?             this help\n");
}

static void cmd_break(Dbg *d, char *arg) {
    if(!*arg) { printf("usage: b <line> | b <file>:<line>\n"); return; }
    char *colon = strchr(arg, ':');
    int line = 0;
    const char *path = d->src.path;
    if(colon) {
        *colon = 0;
        path = arg;
        long v;
        if(!parse_int(colon+1, &v)) { printf("bad line\n"); return; }
        line = (int)v;
    } else {
        long v;
        if(!parse_int(arg, &v)) { printf("bad line\n"); return; }
        line = (int)v;
    }
    int pc = relic_line_to_pc(d->ir, path, line);
    if(pc < 0) { printf("no executable code at %s:%d\n", path, line); return; }
    if(d->nbps >= (int)(sizeof(d->bps)/sizeof(d->bps[0]))) {
        printf("breakpoint table full\n"); return;
    }
    d->bps[d->nbps].pc = (u32)pc;
    d->bps[d->nbps].line = IRLINE_RD(d->ir, pc);
    d->bps[d->nbps].active = true;
    bp_set(d, (u32)pc);
    printf("breakpoint %d set at line %d (pc=%d)\n",
           d->nbps, d->bps[d->nbps].line, pc);
    d->nbps++;
}

static void cmd_bl(Dbg *d) {
    if(d->nbps == 0) { printf("(no breakpoints)\n"); return; }
    for(int i=0;i<d->nbps;i++) {
        printf("  %d  %s  line %d  pc=%u\n",
               i, d->bps[i].active ? "on " : "off",
               d->bps[i].line, d->bps[i].pc);
    }
}

static void cmd_delete(Dbg *d, char *arg) {
    long v;
    if(!parse_int(arg, &v) || v < 0 || v >= d->nbps) {
        printf("usage: d <bp index>\n"); return;
    }
    int i = (int)v;
    if(d->bps[i].active) {
        bp_clear(d, d->bps[i].pc);
        d->bps[i].active = false;
        /* If no other active bp shares that pc, the bit was cleared.
         * If two bps shared a pc, the second is now silently dead — fine. */
    }
    printf("breakpoint %d removed\n", i);
}

static void cmd_print(RelicVM *vm, char *arg) {
    if(!*arg) { printf("usage: p r<n> | p *<addr>\n"); return; }
    if(arg[0] == 'r' || arg[0] == 'R') {
        long v;
        if(!parse_int(arg+1, &v) || v < 0 || v > 15) {
            printf("usage: p r0..r15\n"); return;
        }
        i32 val = vm->r[(int)v & 0xF];
        if(v == 14) val = (i32)vm->fp;
        if(v == 15) val = (i32)vm->sp;
        if(v == 0)  val = 0;
        printf("r%ld = %d (0x%08x)\n", v, val, (u32)val);
    } else if(arg[0] == '*') {
        long v;
        if(!parse_int(arg+1, &v) || v < 0 || (u32)v + 4 > vm->mem_sz) {
            printf("bad address\n"); return;
        }
        i32 word;
        relic_far_read(vm->mem, (u32)v, &word, 4);
        printf("*0x%08lx = %d (0x%08x)\n", v, word, (u32)word);
    } else {
        printf("usage: p r<n> | p *<addr>\n");
    }
}

static void cmd_list(Dbg *d, RelicVM *vm, char *arg) {
    int ctx = 5;
    long v;
    if(*arg && parse_int(arg, &v) && v > 0 && v < 1000) ctx = (int)v;
    int line = line_at_pc(d->ir, vm->pc);
    if(line <= 0) { printf("(no source line for pc=%u)\n", vm->pc); return; }
    printf("-- %s --\n", d->src.path);
    dbg_source_print_range(&d->src, line - ctx, line + ctx, line);
}

static void cmd_disas(Dbg *d, RelicVM *vm) {
    int fn = relic_pc_to_fn(d->ir, vm->pc);
    if(fn < 0) { relic_ir_disasm(d->ir, 0, d->ir->len); return; }
    int from = IRFNOFF(d->ir, fn);
    int to   = d->ir->len;
    for(int i=0;i<d->ir->nfuncs;i++) {
        int o = IRFNOFF(d->ir, i);
        if(o > from && o < to) to = o;
    }
    char name[RELIC_MAX_IDENT];
    relic_ir_fn_name(d->ir, fn, name, sizeof name);
    printf("-- fn%d %s [%d..%d] --\n", fn, name[0] ? name : "?", from, to);
    relic_ir_disasm(d->ir, from, to);
}

static void cmd_bt(Dbg *d, RelicVM *vm) {
    /* Walk frames using the layout established by OP_CALL + OP_ENTER:
     *   at vm->mem[fp+0..3] = saved old fp
     *   at vm->mem[fp+4..7] = saved return pc
     * Stop when fp leaves valid memory or stops moving. */
    u32 pc = vm->pc;
    u32 fp = vm->fp;
    int depth = 0;
    while(depth < 64) {
        int fn = relic_pc_to_fn(d->ir, pc);
        char name[RELIC_MAX_IDENT];
        name[0] = 0;
        if(fn >= 0) relic_ir_fn_name(d->ir, fn, name, sizeof name);
        int line = line_at_pc(d->ir, pc);
        printf("  #%d  pc=%u  %s%s%s",
               depth,
               pc,
               name[0] ? "in " : "",
               name[0] ? name : (fn >= 0 ? "?" : "<top>"),
               (fn >= 0) ? "()" : "");
        if(line > 0) printf("  at %s:%d", d->src.path, line);
        printf("\n");
        if(fp + 8 > vm->mem_sz) break;
        u32 saved_fp, ret_pc;
        relic_far_read(vm->mem, fp, &saved_fp, 4);
        relic_far_read(vm->mem, fp + 4, &ret_pc, 4);
        if(saved_fp <= fp || saved_fp > vm->mem_sz) break;
        pc = ret_pc;
        fp = saved_fp;
        depth++;
        if(pc >= (u32)d->ir->len) break;
    }
}

/* Set up VM state for the various step modes. */
static void start_step_into(Dbg *d, RelicVM *vm) {
    d->step_line = line_at_pc(d->ir, vm->pc);
    d->step_fp   = vm->fp;
    d->step_over = false;
    d->dbg.single_step  = true;
    d->dbg.step_fp_floor = 0;
}
static void start_step_over(Dbg *d, RelicVM *vm) {
    d->step_line = line_at_pc(d->ir, vm->pc);
    d->step_fp   = vm->fp;
    d->step_over = true;
    d->dbg.single_step  = true;
    /* In the VM: fp_ok = vm->fp >= step_fp_floor. fp gets *smaller* on
     * call (stack grows down), so requiring fp >= current_fp suppresses
     * single-step inside callees but breakpoints still fire. */
    d->dbg.step_fp_floor = vm->fp;
}
static void start_step_out(Dbg *d, RelicVM *vm) {
    d->step_line = line_at_pc(d->ir, vm->pc);
    d->step_fp   = vm->fp;
    d->step_over = true;
    d->dbg.single_step  = true;
    /* Only fire once we are strictly shallower than the current frame. */
    d->dbg.step_fp_floor = vm->fp + 1;
}
static void start_continue(Dbg *d) {
    d->dbg.single_step  = false;
    d->dbg.step_fp_floor = 0;
    d->step_line = 0;
}

/* -------------------------------------------------------------------------
 * Break callback (the heart of the REPL)
 * ---------------------------------------------------------------------- */
static void on_break(RelicVM *vm, void *user) {
    Dbg *d = (Dbg*)user;

    /* If we're stepping by source line, suppress the prompt until the
     * line actually changes. This also collapses long runs of IR words
     * that all map to the same source line into a single user step. */
    if(d->dbg.single_step && d->step_line > 0) {
        int cur = line_at_pc(d->ir, vm->pc);
        bool fp_ok = (d->dbg.step_fp_floor == 0) ||
                     (vm->fp >= d->dbg.step_fp_floor);
        if(fp_ok && cur != 0 && cur == d->step_line) {
            return; /* same source line, keep stepping */
        }
    }

    int line = line_at_pc(d->ir, vm->pc);
    int fn   = relic_pc_to_fn(d->ir, vm->pc);
    char fn_name[RELIC_MAX_IDENT];
    fn_name[0] = 0;
    if(fn >= 0) relic_ir_fn_name(d->ir, fn, fn_name, sizeof fn_name);
    printf("\n[stopped] pc=%u%s%s%s", vm->pc,
           fn_name[0] ? "  in " : "",
           fn_name[0] ? fn_name : "",
           fn_name[0] ? "()" : "");
    if(line > 0) printf("  at %s:%d", d->src.path, line);
    printf("\n");
    if(line > 0) dbg_source_print_range(&d->src, line, line, line);

    char buf[512];
    for(;;) {
        printf("(rdb) ");
        fflush(stdout);
        if(!fgets(buf, sizeof(buf), stdin)) {
            /* EOF on stdin: detach and let the program run to completion. */
            start_continue(d);
            return;
        }
        char *line_in = trim(buf);
        if(!*line_in) {
            /* Repeat: default to single-instruction step. */
            d->dbg.single_step = true;
            d->dbg.step_fp_floor = 0;
            d->step_line = 0;
            return;
        }

        /* Split first token. */
        char *cmd = line_in;
        char *arg = line_in;
        while(*arg && !isspace((unsigned char)*arg)) arg++;
        if(*arg) { *arg++ = 0; arg = trim(arg); }

        if(!strcmp(cmd,"h") || !strcmp(cmd,"?") || !strcmp(cmd,"help")) {
            cmd_help();
        } else if(!strcmp(cmd,"b") || !strcmp(cmd,"break")) {
            cmd_break(d, arg);
        } else if(!strcmp(cmd,"bl")) {
            cmd_bl(d);
        } else if(!strcmp(cmd,"d") || !strcmp(cmd,"delete")) {
            cmd_delete(d, arg);
        } else if(!strcmp(cmd,"c") || !strcmp(cmd,"cont") || !strcmp(cmd,"continue")) {
            start_continue(d); return;
        } else if(!strcmp(cmd,"s") || !strcmp(cmd,"stepi")) {
            d->dbg.single_step = true;
            d->dbg.step_fp_floor = 0;
            d->step_line = 0; /* one IR instruction, not one source line */
            return;
        } else if(!strcmp(cmd,"si")) {
            start_step_into(d, vm); return;
        } else if(!strcmp(cmd,"n") || !strcmp(cmd,"next")) {
            start_step_over(d, vm); return;
        } else if(!strcmp(cmd,"fin") || !strcmp(cmd,"finish")) {
            start_step_out(d, vm); return;
        } else if(!strcmp(cmd,"bt") || !strcmp(cmd,"backtrace")) {
            cmd_bt(d, vm);
        } else if(!strcmp(cmd,"p") || !strcmp(cmd,"print")) {
            cmd_print(vm, arg);
        } else if(!strcmp(cmd,"l") || !strcmp(cmd,"list")) {
            cmd_list(d, vm, arg);
        } else if(!strcmp(cmd,"disas") || !strcmp(cmd,"disasm")) {
            cmd_disas(d, vm);
        } else if(!strcmp(cmd,"q") || !strcmp(cmd,"quit") || !strcmp(cmd,"exit")) {
            vm->running = false;
            vm->exit_code = 130;
            return;
        } else {
            printf("unknown command '%s' (try 'h')\n", cmd);
        }
    }
}

/* -------------------------------------------------------------------------
 * Public entry: relic_debug_run
 * ---------------------------------------------------------------------- */
int relic_debug_run(RelicVM *vm, IRBuffer *ir, int main_fn_id) {
    Dbg d;
    memset(&d, 0, sizeof(d));
    d.ir = ir;
    d.dbg.on_break = on_break;
    d.dbg.user     = &d;
    d.dbg.single_step = true;     /* break on the very first instruction */
    d.dbg.step_fp_floor = 0;

    dbg_source_load(&d.src, ir->source_path);

    printf("Relic debugger %s\n", RELIC_VERSION);
    if(d.src.buf) {
        printf("source: %s (%d lines)\n", d.src.path, d.src.nlines);
    } else if(*ir->source_path) {
        printf("source: %s (could not read file)\n", ir->source_path);
    } else {
        printf("source: <unknown>\n");
    }
    printf("type 'h' for help, 'c' to run.\n");

    vm->dbg = &d.dbg;
    relic_vm_run(vm);
    vm->dbg = NULL;

    free(d.dbg.bp_bitmap);
    dbg_source_free(&d.src);
    (void)main_fn_id;
    return vm->exit_code;
}
