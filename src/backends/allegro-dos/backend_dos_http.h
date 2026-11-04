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
 *
 * See COPYING and EXCEPTION.md for the linking exception that applies
 * to programs built with Tigryx.
 */

/*
 * backend_dos_http.h - Shared TCP/IP HTTP implementation for DOS backends.
 *
 * When HAVE_DOSNET is defined at compile time, the DOS backends get real
 * HTTP support via the in-tree lwIP port (src/dosnet: lwIP over a DOS
 * packet driver, exposed through the small tdn_* socket API in
 * tdosnet.h). When HAVE_DOSNET is NOT defined, the stub functions in the
 * backend .c files are used instead (all return -1 / "not supported").
 *
 * Design: DOS is single-threaded, so HTTP uses a non-blocking state machine
 * per request slot. Each @http_poll call advances the state machine by:
 *   1. Calling tdn_poll() to drain the packet driver and run timers
 *   2. Checking socket readiness via tdn_select()
 *   3. Performing the next non-blocking I/O step
 *
 * States: RESOLVING -> CONNECTING -> SENDING -> RECEIVING_HEADERS
 *          -> RECEIVING_BODY -> DONE | ERROR
 *
 * URL parsing is minimal: http://host[:port]/path (no HTTPS/TLS).
 * Response parsing splits headers from body at the first \r\n\r\n.
 */

#ifndef BACKEND_DOS_HTTP_H
#define BACKEND_DOS_HTTP_H

#include "tigryx.h"

#ifdef HAVE_DOSNET

#include "tdosnet.h"

#define DOS_HTTP_MAX 16

typedef enum {
    HTTP_ST_IDLE = 0,
    HTTP_ST_RESOLVING,
    HTTP_ST_CONNECTING,
    HTTP_ST_SENDING,
    HTTP_ST_RECV_HEADERS,
    HTTP_ST_RECV_BODY,
    HTTP_ST_DONE,
    HTTP_ST_ERROR
} DosHttpState;

typedef struct {
    int       in_use;
    DosHttpState state;
    int       sock;           /* tdosnet fd, -1 when closed */
    int       status;         /* HTTP status code */
    unsigned char *body;      /* malloc'd response body */
    long      body_len;
    long      read_pos;       /* @http_read cursor */

    /* Internal buffers */
    char      send_buf[4096]; /* request text being sent */
    int       send_len;
    int       send_pos;

    char      recv_buf[8192]; /* raw receive buffer */
    int       recv_pos;       /* bytes in recv_buf */
    int       header_end;     /* offset of body start in recv_buf (-1 = not found) */
} DosHttpReq;

/* Global TCP/IP init flag */
static int g_dosnet_inited = 0;

/* Per-VM HTTP slot array stored in AllegroState.
 * The backend .c file must include this header AFTER defining AllegroState
 * with a `DosHttpReq http[DOS_HTTP_MAX];` field, and must #define
 * DOS_HTTP_STATE(s) to access it. */

static const char *dos_http_method_str(int method) {
    switch (method) {
        case 0:  return "GET";
        case 1:  return "POST";
        case 2:  return "PUT";
        case 3:  return "DELETE";
        default: return "GET";
    }
}

/* Parse "http://host[:port]/path" into host, port, path.
 * Returns 0 on success, -1 on error. */
static int dos_http_parse_url(const char *url, char *host, int host_sz,
                              int *port, char *path, int path_sz) {
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return -1;
    p += 7;

    *port = 80;
    path[0] = '/';
    path[1] = '\0';

    /* Extract host[:port] */
    int hi = 0;
    while (*p && *p != '/' && hi < host_sz - 1) {
        if (*p == ':') {
            host[hi] = '\0';
            p++;
            *port = 0;
            while (*p >= '0' && *p <= '9') {
                *port = *port * 10 + (*p - '0');
                p++;
            }
            break;
        }
        host[hi++] = *p++;
    }
    host[hi] = '\0';

    /* Skip to path */
    while (*p && *p != '/') p++;
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < path_sz - 1) {
            path[pi++] = *p++;
        }
        path[pi] = '\0';
    }

    if (hi == 0) return -1;
    return 0;
}

