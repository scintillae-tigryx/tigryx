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
 * nsnf_server.h - shared POSIX implementation of @serve / @serve_reply
 * (the experimental "NSNF" tiny socket server).
 *
 * Only compiled into a backend when TIGRYX_EXPERIMENTAL_SERVER is defined;
 * the including .c file does the #ifdef itself and wires the vtable, e.g.:
 *
 *     #ifdef TIGRYX_EXPERIMENTAL_SERVER
 *     #include "common/nsnf_server.h"
 *     #endif
 *     ...
 *     #ifdef TIGRYX_EXPERIMENTAL_SERVER
 *     .sys_serve       = nsnf_sys_serve,
 *     .sys_serve_reply = nsnf_sys_serve_reply,
 *     #endif
 *
 * and calls nsnf_serve_shutdown(vm) from its backend shutdown path.
 *
 * Design: the host implements a generic, minimal HTTP
 * listener only - it knows nothing about NSNF, shares or auth; all of
 * that lives in Tigryx. Nothing here ever blocks the VM thread: accepted
 * connections are handed to a reader thread that parses headers, and
 * replies are written by a sender thread.
 *
 *   SRV_OPEN  -> listen socket, non-blocking, SO_REUSEADDR
 *   SRV_POLL  -> accept() + drain the "headers parsed" queue, fill the
 *                576-byte ServeRequest struct in guest memory
 *   SRV_READ  -> non-blocking read of a request body
 *   SRV_DROP  -> close the connection, free the slot
 *   SRV_REPLY -> send status + headers + body; body_len == -1 means `body`
 *                is a HOST filesystem path streamed asynchronously by a
 *                worker thread (Content-Length set from stat(), Range
 *                honoured, 206 semantics). The VM never holds the file.
 *   SRV_STAT  -> fill the 48-byte ServeStat struct
 *   SRV_XFER  -> fill an array of 176-byte ServeXfer records, one per
 *                in-flight streamed reply
 *
 * Guest-visible structs are packed, host byte order, size-prefixed: the
 * first i32 is the caller's sizeof, and the host never writes past it.
 */

#ifndef NSNF_SERVER_H
#define NSNF_SERVER_H

#include "tigryx.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
/* Winsock2 flavour of the same implementation. The including backend must
 * have set WIN32_LEAN_AND_MEAN before <windows.h> (both win32 backends do)
 * so winsock.h is not already pulled in. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <process.h>
#else
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

/* ---- platform compat ----------------------------------------------------
 * Sockets, threads and mutexes, kept to the few verbs the body uses. */

#ifdef _WIN32

typedef SOCKET            nsnf_fd_t;
#define NSNF_BAD_FD       INVALID_SOCKET
typedef int               nsnf_socklen_t;
typedef CRITICAL_SECTION  nsnf_mutex_t;
#define nsnf_mutex_init(m)    InitializeCriticalSection(m)
#define nsnf_mutex_lock(m)    EnterCriticalSection(m)
#define nsnf_mutex_unlock(m)  LeaveCriticalSection(m)

/* The per-process registry is guarded by a spinlock so it needs no init
 * call and works on everything from Win95 up. */
static volatile LONG g_nsnf_reg_spin = 0;
#define nsnf_reg_lock() \
    do { while (InterlockedCompareExchange(&g_nsnf_reg_spin, 1, 0)) Sleep(0); } while (0)
#define nsnf_reg_unlock() InterlockedExchange(&g_nsnf_reg_spin, 0)

static int g_nsnf_wsa = 0;   /* 0 untried, 1 up, -1 failed */
static int nsnf_wsa_up(void) {
    if (!g_nsnf_wsa) {
        WSADATA wd;
        g_nsnf_wsa = (WSAStartup(MAKEWORD(2, 2), &wd) == 0) ? 1 : -1;
    }
    return g_nsnf_wsa > 0;
}

typedef void *(*nsnf_thread_fn)(void *);
typedef struct { nsnf_thread_fn fn; void *arg; } NsnfThunk;
static unsigned __stdcall nsnf_thunk(void *p) {
    NsnfThunk *t = (NsnfThunk *)p;
    nsnf_thread_fn fn = t->fn;
    void *arg = t->arg;
    free(t);
    fn(arg);
    return 0;
}
static int nsnf_spawn(nsnf_thread_fn fn, void *arg) {
    NsnfThunk *t = (NsnfThunk *)malloc(sizeof(NsnfThunk));
    uintptr_t h;
    if (!t) return -1;
    t->fn = fn; t->arg = arg;
    h = _beginthreadex(NULL, 0, nsnf_thunk, t, 0, NULL);
    if (!h) { free(t); return -1; }
    CloseHandle((HANDLE)h);          /* detached */
    return 0;
}

#else /* POSIX */

typedef int               nsnf_fd_t;
#define NSNF_BAD_FD       (-1)
typedef socklen_t         nsnf_socklen_t;
typedef pthread_mutex_t   nsnf_mutex_t;
#define nsnf_mutex_init(m)    pthread_mutex_init(m, NULL)
#define nsnf_mutex_lock(m)    pthread_mutex_lock(m)
#define nsnf_mutex_unlock(m)  pthread_mutex_unlock(m)

static pthread_mutex_t g_nsnf_reg_lock = PTHREAD_MUTEX_INITIALIZER;
#define nsnf_reg_lock()   pthread_mutex_lock(&g_nsnf_reg_lock)
#define nsnf_reg_unlock() pthread_mutex_unlock(&g_nsnf_reg_lock)

