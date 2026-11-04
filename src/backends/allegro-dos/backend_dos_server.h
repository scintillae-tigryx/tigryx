/*
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
 * backend_dos_server.h - lwIP/tdn_* implementation of @serve/@serve_reply
 * (the experimental "NSNF" tiny socket server).
 *
 * DOS counterpart of backends/common/nsnf_server.h. There are no worker
 * threads here: everything is a non-blocking state machine advanced from
 * the builtins, so progress only happens while the VM keeps polling
 * (SRV_POLL/XFER/READ all pump the stack). The guest-visible ABI -
 * ServeRequest (576B), ServeStat (48B), ServeXfer (176B records), handle
 * encoding, command numbers and reply semantics - is identical, so the
 * same coop.tgx/pigeon.tgx binaries run unchanged.
 *
 * Only compiled into a DOS backend when both HAVE_DOSNET (lwIP is linked)
 * and TIGRYX_EXPERIMENTAL_SERVER are defined; the including .c file does
 * the #ifdef and wires .sys_serve / .sys_serve_reply / shutdown itself:
 *
 *     #if defined(HAVE_DOSNET) && defined(TIGRYX_EXPERIMENTAL_SERVER)
 *     #include "../allegro-dos/backend_dos_server.h"
 *     #endif
 */

#ifndef BACKEND_DOS_SERVER_H
#define BACKEND_DOS_SERVER_H

#include "tigryx.h"
#include "tdosnet.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define DS_MAX_SRV   4
#define DS_MAX_CONN  8
#define DS_HDR_CAP   8192     /* smaller than desktop's 16K: XMS is finite */
#define DS_PRE_CAP   2048     /* request-body bytes piggybacked on headers */
#define DS_FILE_CHUNK 4096

#define DS_REQ_SIZE  576
#define DS_STAT_SIZE 48
#define DS_XFER_SIZE 176

enum {
    DS_ST_READING = 0,    /* headers still arriving        */
    DS_ST_READY,          /* parsed, in the srv ready queue */
    DS_ST_HANDED,         /* given to the guest by SRV_POLL */
    DS_ST_REPLYING,       /* response in flight            */
    DS_ST_DEAD
};

typedef struct {
    int       in_use;
    int       gen;
    int       fd;              /* tdn_* conn fd */
    int       srv_idx;
    int       state;
    char      ip[32];

    /* parsed request */
    int       method;
    long long body_len;
    long long range_off;
    long long range_len;
    int       keep_alive;
    char      token[96];
    char      ua[64];
    char      path[256];
    char      query[96];

    /* incremental header assembly */
    char      hdrbuf[DS_HDR_CAP];
    int       hdr_used;

    /* body bytes that arrived with the headers, served first by SRV_READ */
    unsigned char body_pre[DS_PRE_CAP];
    int       body_pre_len;
    int       body_pre_pos;

    /* reply: header block always; then in-mem body or file stream */
    int       reply_status;
    char     *snd;            /* malloc'd response headers */
    int       snd_len, snd_pos;
    unsigned char *body;      /* malloc'd copy for in-mem replies */
    int       body_len_out, body_pos;
    FILE     *fp;             /* file streaming */
    long long file_left;
    long long sent;           /* body bytes sent (XFER progress)     */
    long long total;          /* -1 unknown                          */
} DsConn;

typedef struct {
    int in_use;
    int lfd;                  /* tdn_* listener fd */
    int port;
    long long reqs_total;
    long long reqs_failed;
    long long bytes_sent;
    int  ready[DS_MAX_CONN];
    int  ready_n;
} DsSrv;

typedef struct {
    int      in_use;
    RelicVM *vm;
    DsSrv    srv[DS_MAX_SRV];
    DsConn   conn[DS_MAX_CONN];
} DsState;

static DsState g_ds[2];       /* DOS hosts one VM; 2 for safety */