/* Initialize the TCP/IP stack if not already done. Returns 1 on success. */
static int dos_http_ensure_init(void) {
    if (!g_dosnet_inited) {
        if (tdn_init() < 0) return 0;
        g_dosnet_inited = 1;
    }
    /* Drive the TCP/IP stack every time we're called */
    tdn_poll();
    return 1;
}

/* @http_request(method, url, headers, body, body_len) -> i32 handle */
static void dos_http_request(RelicVM *vm, DosHttpReq *http_slots) {
    if (!dos_http_ensure_init()) { vm->r[1] = -1; return; }

    int method   = vm->r[1];
    u32 url_ptr  = (u32)vm->r[2];
    u32 hdr_ptr  = (u32)vm->r[3];
    u32 body_ptr = (u32)vm->r[4];
    i32 body_len = vm->r[5];

    if (url_ptr == 0 || url_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }

    /* Copy URL from VM memory */
    char url[2048];
    int ul = 0;
    while (ul < (int)sizeof(url) - 1 && url_ptr + (u32)ul < vm->mem_sz
           && vm->mem[url_ptr + ul] != 0) {
        url[ul] = (char)vm->mem[url_ptr + ul]; ul++;
    }
    url[ul] = '\0';
    if (ul == 0) { vm->r[1] = -1; return; }

    /* Parse the original URL to extract host (for Host: header) and path.
     * When a proxy is configured, we connect to the proxy instead and send
     * the full URL in the request line (standard HTTP proxy behaviour).
     * This lets DOS reach HTTPS servers via a TLS-terminating proxy. */
    char host[256], path[2048];
    int port;
    int is_https = (strncmp(url, "https://", 8) == 0);

    /* If https:// and proxy is set, rewrite to http:// for parsing */
    char parse_url[2048];
    if (is_https && vm->http_proxy) {
        /* Replace "https://" with "http://" so dos_http_parse_url can handle it */
        snprintf(parse_url, sizeof(parse_url), "http://%s", url + 8);
    } else {
        snprintf(parse_url, sizeof(parse_url), "%s", url);
    }

    if (dos_http_parse_url(parse_url, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        vm->r[1] = -1; return;
    }

    /* Determine connect target: proxy or direct */
    char connect_host[256];
    int  connect_port;

    if (vm->http_proxy) {
        /* Parse proxy URL "http://host[:port]" to get proxy host/port */
        char proxy_host[256], proxy_path[2048];
        int  proxy_port;
        if (dos_http_parse_url(vm->http_proxy, proxy_host, sizeof(proxy_host),
                               &proxy_port, proxy_path, sizeof(proxy_path)) < 0) {
            vm->r[1] = -1; return;
        }
        snprintf(connect_host, sizeof(connect_host), "%s", proxy_host);
        connect_port = proxy_port;
    } else {
        /* Direct connection - but https:// without proxy can't work on DOS */
        if (is_https) { vm->r[1] = -1; return; }
        snprintf(connect_host, sizeof(connect_host), "%s", host);
        connect_port = port;
    }

    /* Allocate a slot */
    int slot = -1;
    for (int i = 0; i < DOS_HTTP_MAX; i++) {
        if (!http_slots[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { vm->r[1] = -1; return; }

    DosHttpReq *r = &http_slots[slot];
    memset(r, 0, sizeof(*r));
    r->in_use = 1;
    r->sock = -1;
    r->state = HTTP_ST_RESOLVING;
    r->header_end = -1;

    /* Build the HTTP request text.
     * When using a proxy, send a path-based request line ("/host/path")
     * which our TLS-terminating proxy (tools/proxy.py) interprets as:
     *   first path segment = upstream HTTPS host, rest = path.
     * For direct connections, send just the path (origin-form). */
    const char *m = dos_http_method_str(method);
    int sl = 0;
    if (vm->http_proxy) {
        /* Build "/host/path" from the parsed host and path.
         * This works for both http:// and https:// original URLs since
         * we already parsed them into host + path above. */
        char proxy_path[2304];
        snprintf(proxy_path, sizeof(proxy_path), "/%s%s", host, path);
        sl += snprintf(r->send_buf + sl, sizeof(r->send_buf) - sl,
                       "%s %s HTTP/1.0\r\nHost: %s\r\n", m, proxy_path, host);
    } else {
        sl += snprintf(r->send_buf + sl, sizeof(r->send_buf) - sl,
                       "%s %s HTTP/1.0\r\nHost: %s\r\n", m, path, host);
    }
    sl += snprintf(r->send_buf + sl, sizeof(r->send_buf) - sl,
                   "Connection: close\r\n");
    sl += snprintf(r->send_buf + sl, sizeof(r->send_buf) - sl,
                   "User-Agent: Relic/1.0\r\n");

    /* Copy headers from VM memory (NUL-terminated block of "Key: Value\r\n..." */
    if (hdr_ptr != 0 && hdr_ptr < vm->mem_sz) {
        int hl = 0;
        while (hdr_ptr + (u32)hl < vm->mem_sz && vm->mem[hdr_ptr + hl] != 0) hl++;
        if (hl > 0 && sl + hl < (int)sizeof(r->send_buf) - 4) {
            memcpy(r->send_buf + sl, vm->mem + hdr_ptr, hl);
            sl += hl;
            if (hl > 0 && r->send_buf[sl-1] != '\n') {
                r->send_buf[sl++] = '\r'; r->send_buf[sl++] = '\n';
            }
        }
    }

    /* Content-Length for body */
    if (body_ptr != 0 && body_len > 0 && body_ptr + (u32)body_len <= vm->mem_sz) {
        sl += snprintf(r->send_buf + sl, sizeof(r->send_buf) - sl,
                       "Content-Length: %d\r\n", body_len);
    }

    r->send_buf[sl++] = '\r'; r->send_buf[sl++] = '\n';
    r->send_len = sl;
    r->send_pos = 0;

    /* Copy body from VM memory (stored after headers in send_buf if room,
     * otherwise we send it separately after the request headers) */
    /* For simplicity, we append the body to send_buf if it fits */
    if (body_ptr != 0 && body_len > 0 && body_ptr + (u32)body_len <= vm->mem_sz) {
        if (sl + body_len <= (int)sizeof(r->send_buf)) {
            memcpy(r->send_buf + sl, vm->mem + body_ptr, body_len);
            r->send_len = sl + body_len;
        }
        /* If body doesn't fit, we just truncate it (DOS memory constraints) */
    }

    /* Resolve hostname - pumps internally, typically fast with DNS */
    uint32_t ip = 0;
    if (tdn_resolve(connect_host, &ip) < 0) {
        r->state = HTTP_ST_ERROR;
        vm->r[1] = slot;
        return;
    }

    /* Create socket (always non-blocking) */
    int fd = tdn_socket();
    if (fd < 0) {
        r->state = HTTP_ST_ERROR;
        vm->r[1] = slot;
        return;
    }
    r->sock = fd;

    /* Initiate non-blocking connect */
    int rc = tdn_connect(fd, ip, (uint16_t)connect_port);
    if (rc < 0) {
        r->state = HTTP_ST_ERROR;
    } else {
        r->state = HTTP_ST_CONNECTING;
    }

    vm->r[1] = slot;
}

/* Advance the state machine for a single request slot.
 * Called from dos_http_poll. */
static void dos_http_advance(DosHttpReq *r) {
    if (!r->in_use) return;

    switch (r->state) {
    case HTTP_ST_IDLE:
    case HTTP_ST_DONE:
    case HTTP_ST_ERROR:
        return;

    case HTTP_ST_RESOLVING:
        /* Should not happen - resolution is done in request() */
        r->state = HTTP_ST_ERROR;
        break;

    case HTTP_ST_CONNECTING: {
        /* Check if socket is writable (connected) */
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(r->sock, &wset);
        int rc = tdn_select(r->sock + 1, NULL, &wset);
        if (rc > 0) {
            r->state = HTTP_ST_SENDING;
        } else if (rc < 0) {
            r->state = HTTP_ST_ERROR;
        }
        break;
    }

    case HTTP_ST_SENDING: {
        /* Send remaining request data */
        int remaining = r->send_len - r->send_pos;
        if (remaining <= 0) {
            r->state = HTTP_ST_RECV_HEADERS;
            break;
        }
        int n = tdn_send(r->sock, r->send_buf + r->send_pos, remaining);
        if (n > 0) {
            r->send_pos += n;
            if (r->send_pos >= r->send_len) {
                r->state = HTTP_ST_RECV_HEADERS;
            }
        } else if (n < 0) {
            /* Would block - just wait for next poll */
            /* Check for real error via select */
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(r->sock, &wset);
            int rc = tdn_select(r->sock + 1, NULL, &wset);
            if (rc < 0) r->state = HTTP_ST_ERROR;
        }
        break;
    }

    case HTTP_ST_RECV_HEADERS: {
        /* Check for readable data */
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(r->sock, &rset);
        int rc = tdn_select(r->sock + 1, &rset, NULL);
        if (rc <= 0) {
            if (rc < 0) r->state = HTTP_ST_ERROR;
            break;
        }

        int space = (int)sizeof(r->recv_buf) - r->recv_pos;
        if (space <= 0) {
            /* Headers too large for buffer - error */
            r->state = HTTP_ST_ERROR;
            break;
        }
        int n = tdn_recv(r->sock, r->recv_buf + r->recv_pos, space);
        if (n < 0) break;  /* would block */
        if (n == 0) {
            /* Connection closed - parse what we have */
            r->state = HTTP_ST_RECV_BODY;
            break;
        }
        r->recv_pos += n;

        /* Look for \r\n\r\n header terminator */
        for (int i = 0; i <= r->recv_pos - 4; i++) {
            if (r->recv_buf[i] == '\r' && r->recv_buf[i+1] == '\n' &&
                r->recv_buf[i+2] == '\r' && r->recv_buf[i+3] == '\n') {
                r->header_end = i + 4;
                /* Parse status code from first line: "HTTP/1.x NNN ..." */
                if (r->recv_pos >= 12 &&
                    r->recv_buf[0] == 'H' && r->recv_buf[1] == 'T' &&
                    r->recv_buf[2] == 'T' && r->recv_buf[3] == 'P') {
                    r->status = atoi(r->recv_buf + 9);
                }
                /* Move any body data to the body buffer */
                int body_start = r->recv_pos - r->header_end;
                if (body_start > 0) {
                    r->body = malloc(body_start);
                    if (r->body) {
                        memcpy(r->body, r->recv_buf + r->header_end, body_start);
                        r->body_len = body_start;
                    }
                }
                r->state = HTTP_ST_RECV_BODY;
                break;
            }
        }
        break;
    }

    case HTTP_ST_RECV_BODY: {
        /* Check for readable data */
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(r->sock, &rset);
        int rc = tdn_select(r->sock + 1, &rset, NULL);
        if (rc <= 0) {
            if (rc < 0) r->state = HTTP_ST_ERROR;
            break;
        }

        char tmp[2048];
        int n = tdn_recv(r->sock, tmp, sizeof(tmp));
        if (n < 0) break;  /* would block */
        if (n == 0) {
            /* Connection closed - done */
            tdn_close(r->sock);
            r->sock = -1;
            r->state = HTTP_ST_DONE;
            break;
        }
        /* Append to body buffer */
        unsigned char *nb = realloc(r->body, r->body_len + n);
        if (!nb) {
            r->state = HTTP_ST_ERROR;
            break;
        }
        r->body = nb;
        memcpy(r->body + r->body_len, tmp, n);
        r->body_len += n;
        break;
    }
    }
}

/* @http_poll(handle) -> i32 (0=pending, 1=ready, <0=error) */
static void dos_http_poll(RelicVM *vm, DosHttpReq *http_slots) {
    dos_http_ensure_init();  /* pump the stack */

    int handle = vm->r[1];
    if (handle < 0 || handle >= DOS_HTTP_MAX) { vm->r[1] = -1; return; }
    DosHttpReq *r = &http_slots[handle];
    if (!r->in_use) { vm->r[1] = -1; return; }

    dos_http_advance(r);

    switch (r->state) {
    case HTTP_ST_DONE:    vm->r[1] = 1;  break;
    case HTTP_ST_ERROR:   vm->r[1] = -1; break;
    default:              vm->r[1] = 0;  break;  /* still pending */
    }
}

/* @http_status(handle) -> i32 HTTP status code */
static void dos_http_status(RelicVM *vm, DosHttpReq *http_slots) {
    int handle = vm->r[1];
    if (handle < 0 || handle >= DOS_HTTP_MAX) { vm->r[1] = -1; return; }
    DosHttpReq *r = &http_slots[handle];
    if (!r->in_use) { vm->r[1] = -1; return; }
    vm->r[1] = r->status;
}

/* @http_read(handle, buf, cap) -> i32 bytes copied (0=EOF) */
static void dos_http_read(RelicVM *vm, DosHttpReq *http_slots) {
    int handle  = vm->r[1];
    u32 buf_ptr = (u32)vm->r[2];
    i32 cap     = vm->r[3];
    if (handle < 0 || handle >= DOS_HTTP_MAX || cap <= 0
        || buf_ptr == 0 || buf_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
    DosHttpReq *r = &http_slots[handle];
    if (!r->in_use) { vm->r[1] = -1; return; }

    long avail = r->body_len - r->read_pos;
    if (avail <= 0) { vm->r[1] = 0; return; }
    long to_copy = avail < cap ? avail : cap;
    if (buf_ptr + (u32)to_copy > vm->mem_sz) to_copy = vm->mem_sz - buf_ptr;
    memcpy(vm->mem + buf_ptr, r->body + r->read_pos, (size_t)to_copy);
    r->read_pos += to_copy;
    vm->r[1] = (i32)to_copy;
}

/* @http_close(handle) */
static void dos_http_close(RelicVM *vm, DosHttpReq *http_slots) {
    int handle = vm->r[1];
    if (handle < 0 || handle >= DOS_HTTP_MAX) return;
    DosHttpReq *r = &http_slots[handle];
    if (!r->in_use) return;

    if (r->sock >= 0) {
        tdn_close(r->sock);
        r->sock = -1;
    }
    if (r->body) {
        free(r->body);
        r->body = NULL;
    }
    r->in_use = 0;
    r->state = HTTP_ST_IDLE;
    r->body_len = 0;
    r->read_pos = 0;
}

/* Call from backend shutdown to clean up all in-flight requests */
static void dos_http_shutdown(DosHttpReq *http_slots) {
    for (int i = 0; i < DOS_HTTP_MAX; i++) {
        DosHttpReq *r = &http_slots[i];
        if (r->in_use) {
            if (r->sock >= 0) tdn_close(r->sock);
            if (r->body) free(r->body);
            r->in_use = 0;
            r->sock = -1;
            r->body = NULL;
            r->body_len = 0;
            r->read_pos = 0;
            r->state = HTTP_ST_IDLE;
        }
    }
    if (g_dosnet_inited) {
        tdn_exit();
        g_dosnet_inited = 0;
    }
}

#endif /* HAVE_DOSNET */
#endif /* BACKEND_DOS_HTTP_H */