typedef void *(*nsnf_thread_fn)(void *);
static int nsnf_spawn(nsnf_thread_fn fn, void *arg) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, fn, arg) != 0) return -1;
    pthread_detach(tid);
    return 0;
}

#endif

static void nsnf_fd_close(nsnf_fd_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static void nsnf_fd_shutdown(nsnf_fd_t fd) {
#ifdef _WIN32
    shutdown(fd, SD_BOTH);
#else
    shutdown(fd, SHUT_RDWR);
#endif
}

static void nsnf_fd_nonblock(nsnf_fd_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Restore blocking mode. Needed on accept()ed sockets: BSD/macOS
 * inherit the listener's O_NONBLOCK, and the reader thread's recv()
 * loop is written for a blocking socket (EAGAIN would look like a
 * dead connection). Linux doesn't inherit, so this is a no-op there. */
static void nsnf_fd_block(nsnf_fd_t fd) {
#ifdef _WIN32
    u_long mode = 0;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

static int nsnf_sock_send(nsnf_fd_t fd, const char *buf, int n) {
#ifdef _WIN32
    return send(fd, buf, n, 0);
#else
#ifdef MSG_NOSIGNAL
    return (int)send(fd, buf, (size_t)n, MSG_NOSIGNAL);
#else
    return (int)send(fd, buf, (size_t)n, 0);
#endif
#endif
}

static int nsnf_sock_recv(nsnf_fd_t fd, void *buf, int n) {
    return (int)recv(fd, (char *)buf, n, 0);
}

static int nsnf_sock_intr(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

/* Zero-timeout readability probe (portable non-blocking peek). */
static int nsnf_can_read(nsnf_fd_t fd) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 0; tv.tv_usec = 0;
#ifdef _WIN32
    return select(0, &fds, NULL, NULL, &tv);
#else
    return select(fd + 1, &fds, NULL, NULL, &tv);
#endif
}

static int nsnf_inet_parse(const char *s, struct in_addr *out) {
#ifdef _WIN32
    unsigned long a = inet_addr(s);
    if (a == INADDR_NONE && strcmp(s, "255.255.255.255") != 0) return -1;
    out->s_addr = a;
    return 0;
#else
    return inet_pton(AF_INET, s, out) == 1 ? 0 : -1;
#endif
}

static void nsnf_inet_str(const struct in_addr *a, char *out, int cap) {
#ifdef _WIN32
    int n = 0;
    const char *s = inet_ntoa(*a);   /* TLS buffer; copied immediately */
    if (s) while (s[n] && n < cap - 1) { out[n] = s[n]; n++; }
    out[n] = 0;
#else
    (void)cap;
    inet_ntop(AF_INET, a, out, 32);
#endif
}

/* Case-insensitive helpers (strcasecmp/strtok_r are not on MSVC). */
static int nsnf_ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 1;
        if (!x) return 0;
    }
}

static int nsnf_istarts(const char *s, const char *pfx) {
    while (*pfx) {
        char x = *s, y = *pfx;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        s++; pfx++;
    }
    return 1;
}

static char *nsnf_strtok(char *s, const char *delim, char **save) {
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

/* 64-bit-safe file size / seek. */
static long long nsnf_filesize(const char *path) {
#ifdef _WIN32
    struct _stati64 st;
    return _stati64(path, &st) == 0 ? (long long)st.st_size : 0;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : 0;
#endif
}

static void nsnf_fseek64(FILE *f, long long off) {
#ifdef _WIN32
    _fseeki64(f, off, SEEK_SET);
#else
    fseeko(f, (off_t)off, SEEK_SET);
#endif
}

/* Format an unsigned 64-bit value without relying on %lld (msvcrt.dll on
 * Win95 targets has no long-long conversion). */
static int nsnf_fmt_u64(char *out, int cap, long long v) {
    char tmp[24];
    int n = 0, i = 0;
    unsigned long long u = (unsigned long long)v;
    if (u == 0) tmp[n++] = '0';
    while (u && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    while (n > 0 && i < cap - 1) out[i++] = tmp[--n];
    out[i] = 0;
    return i;
}

#define NSNF_MAX_VM    4    /* VMs with live server state (few per process) */
#define NSNF_MAX_SRV   4    /* listeners per VM */
#define NSNF_MAX_CONN  32   /* connection slots per VM */
#define NSNF_HDR_CAP   16384

/* ServeRequest guest layout (576 bytes packed). */
#define NSNF_REQ_SIZE  576
/* ServeStat guest layout (48 bytes packed). */
#define NSNF_STAT_SIZE 48
/* ServeXfer record stride (176 bytes packed per record). */
#define NSNF_XFER_SIZE 176

enum {
    NSNF_ST_READING = 0,  /* reader thread still reading headers   */
    NSNF_ST_READY,        /* parsed, sitting in the ready queue    */
    NSNF_ST_HANDED,       /* given to the guest by SRV_POLL        */
    NSNF_ST_REPLYING,     /* a sender thread owns the connection   */
    NSNF_ST_DONE,         /* response fully sent                   */
    NSNF_ST_DEAD          /* dropped / errored                     */
};

typedef struct {
    int       in_use;
    int       gen;          /* handle = (gen<<8)|idx, invalidated on reuse */
    nsnf_fd_t fd;
    int       srv_idx;
    int       state;
    char      ip[32];

    /* parsed request */
    int       method;
    long long body_len;     /* declared Content-Length, -1 unknown */
    long long range_off;    /* parsed Range start, 0 if none        */
    long long range_len;    /* -1 = to end / no range               */
    int       keep_alive;
    char      token[96];
    char      ua[64];
    char      path[256];
    char      query[96];

    /* request body bytes that arrived in the same segment as the
     * headers — served first by SRV_READ before the socket */
    unsigned char body_pre[8192];
    int       body_pre_len;
    int       body_pre_pos;

    /* reply progress (sender thread writes, SRV_XFER reads) */
    int       reply_started;
    long long sent;
    long long total;        /* -1 unknown (in-memory body w/o length) */
    int       reply_status; /* status code chosen by the guest       */
} NsnfConn;

typedef struct {
    int  in_use;
    nsnf_fd_t lfd;          /* listening socket */
    int  port;              /* bound port */
    long long reqs_total;
    long long reqs_failed;
    long long bytes_sent;
    /* ready queue: conn indices whose headers are parsed */
    int  ready[NSNF_MAX_CONN];
    int  ready_n;
} NsnfSrv;

typedef struct {
    int             in_use;
    RelicVM        *vm;
    nsnf_mutex_t    lock;
    NsnfSrv         srv[NSNF_MAX_SRV];
    NsnfConn        conn[NSNF_MAX_CONN];
} NsnfState;

static NsnfState      g_nsnf[NSNF_MAX_VM];

static NsnfState *nsnf_state_for(RelicVM *vm) {
    NsnfState *st = NULL;
    int i;
    nsnf_reg_lock();
    for (i = 0; i < NSNF_MAX_VM; i++) {
        if (g_nsnf[i].in_use && g_nsnf[i].vm == vm) { st = &g_nsnf[i]; break; }
    }
    if (!st) {
        for (i = 0; i < NSNF_MAX_VM; i++) {
            if (!g_nsnf[i].in_use) {
                memset(&g_nsnf[i], 0, sizeof(NsnfState));
                g_nsnf[i].in_use = 1;
                g_nsnf[i].vm     = vm;
                nsnf_mutex_init(&g_nsnf[i].lock);
                st = &g_nsnf[i];
                break;
            }
        }
    }
    nsnf_reg_unlock();
    return st;
}

/* Called by the backend's shutdown path. Closes listeners and conns. */
static void nsnf_serve_shutdown(RelicVM *vm) {
    NsnfState *st = nsnf_state_for(vm);
    int i, j;
    if (!st) return;
    nsnf_mutex_lock(&st->lock);
    for (i = 0; i < NSNF_MAX_SRV; i++)
        if (st->srv[i].in_use) {
            nsnf_fd_close(st->srv[i].lfd);
            st->srv[i].in_use = 0;
        }
    for (j = 0; j < NSNF_MAX_CONN; j++)
        if (st->conn[j].in_use) {
            nsnf_fd_shutdown(st->conn[j].fd);
            nsnf_fd_close(st->conn[j].fd);
            st->conn[j].in_use = 0;
        }
    nsnf_mutex_unlock(&st->lock);
}

/* ---- small helpers ----------------------------------------------------- */

static void nsnf_no_sigpipe(nsnf_fd_t fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

static int nsnf_send_all(nsnf_fd_t fd, const char *buf, int n) {
    int off = 0;
    while (off < n) {
        int w = nsnf_sock_send(fd, buf + off, n - off);
        if (w <= 0) {
            if (w < 0 && nsnf_sock_intr()) continue;
            return -1;
        }
        off += w;
    }
    return 0;
}

static void nsnf_close_conn(NsnfConn *c) {
    if (c->fd != NSNF_BAD_FD) { nsnf_fd_shutdown(c->fd); nsnf_fd_close(c->fd); }
    c->fd     = NSNF_BAD_FD;
    c->state  = NSNF_ST_DEAD;
    c->in_use = 0;          /* gen stays bumped; stale handles fail */
}

/* Copy a NUL-terminated guest string into a host buffer. */
static int nsnf_vm_str(RelicVM *vm, u32 ptr, char *out, int cap) {
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

static void nsnf_vm_put32(RelicVM *vm, u32 off, i32 v, u32 cap_end) {
    if (off + 4 <= cap_end && off + 4 <= vm->mem_sz)
        memcpy(vm->mem + off, &v, 4);
}

static void nsnf_vm_str_out(RelicVM *vm, u32 off, const char *s, int cap,
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

/* ---- header parsing ---------------------------------------------------- */

static int nsnf_method_code(const char *m) {
    if (!strcmp(m, "POST"))   return 1;
    if (!strcmp(m, "PUT"))    return 2;
    if (!strcmp(m, "DELETE")) return 3;
    if (!strcmp(m, "HEAD"))   return 4;
    if (!strcmp(m, "PATCH"))  return 5;
    return 0;
}

/* strcasestr() is BSD/GNU-only - roll our own for Solaris. */
static int nsnf_icontains(const char *hay, const char *needle) {
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

/* URL-decode in place (%XX + '+'). Returns length. */
static int nsnf_url_decode(char *s) {
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

/* Copy a header value (truncated) into a capped field. */
static void nsnf_field(char *dst, int cap, const char *val) {
    int n = 0;
    while (val[n] && n < cap - 1) { dst[n] = val[n]; n++; }
    dst[n] = 0;
}

/* Parse a completed header block. Fills the conn's request fields. */
static void nsnf_parse_request(NsnfConn *c, char *hdr) {
    char *save = NULL;
    char *line = nsnf_strtok(hdr, "\r\n", &save);
    c->method     = 0;
    c->body_len   = -1;
    c->range_off  = 0;
    c->range_len  = -1;
    c->keep_alive = 0;

    if (line) {
        /* "METHOD path?query HTTP/1.x" */
        char m[16] = {0};
        char u[400] = {0};
        if (sscanf(line, "%15s %399s", m, u) == 2)
            c->method = nsnf_method_code(m);
        char *q = strchr(u, '?');
        if (q) { *q = 0; nsnf_field(c->query, (int)sizeof(c->query), q + 1); }
        nsnf_field(c->path, (int)sizeof(c->path), u);
        nsnf_url_decode(c->path);
    }
    while ((line = nsnf_strtok(NULL, "\r\n", &save)) != NULL) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if      (!nsnf_ieq(line, "authorization"))  nsnf_field(c->token, (int)sizeof(c->token), v);
        else if (!nsnf_ieq(line, "user-agent"))     nsnf_field(c->ua,    (int)sizeof(c->ua),    v);
        else if (!nsnf_ieq(line, "content-length")) c->body_len = strtoll(v, NULL, 10);
        else if (!nsnf_ieq(line, "range")) {
            /* "bytes=A-B" or "bytes=A-" */
            if (nsnf_istarts(v, "bytes=")) {
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
        else if (!nsnf_ieq(line, "connection")) {
            if (nsnf_icontains(v, "keep-alive")) c->keep_alive = 1;
        }
    }
}

/* ---- reader / sender threads ------------------------------------------- */

typedef struct {
    NsnfState *st;
    int        idx;
    int        gen;
} NsnfWorkerCtx;

/* Read request headers off an accepted connection, then hand the conn to
 * the ready queue. Runs detached; touches only host-side state. */
static void *nsnf_reader(void *argp) {
    NsnfWorkerCtx *w = (NsnfWorkerCtx *)argp;
    NsnfState *st = w->st;
    int idx = w->idx, gen = w->gen;
    free(w);

    char *buf = (char *)malloc(NSNF_HDR_CAP);
    if (!buf) goto dead;
    {
        int   used = 0;
        int   hdr_end = -1;
        for (;;) {
            int r, i;
            if (used >= NSNF_HDR_CAP - 1) goto dead;   /* headers too big */
            r = nsnf_sock_recv(st->conn[idx].fd, buf + used,
                               NSNF_HDR_CAP - 1 - used);
            if (r == 0) goto dead;                     /* peer closed */
            if (r < 0) {
                if (nsnf_sock_intr()) continue;
                goto dead;
            }
            used += r;
            buf[used] = 0;
            for (i = 0; i + 3 < used; i++)
                if (buf[i] == '\r' && buf[i+1] == '\n' &&
                    buf[i+2] == '\r' && buf[i+3] == '\n') {
                    hdr_end = i + 4;
                    break;
                }
            if (hdr_end >= 0) break;
        }
        /* body bytes already received alongside the headers must not be
         * lost - carry them on the conn for SRV_READ */
        nsnf_mutex_lock(&st->lock);
        if (st->conn[idx].in_use && st->conn[idx].gen == gen) {
            int extra = used - hdr_end;
            if (extra > (int)sizeof(st->conn[idx].body_pre))
                extra = (int)sizeof(st->conn[idx].body_pre);
            memcpy(st->conn[idx].body_pre, buf + hdr_end, (size_t)extra);
            st->conn[idx].body_pre_len = extra;
        }
        nsnf_mutex_unlock(&st->lock);
        buf[hdr_end - 2] = 0;   /* terminate headers at the blank line */
    }
    nsnf_mutex_lock(&st->lock);
    if (st->conn[idx].in_use && st->conn[idx].gen == gen &&
        st->conn[idx].state == NSNF_ST_READING) {
        NsnfConn *c = &st->conn[idx];
        int sidx = c->srv_idx;
        nsnf_parse_request(c, buf);
        c->state = NSNF_ST_READY;
        if (sidx >= 0 && st->srv[sidx].in_use &&
            st->srv[sidx].ready_n < NSNF_MAX_CONN) {
            st->srv[sidx].ready[st->srv[sidx].ready_n++] = idx;
            st->srv[sidx].reqs_total++;
        } else {
            c->state = NSNF_ST_READING;
            nsnf_close_conn(c);
        }
    }
    nsnf_mutex_unlock(&st->lock);
    free(buf);
    return NULL;

dead:
    free(buf);
    nsnf_mutex_lock(&st->lock);
    if (st->conn[idx].in_use && st->conn[idx].gen == gen &&
        st->conn[idx].state == NSNF_ST_READING) {
        int sidx = st->conn[idx].srv_idx;
        if (sidx >= 0 && st->srv[sidx].in_use) st->srv[sidx].reqs_failed++;
        nsnf_close_conn(&st->conn[idx]);
    }
    nsnf_mutex_unlock(&st->lock);
    return NULL;
}

typedef struct {
    NsnfState *st;
    int        idx;
    int        gen;
    int        status;
    char      *hdr;      /* malloc'd header block */
    int        hdr_len;
    unsigned char *body; /* malloc'd copy, or NULL */
    int        body_len;
    char       file_path[1024]; /* when body_len == -1 mode used */
    int        is_file;
    long long  file_off;   /* seek start (Range)                  */
    long long  file_len;   /* bytes to stream                     */
} NsnfSendCtx;

/* Write the response then close the connection (HTTP/1.0 semantics: we
 * always close - keep-alive is not implemented). Runs detached. */
static void *nsnf_sender(void *argp) {
    NsnfSendCtx *x = (NsnfSendCtx *)argp;
    NsnfState *st = x->st;
    int idx = x->idx, gen = x->gen;
    nsnf_fd_t fd = NSNF_BAD_FD;
    long long sent = 0;

    nsnf_mutex_lock(&st->lock);
    if (st->conn[idx].in_use && st->conn[idx].gen == gen) {
        fd = st->conn[idx].fd;
        st->conn[idx].state = NSNF_ST_REPLYING;
        st->conn[idx].reply_status = x->status;
    }
    nsnf_mutex_unlock(&st->lock);
    if (fd == NSNF_BAD_FD) goto out;

    if (nsnf_send_all(fd, x->hdr, x->hdr_len) != 0) goto dead;

    if (x->is_file) {
        FILE *f = fopen(x->file_path, "rb");
        char fb[8192];
        if (!f) goto dead;
        if (x->file_off > 0) nsnf_fseek64(f, x->file_off);
        while (sent < x->file_len) {
            int want = (x->file_len - sent > (long long)sizeof(fb))
                     ? (int)sizeof(fb) : (int)(x->file_len - sent);
            int nr = (int)fread(fb, 1, (size_t)want, f);
            if (nr <= 0) break;
            if (nsnf_send_all(fd, fb, nr) != 0) { fclose(f); goto dead; }
            sent += nr;
            nsnf_mutex_lock(&st->lock);
            if (st->conn[idx].in_use) st->conn[idx].sent = sent;
            nsnf_mutex_unlock(&st->lock);
        }
        fclose(f);
    } else if (x->body && x->body_len > 0) {
        if (nsnf_send_all(fd, (const char *)x->body, x->body_len) != 0)
            goto dead;
        sent = x->body_len;
        nsnf_mutex_lock(&st->lock);
        if (st->conn[idx].in_use) st->conn[idx].sent = sent;
        nsnf_mutex_unlock(&st->lock);
    }

    nsnf_mutex_lock(&st->lock);
    if (st->conn[idx].in_use && st->conn[idx].gen == gen) {
        int sidx = st->conn[idx].srv_idx;
        st->conn[idx].state = NSNF_ST_DONE;
        if (sidx >= 0 && st->srv[sidx].in_use)
            st->srv[sidx].bytes_sent += (long long)x->hdr_len + sent;
    }
    nsnf_mutex_unlock(&st->lock);
    goto out;

dead:
    nsnf_mutex_lock(&st->lock);
    if (st->conn[idx].in_use && st->conn[idx].gen == gen) {
        int sidx = st->conn[idx].srv_idx;
        if (sidx >= 0 && st->srv[sidx].in_use) st->srv[sidx].reqs_failed++;
    }
    nsnf_mutex_unlock(&st->lock);

out:
    nsnf_mutex_lock(&st->lock);
    if (st->conn[idx].in_use && st->conn[idx].gen == gen)
        nsnf_close_conn(&st->conn[idx]);
    nsnf_mutex_unlock(&st->lock);
    free(x->hdr);
    free(x->body);
    free(x);
    return NULL;
}

/* ---- the builtins ------------------------------------------------------- */

/* @serve(cmd, h, p, n) - multiplexed control plane. See tigryx.h. */
static void nsnf_sys_serve(RelicVM *vm) {
    NsnfState *st;
    int cmd, h, i;
    u32 p_ptr;
    i32 n;

    st = nsnf_state_for(vm);
    if (!st) { vm->r[1] = -1; return; }

    cmd   = vm->r[1];
    h     = vm->r[2];
    p_ptr = (u32)vm->r[3];
    n     = vm->r[4];

    switch (cmd) {

    case 0: { /* SRV_OPEN: r2=port, r3=bind *u8 or 0, r4=flags -> handle */
        int slot = -1;
        nsnf_fd_t lfd;
        struct sockaddr_in sa;

#ifdef _WIN32
        if (!nsnf_wsa_up()) { vm->r[1] = -3; return; }
#endif

        nsnf_mutex_lock(&st->lock);
        for (i = 0; i < NSNF_MAX_SRV; i++)
            if (!st->srv[i].in_use) { slot = i; break; }
        nsnf_mutex_unlock(&st->lock);
        if (slot < 0) { vm->r[1] = -1; return; }

        lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd == NSNF_BAD_FD) { vm->r[1] = -3; return; }

        {
            int one = 1;
            setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR,
                       (const char *)&one, sizeof(one));
        }
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons((unsigned short)h);
        if (p_ptr && p_ptr < vm->mem_sz) {
            char bind_addr[64];
            nsnf_vm_str(vm, p_ptr, bind_addr, sizeof(bind_addr));
            if (bind_addr[0] &&
                nsnf_inet_parse(bind_addr, &sa.sin_addr) != 0) {
                nsnf_fd_close(lfd); vm->r[1] = -3; return;
            }
            if (!bind_addr[0]) sa.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            sa.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            nsnf_fd_close(lfd); vm->r[1] = -2; return;
        }
        if (listen(lfd, 8) != 0) {
            nsnf_fd_close(lfd); vm->r[1] = -2; return;
        }
        nsnf_fd_nonblock(lfd);
        if (h == 0) {
            struct sockaddr_in ba; nsnf_socklen_t bl = sizeof(ba);
            if (getsockname(lfd, (struct sockaddr *)&ba, &bl) == 0)
                h = ntohs(ba.sin_port);
        }

        nsnf_mutex_lock(&st->lock);
        memset(&st->srv[slot], 0, sizeof(NsnfSrv));
        st->srv[slot].in_use = 1;
        st->srv[slot].lfd    = lfd;
        st->srv[slot].port   = h;
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = slot;
        return;
    }

    case 1: { /* SRV_CLOSE: r2=srv */
        if (h < 0 || h >= NSNF_MAX_SRV) { vm->r[1] = -1; return; }
        nsnf_mutex_lock(&st->lock);
        if (st->srv[h].in_use) {
            nsnf_fd_close(st->srv[h].lfd);
            st->srv[h].in_use = 0;
            for (i = 0; i < NSNF_MAX_CONN; i++)
                if (st->conn[i].in_use && st->conn[i].srv_idx == h)
                    nsnf_close_conn(&st->conn[i]);
        }
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = 0;
        return;
    }

    case 2: { /* SRV_POLL: r2=srv, r3=*ServeRequest, r4=sizeof -> req/-1 */
        NsnfSrv *sv;
        int req_idx = -1;
        u32 cap_end;

        if (h < 0 || h >= NSNF_MAX_SRV || n < 1) { vm->r[1] = -1; return; }
        if (!p_ptr || p_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
        cap_end = p_ptr + (u32)n;
        if (cap_end > vm->mem_sz) cap_end = vm->mem_sz;

        nsnf_mutex_lock(&st->lock);
        sv = (st->srv[h].in_use) ? &st->srv[h] : NULL;
        if (!sv) { nsnf_mutex_unlock(&st->lock); vm->r[1] = -1; return; }

        /* accept pending connections, spawn a reader per conn */
        for (;;) {
            struct sockaddr_in ca; nsnf_socklen_t cl = sizeof(ca);
            nsnf_fd_t cfd = accept(sv->lfd, (struct sockaddr *)&ca, &cl);
            int cidx = -1;
            NsnfWorkerCtx *w;
            if (cfd == NSNF_BAD_FD) break;
            nsnf_no_sigpipe(cfd);
            nsnf_fd_block(cfd);   /* BSD inherits listener O_NONBLOCK */
            for (i = 0; i < NSNF_MAX_CONN; i++)
                if (!st->conn[i].in_use) { cidx = i; break; }
            if (cidx < 0) {
                /* table full: accept-and-close keeps the backlog draining */
                nsnf_fd_close(cfd);
                sv->reqs_failed++;
                continue;
            }
            memset(&st->conn[cidx], 0, sizeof(NsnfConn));
            st->conn[cidx].in_use  = 1;
            st->conn[cidx].gen++;
            if (st->conn[cidx].gen > 0xFFFFFF) st->conn[cidx].gen = 1;
            st->conn[cidx].fd      = cfd;
            st->conn[cidx].srv_idx = h;
            st->conn[cidx].state   = NSNF_ST_READING;
            st->conn[cidx].total   = -1;
            nsnf_inet_str(&ca.sin_addr, st->conn[cidx].ip,
                          (int)sizeof(st->conn[cidx].ip));
            w = (NsnfWorkerCtx *)malloc(sizeof(NsnfWorkerCtx));
            if (!w) { nsnf_close_conn(&st->conn[cidx]); continue; }
            w->st = st; w->idx = cidx; w->gen = st->conn[cidx].gen;
            if (nsnf_spawn(nsnf_reader, w) != 0) {
                free(w);
                nsnf_close_conn(&st->conn[cidx]);
                continue;
            }
        }

        /* drain ready queue: hand one parsed request to the guest */
        if (sv->ready_n > 0) {
            req_idx = sv->ready[0];
            for (i = 1; i < sv->ready_n; i++) sv->ready[i-1] = sv->ready[i];
            sv->ready_n--;
        }
        if (req_idx >= 0) {
            NsnfConn *c = &st->conn[req_idx];
            c->state = NSNF_ST_HANDED;
            i32 req_handle = (c->gen << 8) | req_idx;
            nsnf_vm_put32(vm, p_ptr + 0,  (i32)n
                          , cap_end);
            nsnf_vm_put32(vm, p_ptr + 4,  req_handle, cap_end);
            nsnf_vm_put32(vm, p_ptr + 8,  (i32)c->method, cap_end);
            nsnf_vm_put32(vm, p_ptr + 12, (i32)c->body_len, cap_end);
            nsnf_vm_put32(vm, p_ptr + 16, (i32)c->range_off, cap_end);
            nsnf_vm_put32(vm, p_ptr + 20, (i32)c->range_len, cap_end);
            nsnf_vm_put32(vm, p_ptr + 24, (i32)c->keep_alive, cap_end);
            nsnf_vm_put32(vm, p_ptr + 28, 0, cap_end);
            nsnf_vm_str_out(vm, p_ptr + 32,  c->ip,    32,  cap_end);
            nsnf_vm_str_out(vm, p_ptr + 64,  c->token, 96,  cap_end);
            nsnf_vm_str_out(vm, p_ptr + 160, c->path,  256, cap_end);
            nsnf_vm_str_out(vm, p_ptr + 416, c->query, 96,  cap_end);
            nsnf_vm_str_out(vm, p_ptr + 512, c->ua,    64,  cap_end);
            nsnf_mutex_unlock(&st->lock);
            vm->r[1] = req_handle;
            return;
        }
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = -1;
        return;
    }

    case 3: { /* SRV_READ: r2=req handle, r3=buf, r4=cap -> bytes/0/-1 */
        int idx = h & 0xFF, gen = (h >> 8) & 0xFFFFFF;
        int r = 0;
        u32 want;
        if (!p_ptr || p_ptr >= vm->mem_sz || n <= 0) { vm->r[1] = -1; return; }
        if (idx >= NSNF_MAX_CONN) { vm->r[1] = -1; return; }
        nsnf_mutex_lock(&st->lock);
        if (!st->conn[idx].in_use || st->conn[idx].gen != gen ||
            st->conn[idx].state != NSNF_ST_HANDED) {
            nsnf_mutex_unlock(&st->lock);
            vm->r[1] = -1;
            return;
        }
        want = (u32)n;
        if (p_ptr + want > vm->mem_sz) want = vm->mem_sz - p_ptr;
        /* bytes that arrived with the headers come first */
        if (st->conn[idx].body_pre_pos < st->conn[idx].body_pre_len) {
            int avail = st->conn[idx].body_pre_len - st->conn[idx].body_pre_pos;
            if (avail > (int)want) avail = (int)want;
            memcpy(vm->mem + p_ptr,
                   st->conn[idx].body_pre + st->conn[idx].body_pre_pos,
                   (size_t)avail);
            st->conn[idx].body_pre_pos += avail;
            nsnf_mutex_unlock(&st->lock);
            vm->r[1] = avail;
            return;
        }
        {
            /* zero-timeout readability probe (non-blocking socket read;
             * MSG_DONTWAIT doesn't exist on macOS/BSD) */
            nsnf_fd_t cfd = st->conn[idx].fd;
            nsnf_mutex_unlock(&st->lock);
            if (nsnf_can_read(cfd) > 0)
                r = nsnf_sock_recv(cfd, vm->mem + p_ptr, (int)want);
            else
                r = 0;
        }
        vm->r[1] = r;
        return;
    }

    case 4: { /* SRV_DROP: r2=req handle */
        int idx = h & 0xFF, gen = (h >> 8) & 0xFFFFFF;
        if (idx >= NSNF_MAX_CONN) { vm->r[1] = -1; return; }
        nsnf_mutex_lock(&st->lock);
        if (st->conn[idx].in_use && st->conn[idx].gen == gen &&
            st->conn[idx].state < NSNF_ST_REPLYING) {
            int sidx = st->conn[idx].srv_idx;
            if (sidx >= 0 && st->srv[sidx].in_use) st->srv[sidx].reqs_failed++;
            nsnf_close_conn(&st->conn[idx]);
            vm->r[1] = 0;
        } else {
            vm->r[1] = -1;
        }
        nsnf_mutex_unlock(&st->lock);
        return;
    }

    case 5: { /* SRV_STAT: r2=srv, r3=*ServeStat, r4=sizeof -> 0/-1 */
        NsnfSrv *sv;
        u32 cap_end;
        int open_conns = 0, xfer = 0;
        if (h < 0 || h >= NSNF_MAX_SRV || n < 1 ||
            !p_ptr || p_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
        cap_end = p_ptr + (u32)n;
        if (cap_end > vm->mem_sz) cap_end = vm->mem_sz;
        nsnf_mutex_lock(&st->lock);
        sv = st->srv[h].in_use ? &st->srv[h] : NULL;
        if (!sv) { nsnf_mutex_unlock(&st->lock); vm->r[1] = -1; return; }
        for (i = 0; i < NSNF_MAX_CONN; i++)
            if (st->conn[i].in_use && st->conn[i].srv_idx == h) {
                open_conns++;
                if (st->conn[i].state == NSNF_ST_REPLYING) xfer++;
            }
        nsnf_vm_put32(vm, p_ptr + 0,  (i32)n, cap_end);
        nsnf_vm_put32(vm, p_ptr + 4,  (i32)sv->port, cap_end);
        nsnf_vm_put32(vm, p_ptr + 8,  open_conns, cap_end);
        nsnf_vm_put32(vm, p_ptr + 12, (i32)sv->reqs_total, cap_end);
        nsnf_vm_put32(vm, p_ptr + 16, (i32)sv->reqs_failed, cap_end);
        nsnf_vm_put32(vm, p_ptr + 20, (i32)(sv->bytes_sent & 0xFFFFFFFFLL),
                      cap_end);
        nsnf_vm_put32(vm, p_ptr + 24, (i32)(sv->bytes_sent >> 32), cap_end);
        nsnf_vm_put32(vm, p_ptr + 28, xfer, cap_end);
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = 0;
        return;
    }

    case 6: { /* SRV_XFER: r2=srv, r3=*ServeXfer[], r4=sizeof buf -> count */
        int wrote = 0;
        u32 cap_end;
        if (h < 0 || h >= NSNF_MAX_SRV || n < NSNF_XFER_SIZE ||
            !p_ptr || p_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
        cap_end = p_ptr + (u32)n;
        if (cap_end > vm->mem_sz) cap_end = vm->mem_sz;
        nsnf_mutex_lock(&st->lock);
        if (!st->srv[h].in_use) {
            nsnf_mutex_unlock(&st->lock);
            vm->r[1] = -1;
            return;
        }
        for (i = 0; i < NSNF_MAX_CONN; i++) {
            NsnfConn *c = &st->conn[i];
            u32 off;
            if (!c->in_use || c->srv_idx != h ||
                (c->state != NSNF_ST_REPLYING && c->state != NSNF_ST_DONE))
                continue;
            off = p_ptr + (u32)(wrote * NSNF_XFER_SIZE);
            if (off + NSNF_XFER_SIZE > cap_end) break;
            nsnf_vm_put32(vm, off + 0,  (c->gen << 8) | i, cap_end);
            nsnf_vm_put32(vm, off + 4,
                          c->state == NSNF_ST_REPLYING ? 1 : 2, cap_end);
            nsnf_vm_put32(vm, off + 8,  (i32)(c->sent  & 0xFFFFFFFFLL), cap_end);
            nsnf_vm_put32(vm, off + 12, (i32)(c->sent  >> 32), cap_end);
            nsnf_vm_put32(vm, off + 16, (i32)(c->total & 0xFFFFFFFFLL), cap_end);
            nsnf_vm_put32(vm, off + 20, (i32)(c->total >> 32), cap_end);
            nsnf_vm_put32(vm, off + 24, (i32)c->reply_status, cap_end);
            nsnf_vm_str_out(vm, off + 28, c->ip,   24,  cap_end);
            nsnf_vm_str_out(vm, off + 52, c->path, 124, cap_end);
            wrote++;
        }
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = wrote;
        return;
    }

    default:
        vm->r[1] = -1;
        return;
    }
}

/* @serve_reply(req, status, headers, body, body_len) -> i32
 * body_len >= 0: body is guest memory, copied here and sent on a thread.
 * body_len == -1: body is a guest string holding a HOST filesystem path;
 * the sender thread streams it, applying the parsed Range if any. */
static void nsnf_sys_serve_reply(RelicVM *vm) {
    NsnfState *st = nsnf_state_for(vm);
    int req   = vm->r[1];
    int rcode = vm->r[2];
    u32 hdr   = (u32)vm->r[3];
    u32 body  = (u32)vm->r[4];
    i32 blen  = vm->r[5];
    int idx   = req & 0xFF, gen = (req >> 8) & 0xFFFFFF;
    NsnfSendCtx *x;

    if (!st || idx >= NSNF_MAX_CONN) { vm->r[1] = -1; return; }

    nsnf_mutex_lock(&st->lock);
    if (!st->conn[idx].in_use || st->conn[idx].gen != gen ||
        st->conn[idx].state != NSNF_ST_HANDED) {
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = -1;
        return;
    }

    x = (NsnfSendCtx *)calloc(1, sizeof(NsnfSendCtx));
    if (!x) {
        nsnf_close_conn(&st->conn[idx]);
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = -1;
        return;
    }
    x->st = st; x->idx = idx; x->gen = gen; x->status = rcode;

    if (blen == -1) { /* path-streamed reply */
        x->is_file = 1;
        nsnf_vm_str(vm, body, x->file_path, sizeof(x->file_path));
        /* file size for Content-Length + apply Range */
        {
            long long flen, off = st->conn[idx].range_off;
            if (rcode == 0) rcode = 200;
            flen = nsnf_filesize(x->file_path);
            if (st->conn[idx].range_len >= 0 || off > 0) {
                long long want = st->conn[idx].range_len;
                if (want < 0) want = flen - off;
                if (off > flen) off = flen;
                if (want > flen - off) want = flen - off;
                if (want < 0) want = 0;
                x->file_off = off;
                x->file_len = want;
                rcode = 206;
            } else {
                x->file_off = 0;
                x->file_len = flen;
            }
            st->conn[idx].total = x->file_len;
        }
    } else {
        if (blen > 0 && body && body + (u32)blen <= vm->mem_sz) {
            x->body = (unsigned char *)malloc((size_t)blen);
            if (x->body) {
                memcpy(x->body, vm->mem + body, (size_t)blen);
                x->body_len = blen;
            }
        }
        if (rcode == 0) rcode = 200;
        st->conn[idx].total = blen;
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
            en = nsnf_vm_str(vm, hdr, extra, sizeof(extra) - 2);
        nsnf_fmt_u64(clen, sizeof(clen), x->is_file ? x->file_len : 0);
        if (x->is_file)
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
        x->hdr = (char *)malloc((size_t)hn + 1);
        if (x->hdr) { memcpy(x->hdr, hbuf, (size_t)hn); x->hdr_len = hn; }
    }

    if (!x->hdr) {
        free(x->body); free(x);
        nsnf_close_conn(&st->conn[idx]);
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = -1;
        return;
    }

    if (nsnf_spawn(nsnf_sender, x) != 0) {
        free(x->hdr); free(x->body); free(x);
        nsnf_close_conn(&st->conn[idx]);
        nsnf_mutex_unlock(&st->lock);
        vm->r[1] = -1;
        return;
    }
    nsnf_mutex_unlock(&st->lock);
    vm->r[1] = 0;
    return;
}

#endif /* NSNF_SERVER_H */