static DsState *ds_state_for(RelicVM *vm) {
    int i, free_i = -1;
    for (i = 0; i < 2; i++)
        if (g_ds[i].in_use && g_ds[i].vm == vm) return &g_ds[i];
    for (i = 0; i < 2; i++)
        if (!g_ds[i].in_use) { free_i = i; break; }
    if (free_i < 0) return NULL;
    memset(&g_ds[free_i], 0, sizeof(DsState));
    g_ds[free_i].in_use = 1;
    g_ds[free_i].vm     = vm;
    return &g_ds[free_i];
}

/* Called from the backend shutdown path. */
static void dos_serve_shutdown(RelicVM *vm) {
    DsState *st;
    int i, j;
    for (i = 0; i < 2; i++)
        if (g_ds[i].in_use && g_ds[i].vm == vm) break;
    if (i == 2) return;
    st = &g_ds[i];
    for (i = 0; i < DS_MAX_SRV; i++)
        if (st->srv[i].in_use) { tdn_close(st->srv[i].lfd); st->srv[i].in_use = 0; }
    for (j = 0; j < DS_MAX_CONN; j++)
        if (st->conn[j].in_use) {
            if (st->conn[j].fp) fclose(st->conn[j].fp);
            if (st->conn[j].fd >= 0) tdn_close(st->conn[j].fd);
            free(st->conn[j].snd);
            free(st->conn[j].body);
            st->conn[j].in_use = 0;
        }
}

/* ---- small helpers (ds_* mirror the nsnf_* ones in nsnf_server.h) ------- */

static int ds_vm_str(RelicVM *vm, u32 ptr, char *out, int cap) {
    int n = 0;
    if (!ptr || ptr >= vm->mem_sz || cap <= 1) { out[0] = 0; return 0; }
    while (n < cap - 1 && ptr + (u32)n < vm->mem_sz) {
        char ch = (char)vm->mem[ptr + n];
        if (!ch) break;
        out[n++] = ch;
    }
    out[n] = 0;
    return n;
}

static void ds_vm_put32(RelicVM *vm, u32 off, i32 v, u32 cap_end) {
    if (off + 4 <= cap_end && off + 4 <= vm->mem_sz)
        memcpy(vm->mem + off, &v, 4);
}

static void ds_vm_str_out(RelicVM *vm, u32 off, const char *s, int cap,
                          u32 cap_end) {
    int n = 0;
    if (off >= cap_end) return;
    while (s[n] && n < cap - 1 && off + (u32)n < cap_end - 1
           && off + (u32)n < vm->mem_sz - 1) {
        vm->mem[off + n] = (u8)s[n];
        n++;
    }
    vm->mem[off + n] = 0;
}

static void ds_ip_str(uint32_t host_ip, char *out, int cap) {
    snprintf(out, (size_t)cap, "%lu.%lu.%lu.%lu",
             (unsigned long)((host_ip >> 24) & 0xFF),
             (unsigned long)((host_ip >> 16) & 0xFF),
             (unsigned long)((host_ip >> 8) & 0xFF),
             (unsigned long)(host_ip & 0xFF));
}

static int ds_ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 1;
        if (!x) return 0;
    }
}

static int ds_istarts(const char *s, const char *pfx) {
    while (*pfx) {
        char x = *s, y = *pfx;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        s++; pfx++;
    }
    return 1;
}

static int ds_icontains(const char *hay, const char *needle) {
    int nl = (int)strlen(needle);
    const char *p;
    for (p = hay; *p; p++) {
        int i;
        for (i = 0; i < nl; i++) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == nl) return 1;
        if (!p[i]) return 0;
    }
    return 0;
}

static int ds_url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = r[1], lo = r[2], hv, lv;
            hv = hi >= '0' && hi <= '9' ? hi - '0'
               : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10
               : hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
            lv = lo >= '0' && lo <= '9' ? lo - '0'
               : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10
               : lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
            if (hv >= 0 && lv >= 0) { *w++ = (char)(hv * 16 + lv); r += 3; continue; }
        }
        *w++ = (*r == '+') ? ' ' : *r;
        r++;
    }
    *w = 0;
    return (int)(w - s);
}

static void ds_field(char *dst, int cap, const char *val) {
    int n = 0;
    while (val[n] && n < cap - 1) { dst[n] = val[n]; n++; }
    dst[n] = 0;
}

static char *ds_strtok(char *s, const char *delim, char **save) {
    char *tok;
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { *save = NULL; return NULL; }
    tok = s;
    s += strcspn(s, delim);
    *save = *s ? s + 1 : NULL;
    if (*s) *s = 0;
    return tok;
}

static int ds_fmt_u64(char *out, int cap, long long v) {
    char tmp[24];
    int n = 0, i = 0;
    unsigned long long u = (unsigned long long)v;
    if (u == 0) tmp[n++] = '0';
    while (u && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    while (n > 0 && i < cap - 1) out[i++] = tmp[--n];
    out[i] = 0;
    return i;
}

/* ---- request parsing ---------------------------------------------------- */

static int ds_method_code(const char *m) {
    if (!strcmp(m, "POST"))   return 1;
    if (!strcmp(m, "PUT"))    return 2;
    if (!strcmp(m, "DELETE")) return 3;
    if (!strcmp(m, "HEAD"))   return 4;
    if (!strcmp(m, "PATCH"))  return 5;
    return 0;
}

static void ds_parse_request(DsConn *c, char *hdr) {
    char *save = NULL;
    char *line = ds_strtok(hdr, "\r\n", &save);
    c->method     = 0;
    c->body_len   = -1;
    c->range_off  = 0;
    c->range_len  = -1;
    c->keep_alive = 0;

    if (line) {
        char m[16] = {0};
        char u[400] = {0};
        if (sscanf(line, "%15s %399s", m, u) == 2)
            c->method = ds_method_code(m);
        {
            char *q = strchr(u, '?');
            if (q) { *q = 0; ds_field(c->query, (int)sizeof(c->query), q + 1); }
        }
        ds_field(c->path, (int)sizeof(c->path), u);
        ds_url_decode(c->path);
    }
    while ((line = ds_strtok(NULL, "\r\n", &save)) != NULL) {
        char *colon = strchr(line, ':');
        char *v;
        if (!colon) continue;
        *colon = 0;
        v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if      (!ds_ieq(line, "authorization"))  ds_field(c->token, (int)sizeof(c->token), v);
        else if (!ds_ieq(line, "user-agent"))     ds_field(c->ua,    (int)sizeof(c->ua),    v);
        else if (!ds_ieq(line, "content-length")) c->body_len = strtoll(v, NULL, 10);
        else if (!ds_ieq(line, "range")) {
            if (ds_istarts(v, "bytes=")) {
                long long a = -1, b = -1;
                const char *p = v + 6;
                if (*p >= '0' && *p <= '9') a = strtoll(p, (char **)&p, 10);
                if (*p == '-') b = strtoll(p + 1, NULL, 10);
                if (a >= 0) {
                    c->range_off = a;
                    c->range_len = (b >= a) ? (b - a + 1) : -1;
                }
            }
        }
        else if (!ds_ieq(line, "connection")) {
            if (ds_icontains(v, "keep-alive")) c->keep_alive = 1;
        }
    }
}

/* ---- state machine ------------------------------------------------------ */

static void ds_close_conn(DsConn *c) {
    if (c->fp) { fclose(c->fp); c->fp = NULL; }
    if (c->fd >= 0) tdn_close(c->fd);
    free(c->snd);  c->snd  = NULL;
    free(c->body); c->body = NULL;
    c->fd     = -1;
    c->state  = DS_ST_DEAD;
    c->in_use = 0;
}

/* Pull request headers off READING conns. Called from SRV_POLL (and once
 * per XFER/STAT so progress doesn't stall between polls). */
static void ds_pump_reads(DsState *st) {
    int i;
    for (i = 0; i < DS_MAX_CONN; i++) {
        DsConn *c = &st->conn[i];
        if (!c->in_use || c->state != DS_ST_READING) continue;
        for (;;) {
            int r;
            int j, hdr_end = -1;
            if (c->hdr_used >= DS_HDR_CAP - 1) {
                /* headers too big */
                int sidx = c->srv_idx;
                if (sidx >= 0 && st->srv[sidx].in_use)
                    st->srv[sidx].reqs_failed++;
                ds_close_conn(c);
                break;
            }
            r = tdn_recv(c->fd, c->hdrbuf + c->hdr_used,
                         DS_HDR_CAP - 1 - c->hdr_used);
            if (r == 0) {            /* peer closed mid-headers */
                int sidx = c->srv_idx;
                if (sidx >= 0 && st->srv[sidx].in_use)
                    st->srv[sidx].reqs_failed++;
                ds_close_conn(c);
                break;
            }
            if (r < 0) break;        /* would block */
            c->hdr_used += r;
            c->hdrbuf[c->hdr_used] = 0;
            for (j = 0; j + 3 < c->hdr_used; j++)
                if (c->hdrbuf[j]   == '\r' && c->hdrbuf[j+1] == '\n' &&
                    c->hdrbuf[j+2] == '\r' && c->hdrbuf[j+3] == '\n') {
                    hdr_end = j + 4;
                    break;
                }
            if (hdr_end < 0) continue;

            /* complete request: split off body bytes that arrived with it */
            {
                int sidx = c->srv_idx;
                int extra = c->hdr_used - hdr_end;
                if (extra > DS_PRE_CAP) extra = DS_PRE_CAP;
                memcpy(c->body_pre, c->hdrbuf + hdr_end, (size_t)extra);
                c->body_pre_len = extra;
                c->hdrbuf[hdr_end - 2] = 0;
                ds_parse_request(c, c->hdrbuf);
                c->state = DS_ST_READY;
                if (sidx >= 0 && st->srv[sidx].in_use &&
                    st->srv[sidx].ready_n < DS_MAX_CONN) {
                    st->srv[sidx].ready[st->srv[sidx].ready_n++] = i;
                    st->srv[sidx].reqs_total++;
                } else {
                    c->state = DS_ST_READING;
                    ds_close_conn(c);
                }
            }
            break;
        }
    }
}

/* Push pending reply bytes. Called from SRV_POLL/XFER. */
static void ds_pump_writes(DsState *st) {
    int i;
    for (i = 0; i < DS_MAX_CONN; i++) {
        DsConn *c = &st->conn[i];
        int sidx;
        if (!c->in_use || c->state != DS_ST_REPLYING) continue;
        sidx = c->srv_idx;

        /* response headers first */
        while (c->snd_pos < c->snd_len) {
            int w = tdn_send(c->fd, c->snd + c->snd_pos,
                             c->snd_len - c->snd_pos);
            if (w <= 0) break;
            c->snd_pos += w;
        }
        if (c->snd_pos < c->snd_len) continue;   /* tx window full */

        /* then in-memory body */
        while (c->body_pos < c->body_len_out) {
            int w = tdn_send(c->fd, (const char *)c->body + c->body_pos,
                             c->body_len_out - c->body_pos);
            if (w <= 0) break;
            c->body_pos += w;
            c->sent += w;
        }
        if (c->body_pos < c->body_len_out) continue;

        /* then file bytes */
        while (c->fp && c->file_left > 0) {
            char fb[DS_FILE_CHUNK];
            int want = (c->file_left > (long long)sizeof(fb))
                     ? (int)sizeof(fb) : (int)c->file_left;
            int nr = (int)fread(fb, 1, (size_t)want, c->fp);
            int w;
            if (nr <= 0) { c->file_left = 0; break; }
            w = tdn_send(c->fd, fb, nr);
            if (w <= 0) break;
            c->file_left -= w;
            c->sent      += w;
            if (w < nr) fseek(c->fp, (long)(w - nr), SEEK_CUR);
        }
        if (c->fp && c->file_left > 0) continue;

        /* everything out the door */
        {
            long long total_sent = (long long)c->snd_len + c->sent;
            if (c->fp) { fclose(c->fp); c->fp = NULL; }
            if (sidx >= 0 && st->srv[sidx].in_use)
                st->srv[sidx].bytes_sent += total_sent;
            ds_close_conn(c);
        }
    }
}

/* Drain tdn: pump the stack, accept pending conns, advance all reads and
 * writes. Called at the top of SRV_POLL and lightly from the other cmds. */
static void ds_pump(DsState *st) {
    int i;
    tdn_poll();
    for (i = 0; i < DS_MAX_SRV; i++) {
        int cfd;
        if (!st->srv[i].in_use) continue;
        while ((cfd = tdn_accept(st->srv[i].lfd)) >= 0) {
            int j;
            for (j = 0; j < DS_MAX_CONN; j++)
                if (!st->conn[j].in_use) break;
            if (j == DS_MAX_CONN) { tdn_close(cfd); st->srv[i].reqs_failed++; continue; }
            memset(&st->conn[j], 0, sizeof(DsConn));
            st->conn[j].in_use  = 1;
            st->conn[j].gen++;
            if (st->conn[j].gen > 0xFFFFFF) st->conn[j].gen = 1;
            st->conn[j].fd      = cfd;
            st->conn[j].srv_idx = i;
            st->conn[j].state   = DS_ST_READING;
            st->conn[j].total   = -1;
            st->conn[j].body_len = -1;
            st->conn[j].range_len = -1;
            ds_ip_str(tdn_peerip(cfd), st->conn[j].ip,
                      (int)sizeof(st->conn[j].ip));
        }
    }
    ds_pump_reads(st);
    ds_pump_writes(st);
}

/* ---- the builtins ------------------------------------------------------- */

static void dos_sys_serve(RelicVM *vm) {
    DsState *st = ds_state_for(vm);
    int cmd, h, i;
    u32 p_ptr;
    i32 n;

    if (!st) { vm->r[1] = -1; return; }
    cmd   = vm->r[1];
    h     = vm->r[2];
    p_ptr = (u32)vm->r[3];
    n     = vm->r[4];

    switch (cmd) {

    case 0: { /* SRV_OPEN: r2=port, r3=bind *u8 or 0, r4=flags -> handle */
        int slot = -1;
        int lfd;
        if (tdn_init() < 0) { vm->r[1] = -3; return; }   /* stack up lazily */
        (void)p_ptr;   /* bind addr ignored: lwIP has a single netif */
        for (i = 0; i < DS_MAX_SRV; i++)
            if (!st->srv[i].in_use) { slot = i; break; }
        if (slot < 0) { vm->r[1] = -1; return; }
        lfd = tdn_listen((uint16_t)h);
        if (lfd < 0) { vm->r[1] = -2; return; }
        memset(&st->srv[slot], 0, sizeof(DsSrv));
        st->srv[slot].in_use = 1;
        st->srv[slot].lfd    = lfd;
        /* resolved local port (tdn_listen(0) -> lwIP ephemeral) so
         * SRV_STAT reports the real port like the desktop backend. */
        st->srv[slot].port   = (int)tdn_localport(lfd);
        vm->r[1] = slot;
        return;
    }

    case 1: { /* SRV_CLOSE: r2=srv */
        if (h < 0 || h >= DS_MAX_SRV) { vm->r[1] = -1; return; }
        if (st->srv[h].in_use) {
            tdn_close(st->srv[h].lfd);
            st->srv[h].in_use = 0;
            for (i = 0; i < DS_MAX_CONN; i++)
                if (st->conn[i].in_use && st->conn[i].srv_idx == h)
                    ds_close_conn(&st->conn[i]);
        }
        vm->r[1] = 0;
        return;
    }

    case 2: { /* SRV_POLL: r2=srv, r3=*ServeRequest, r4=sizeof -> req/-1 */
        DsSrv *sv;
        int req_idx = -1;
        u32 cap_end;

        if (h < 0 || h >= DS_MAX_SRV || n < 1) { vm->r[1] = -1; return; }
        if (!p_ptr || p_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
        cap_end = p_ptr + (u32)n;
        if (cap_end > vm->mem_sz) cap_end = vm->mem_sz;

        ds_pump(st);
        sv = st->srv[h].in_use ? &st->srv[h] : NULL;
        if (!sv) { vm->r[1] = -1; return; }

        if (sv->ready_n > 0) {
            req_idx = sv->ready[0];
            for (i = 1; i < sv->ready_n; i++) sv->ready[i-1] = sv->ready[i];
            sv->ready_n--;
        }
        if (req_idx >= 0) {
            DsConn *c = &st->conn[req_idx];
            i32 req_handle;
            c->state = DS_ST_HANDED;
            req_handle = (c->gen << 8) | req_idx;
            ds_vm_put32(vm, p_ptr + 0,  (i32)n, cap_end);
            ds_vm_put32(vm, p_ptr + 4,  req_handle, cap_end);
            ds_vm_put32(vm, p_ptr + 8,  (i32)c->method, cap_end);
            ds_vm_put32(vm, p_ptr + 12, (i32)c->body_len, cap_end);
            ds_vm_put32(vm, p_ptr + 16, (i32)c->range_off, cap_end);
            ds_vm_put32(vm, p_ptr + 20, (i32)c->range_len, cap_end);
            ds_vm_put32(vm, p_ptr + 24, (i32)c->keep_alive, cap_end);
            ds_vm_put32(vm, p_ptr + 28, 0, cap_end);
            ds_vm_str_out(vm, p_ptr + 32,  c->ip,    32,  cap_end);
            ds_vm_str_out(vm, p_ptr + 64,  c->token, 96,  cap_end);
            ds_vm_str_out(vm, p_ptr + 160, c->path,  256, cap_end);
            ds_vm_str_out(vm, p_ptr + 416, c->query, 96,  cap_end);
            ds_vm_str_out(vm, p_ptr + 512, c->ua,    64,  cap_end);
            vm->r[1] = req_handle;
            return;
        }
        vm->r[1] = -1;
        return;
    }

    case 3: { /* SRV_READ: r2=req handle, r3=buf, r4=cap -> bytes/0/-1 */
        int idx = h & 0xFF, gen = (h >> 8) & 0xFFFFFF;
        int r = 0;
        u32 want;
        if (!p_ptr || p_ptr >= vm->mem_sz || n <= 0) { vm->r[1] = -1; return; }
        if (idx >= DS_MAX_CONN) { vm->r[1] = -1; return; }
        tdn_poll();
        if (!st->conn[idx].in_use || st->conn[idx].gen != gen ||
            st->conn[idx].state != DS_ST_HANDED) {
            vm->r[1] = -1;
            return;
        }
        want = (u32)n;
        if (p_ptr + want > vm->mem_sz) want = vm->mem_sz - p_ptr;
        if (st->conn[idx].body_pre_pos < st->conn[idx].body_pre_len) {
            int avail = st->conn[idx].body_pre_len - st->conn[idx].body_pre_pos;
            if (avail > (int)want) avail = (int)want;
            memcpy(vm->mem + p_ptr,
                   st->conn[idx].body_pre + st->conn[idx].body_pre_pos,
                   (size_t)avail);
            st->conn[idx].body_pre_pos += avail;
            vm->r[1] = avail;
            return;
        }
        r = tdn_recv(st->conn[idx].fd, vm->mem + p_ptr, (int)want);
        if (r < 0) r = 0;            /* would block -> 0 this poll */
        vm->r[1] = r;
        return;
    }

    case 4: { /* SRV_DROP: r2=req handle */
        int idx = h & 0xFF, gen = (h >> 8) & 0xFFFFFF;
        if (idx >= DS_MAX_CONN) { vm->r[1] = -1; return; }
        if (st->conn[idx].in_use && st->conn[idx].gen == gen &&
            st->conn[idx].state < DS_ST_REPLYING) {
            int sidx = st->conn[idx].srv_idx;
            if (sidx >= 0 && st->srv[sidx].in_use)
                st->srv[sidx].reqs_failed++;
            ds_close_conn(&st->conn[idx]);
            vm->r[1] = 0;
        } else {
            vm->r[1] = -1;
        }
        return;
    }

    case 5: { /* SRV_STAT: r2=srv, r3=*ServeStat, r4=sizeof -> 0/-1 */
        DsSrv *sv;
        u32 cap_end;
        int open_conns = 0, xfer = 0;
        if (h < 0 || h >= DS_MAX_SRV || n < 1 ||
            !p_ptr || p_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
        cap_end = p_ptr + (u32)n;
        if (cap_end > vm->mem_sz) cap_end = vm->mem_sz;
        sv = st->srv[h].in_use ? &st->srv[h] : NULL;
        if (!sv) { vm->r[1] = -1; return; }
        for (i = 0; i < DS_MAX_CONN; i++)
            if (st->conn[i].in_use && st->conn[i].srv_idx == h) {
                open_conns++;
                if (st->conn[i].state == DS_ST_REPLYING) xfer++;
            }
        ds_vm_put32(vm, p_ptr + 0,  (i32)n, cap_end);
        ds_vm_put32(vm, p_ptr + 4,  (i32)sv->port, cap_end);
        ds_vm_put32(vm, p_ptr + 8,  open_conns, cap_end);
        ds_vm_put32(vm, p_ptr + 12, (i32)sv->reqs_total, cap_end);
        ds_vm_put32(vm, p_ptr + 16, (i32)sv->reqs_failed, cap_end);
        ds_vm_put32(vm, p_ptr + 20, (i32)(sv->bytes_sent & 0xFFFFFFFFLL),
                    cap_end);
        ds_vm_put32(vm, p_ptr + 24, (i32)(sv->bytes_sent >> 32), cap_end);
        ds_vm_put32(vm, p_ptr + 28, xfer, cap_end);
        vm->r[1] = 0;
        return;
    }

    case 6: { /* SRV_XFER: r2=srv, r3=*ServeXfer[], r4=sizeof buf -> count */
        int wrote = 0;
        u32 cap_end;
        if (h < 0 || h >= DS_MAX_SRV || n < DS_XFER_SIZE ||
            !p_ptr || p_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
        cap_end = p_ptr + (u32)n;
        if (cap_end > vm->mem_sz) cap_end = vm->mem_sz;
        ds_pump(st);
        if (!st->srv[h].in_use) { vm->r[1] = -1; return; }
        for (i = 0; i < DS_MAX_CONN; i++) {
            DsConn *c = &st->conn[i];
            u32 off;
            if (!c->in_use || c->srv_idx != h || c->state != DS_ST_REPLYING)
                continue;
            off = p_ptr + (u32)(wrote * DS_XFER_SIZE);
            if (off + DS_XFER_SIZE > cap_end) break;
            ds_vm_put32(vm, off + 0,  (c->gen << 8) | i, cap_end);
            ds_vm_put32(vm, off + 4,  1, cap_end);   /* always replying */
            ds_vm_put32(vm, off + 8,  (i32)(c->sent  & 0xFFFFFFFFLL), cap_end);
            ds_vm_put32(vm, off + 12, (i32)(c->sent  >> 32), cap_end);
            ds_vm_put32(vm, off + 16, (i32)(c->total & 0xFFFFFFFFLL), cap_end);
            ds_vm_put32(vm, off + 20, (i32)(c->total >> 32), cap_end);
            ds_vm_put32(vm, off + 24, (i32)c->reply_status, cap_end);
            ds_vm_str_out(vm, off + 28, c->ip,   24,  cap_end);
            ds_vm_str_out(vm, off + 52, c->path, 124, cap_end);
            wrote++;
        }
        vm->r[1] = wrote;
        return;
    }

    default:
        vm->r[1] = -1;
        return;
    }
}

static void dos_sys_serve_reply(RelicVM *vm) {
    DsState *st = ds_state_for(vm);
    int req   = vm->r[1];
    int rcode = vm->r[2];
    u32 hdr   = (u32)vm->r[3];
    u32 body  = (u32)vm->r[4];
    i32 blen  = vm->r[5];
    int idx   = req & 0xFF, gen = (req >> 8) & 0xFFFFFF;
    DsConn *c;

    if (!st || idx >= DS_MAX_CONN) { vm->r[1] = -1; return; }
    if (!st->conn[idx].in_use || st->conn[idx].gen != gen ||
        st->conn[idx].state != DS_ST_HANDED) {
        vm->r[1] = -1;
        return;
    }
    c = &st->conn[idx];
    c->state        = DS_ST_REPLYING;
    c->reply_status = rcode;

    if (blen == -1) {                       /* host path: stream the file */
        char path[1024];
        long long flen, off = c->range_off;
        ds_vm_str(vm, body, path, sizeof(path));
        if (rcode == 0) rcode = 200;
        {
            FILE *probe = fopen(path, "rb");
            flen = 0;
            if (probe) {
                fseek(probe, 0, SEEK_END);
                flen = (long long)ftell(probe);
                fclose(probe);
            }
        }
        if (c->range_len >= 0 || off > 0) {
            long long want = c->range_len;
            if (want < 0) want = flen - off;
            if (off > flen) off = flen;
            if (want > flen - off) want = flen - off;
            if (want < 0) want = 0;
            c->fp = fopen(path, "rb");
            if (c->fp && off > 0) fseek(c->fp, (long)off, SEEK_SET);
            c->file_left = want;
            rcode = 206;
        } else {
            c->fp = fopen(path, "rb");
            c->file_left = flen;
        }
        if (!c->fp) c->file_left = 0;
        c->total = c->file_left;
    } else {
        if (blen > 0 && body && body + (u32)blen <= vm->mem_sz) {
            c->body = (unsigned char *)malloc((size_t)blen);
            if (c->body) {
                memcpy(c->body, vm->mem + body, (size_t)blen);
                c->body_len_out = blen;
            }
        }
        if (rcode == 0) rcode = 200;
        c->total = blen;
    }

    /* response header block */
    {
        char extra[4096];
        char hbuf[8192];
        char clen[24];
        int en = 0, hn;
        const char *rp =
            rcode == 200 ? "OK" : rcode == 206 ? "Partial Content" :
            rcode == 400 ? "Bad Request" : rcode == 401 ? "Unauthorized" :
            rcode == 403 ? "Forbidden" : rcode == 404 ? "Not Found" :
            rcode == 416 ? "Range Not Satisfiable" :
            rcode == 500 ? "Internal Server Error" : "OK";
        extra[0] = 0;
        if (hdr && hdr < vm->mem_sz)
            en = ds_vm_str(vm, hdr, extra, sizeof(extra) - 2);
        ds_fmt_u64(clen, sizeof(clen), c->fp ? c->file_left : 0);
        if (blen == -1)
            hn = snprintf(hbuf, sizeof(hbuf),
                "HTTP/1.0 %d %s\r\nContent-Length: %s\r\n"
                "Connection: close\r\n%s%s\r\n",
                rcode, rp, clen,
                extra, (en && extra[en-1] != '\n') ? "\r\n" : "");
        else
            hn = snprintf(hbuf, sizeof(hbuf),
                "HTTP/1.0 %d %s\r\nContent-Length: %d\r\n"
                "Connection: close\r\n%s%s\r\n",
                rcode, rp, blen > 0 ? blen : 0,
                extra, (en && extra[en-1] != '\n') ? "\r\n" : "");
        c->snd = (char *)malloc((size_t)hn + 1);
        if (c->snd) { memcpy(c->snd, hbuf, (size_t)hn); c->snd_len = hn; }
        c->reply_status = rcode;
    }

    if (!c->snd) {
        int sidx = c->srv_idx;
        if (sidx >= 0 && st->srv[sidx].in_use) st->srv[sidx].reqs_failed++;
        ds_close_conn(c);
        vm->r[1] = -1;
        return;
    }
    vm->r[1] = 0;
    return;
}

#endif /* BACKEND_DOS_SERVER_H */
