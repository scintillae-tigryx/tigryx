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
 * tdosnet.c - minimal TCP socket API for DOS over lwIP + packet driver.
 * Implements the tdn_* API declared in tdosnet.h.
 *
 * All stack progress is cooperative: tdn_poll() drains the
 * packet-driver receive ring into lwIP and runs sys_check_timeouts().
 * Blocking-ish operations (DNS, DHCP wait) simply pump in a loop.
 */
#include <lwip/init.h>
#include <lwip/sys.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/dns.h>
#include <lwip/dhcp.h>
#include <lwip/etharp.h>
#include <lwip/timeouts.h>
#include <lwip/prot/ethernet.h>
#include <netif/ethernet.h>

#include "tdosnet.h"
#include "pktdrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pc.h>

#define TDNS_MAX_SOCK   24
#define TDNS_RX_FRAME   1600
#define TDNS_ACCEPT_Q   4       /* pending connections per listener */

/* ------------------------------------------------------------------ */
/* Socket table                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    struct tcp_pcb *pcb;
    struct pbuf    *rxq;        /* queue of received pbuf chains */
    unsigned        rx_off;     /* read offset into head pbuf */
    unsigned short  flags;
    uint32_t        peer_ip;    /* host order, set for accepted conns */
    /* listeners: pcbs accepted by lwIP, waiting for tdn_accept() */
    struct tcp_pcb *pend[TDNS_ACCEPT_Q];
    int             pend_n;
} tdns_sock;

#define SF_USED     0x01
#define SF_CONN     0x02        /* connect completed (ESTABLISHED+) */
#define SF_DEAD     0x04        /* aborted / connect failed */
#define SF_FIN      0x08        /* peer sent FIN, rxq may still drain */
#define SF_LISTEN   0x10        /* listen pcb: tdn_accept only */

static tdns_sock tdns_socks[TDNS_MAX_SOCK];
static int       tdns_inited;

/* ------------------------------------------------------------------ */
/* netif glue                                                          */
/* ------------------------------------------------------------------ */

static struct netif tdns_netif;
static uint8_t     tdns_rxbuf[TDNS_RX_FRAME];

static err_t tdns_linkoutput(struct netif *netif, struct pbuf *p)
{
    static uint8_t frame[TDNS_RX_FRAME];
    unsigned off = 0;
    struct pbuf *q;

    for (q = p; q && off < sizeof(frame); q = q->next) {
        unsigned n = q->len;
        if (off + n > sizeof(frame)) n = sizeof(frame) - off;
        memcpy(frame + off, q->payload, n);
        off += n;
    }
    if (pktdrv_send(frame, off) != 0)
        return ERR_IF;
    return ERR_OK;
}

static err_t tdns_netif_init(struct netif *netif)
{
    netif->name[0] = 't';
    netif->name[1] = 'd';
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    netif->output = etharp_output;
    netif->linkoutput = tdns_linkoutput;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    return ERR_OK;
}

/* Drain the packet-driver ring into lwIP. */
static void tdns_input(void)
{
    int n;
    while ((n = pktdrv_poll(tdns_rxbuf, sizeof(tdns_rxbuf))) > 0) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_RAM);
        if (!p)
            break;                /* out of memory - drop, TCP recovers */
        memcpy(p->payload, tdns_rxbuf, n);
        if (tdns_netif.input(p, &tdns_netif) != ERR_OK)
            pbuf_free(p);
    }
}

/* ------------------------------------------------------------------ */
/* Pump = input drain + lwIP timers                                    */
/* ------------------------------------------------------------------ */

static void tdns_pump(void)
{
    tdns_input();
    sys_check_timeouts();
}

void tdn_poll(void)
{
    if (tdns_inited)
        tdns_pump();
}

/* Pump until pred() returns true or ~ms elapse. Returns pred(). */
static int tdns_wait(int (*pred)(void *), void *arg, unsigned long ms)
{
    u32_t end = sys_now() + ms;
    while (!pred(arg)) {
        tdns_pump();
        if ((long)(sys_now() - end) > 0)
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* tdnet.cfg configuration                                             */
/* ------------------------------------------------------------------ */

static char cfg_ip[32], cfg_mask[32], cfg_gw[32], cfg_dns[2][32];
static int  cfg_have;           /* a usable static config was found */
static int  cfg_want_dhcp;      /* my_ip=dhcp|bootp|auto, or no config */

static int cfg_is_quad(const char *s)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (!(*s >= '0' && *s <= '9')) return 0;
        while (*s >= '0' && *s <= '9') s++;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    return *s == '\0';
}

static void cfg_parse_line(char *line)
{
    char *eq, *key, *val, *p;
    static int ndns;

    /* strip comment + whitespace */
    for (p = line; *p && (*p == ' ' || *p == '\t'); p++) ;
    if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') return;
    eq = strchr(p, '=');
    if (!eq) return;
    *eq = '\0';
    key = p;
    val = eq + 1;
    while (key > line && (eq[-1] == ' ' || eq[-1] == '\t')) *--eq = '\0';
    while (*val == ' ' || *val == '\t') val++;
    p = val + strlen(val);
    while (p > val && (p[-1] == ' ' || p[-1] == '\t' ||
                       p[-1] == '\r' || p[-1] == '\n')) *--p = '\0';

    if (!stricmp(key, "my_ip")) {
        if (!stricmp(val, "dhcp") || !stricmp(val, "bootp") ||
            !stricmp(val, "auto") || !strcmp(val, "0.0.0.0"))
            cfg_want_dhcp = 1;
        else if (cfg_is_quad(val)) {
            strncpy(cfg_ip, val, sizeof(cfg_ip) - 1);
            cfg_have = 1;
        }
    } else if (!stricmp(key, "netmask") || !stricmp(key, "subnet")) {
        if (cfg_is_quad(val)) strncpy(cfg_mask, val, sizeof(cfg_mask) - 1);
    } else if (!stricmp(key, "gateway")) {
        if (cfg_is_quad(val)) strncpy(cfg_gw, val, sizeof(cfg_gw) - 1);
    } else if (!stricmp(key, "nameserver")) {
        if (ndns < 2 && cfg_is_quad(val)) strncpy(cfg_dns[ndns++], val, 31);
    }
}

static void cfg_load(void)
{
    char line[256];
    FILE *f = NULL;
    const char *e;

    if ((e = getenv("TDNETCFG")) != NULL)
        f = fopen(e, "r");
    if (!f)
        f = fopen("tdnet.cfg", "r");
    if (!f) {
        cfg_want_dhcp = 1;        /* no config anywhere: try DHCP */
        return;
    }
    while (fgets(line, sizeof(line), f))
        cfg_parse_line(line);
    fclose(f);
    if (!cfg_have && !cfg_want_dhcp)
        cfg_want_dhcp = 1;        /* config existed but had no my_ip */
}

/* tiny dotted-quad parser. Returns the host-order address, or -1UL
 * when the whole string is not a quad. */
static unsigned long tdns_parse_quad(const char *s)
{
    unsigned long a = 0;
    int i, digits = 0;
    for (i = 0; i < 4; i++) {
        unsigned v = 0;
        if (!(*s >= '0' && *s <= '9')) return ~0UL;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            digits++;
        }
        if (v > 255) return ~0UL;
        a = (a << 8) | v;
        if (*s == '.') s++;
    }
    if (*s != '\0' || digits == 0) return ~0UL;
    return a;
}

/* ------------------------------------------------------------------ */
/* tdn_init / tdn_exit                                                 */
/* ------------------------------------------------------------------ */

static int tdns_dhcp_bound;

static void tdns_dhcp_poll(void)
{
    tdns_dhcp_bound = dhcp_supplied_address(&tdns_netif) ? 1 : 0;
}

static int pred_dhcp(void *arg)
{
    (void)arg;
    tdns_dhcp_poll();
    return tdns_dhcp_bound;
}

/* Startup diagnostics. Networking failures on DOS are almost always
 * configuration (no packet driver loaded, no DHCP server, wrong
 * tdnet.cfg) rather than code, and the @http_* API can only report
 * -1 - so say what actually went wrong on the way up. Set TDNETQUIET
 * to suppress. */
static int tdns_quiet;

static void tdns_say(const char *msg)
{
    if (!tdns_quiet)
        fprintf(stderr, "tdosnet: %s\n", msg);
}

int tdn_init(void)
{
    ip4_addr_t ip, mask, gw;
    uint8_t mac[6];
    int rc;

    if (tdns_inited)
        return 0;

    tdns_quiet = (getenv("TDNETQUIET") != NULL);

    rc = pktdrv_init(mac);
    if (rc != 0) {
        switch (rc) {
        case -1: tdns_say("no packet driver found on INT 60h-80h "
                          "(load one, e.g. NE2000 / PCNTPK)"); break;
        case -2: tdns_say("out of conventional memory for packet buffers"); break;
        case -3: tdns_say("DPMI real-mode callback allocation failed"); break;
        case -4: tdns_say("packet driver refused access_type "
                          "(another client already has it?)"); break;
        case -5: tdns_say("packet driver get_address failed"); break;
        default: tdns_say("packet driver init failed"); break;
        }
        pktdrv_shutdown();        /* release partial allocations */
        return -1;
    }

    lwip_init();
    cfg_load();

    memset(&tdns_netif, 0, sizeof(tdns_netif));
    if (!netif_add(&tdns_netif, NULL, NULL, NULL, NULL,
                   tdns_netif_init, ethernet_input)) {
        pktdrv_shutdown();
        return -2;
    }
    memcpy(tdns_netif.hwaddr, mac, ETH_HWADDR_LEN);
    netif_set_default(&tdns_netif);
    netif_set_up(&tdns_netif);

    if (cfg_have && !cfg_want_dhcp) {
        /* Static configuration. */
        ip.addr = htonl(tdns_parse_quad(cfg_ip));
        mask.addr = cfg_mask[0] ? htonl(tdns_parse_quad(cfg_mask))
                                : htonl(0xffffff00UL);
        gw.addr = htonl(tdns_parse_quad(cfg_gw));
        netif_set_addr(&tdns_netif, &ip, &mask, &gw);
    } else {
        /* DHCP: wait for a lease, fall back to static if one is
         * also present in the config, else fail. */
        dhcp_start(&tdns_netif);
        tdns_dhcp_bound = 0;
        if (!tdns_wait(pred_dhcp, NULL, 10000)) {
            if (cfg_have) {
                ip.addr = htonl(tdns_parse_quad(cfg_ip));
                mask.addr = cfg_mask[0] ? htonl(tdns_parse_quad(cfg_mask))
                                        : htonl(0xffffff00UL);
                gw.addr = htonl(tdns_parse_quad(cfg_gw));
                dhcp_stop(&tdns_netif);
                netif_set_addr(&tdns_netif, &ip, &mask, &gw);
            } else {
                tdns_say("DHCP got no lease in 10s and no my_ip in "
                         "tdnet.cfg - set my_ip/netmask/gateway/"
                         "nameserver there");
                dhcp_stop(&tdns_netif);
                netif_remove(&tdns_netif);
                pktdrv_shutdown();
                return -3;        /* no lease, no static config */
            }
        }
    }

    /* Static nameservers from config; DHCP-provided ones (if any)
     * are already installed by lwIP's dhcp.c. */
    for (int i = 0; i < 2; i++) {
        if (cfg_dns[i][0]) {
            ip_addr_t ns;
            ip_addr_set_ip4_u32(&ns, htonl(tdns_parse_quad(cfg_dns[i])));
            dns_setserver(i, &ns);
        }
    }

    if (!tdns_quiet) {
        const ip4_addr_t *a = netif_ip4_addr(&tdns_netif);
        const ip4_addr_t *g = netif_ip4_gw(&tdns_netif);
        const ip_addr_t  *d = dns_getserver(0);
        fprintf(stderr, "tdosnet: ip %s", ip4addr_ntoa(a));
        fprintf(stderr, " gw %s", ip4addr_ntoa(g));
        fprintf(stderr, " dns %s\n", ipaddr_ntoa(d));
    }

    tdns_inited = 1;
    return 0;
}

void tdn_exit(void)
{
    int i;
    for (i = 0; i < TDNS_MAX_SOCK; i++)
        if (tdns_socks[i].flags & SF_USED)
            tdn_close(i);
    if (tdns_inited) {
        dhcp_stop(&tdns_netif);
        netif_remove(&tdns_netif);   /* so a later tdn_init can re-add */
        pktdrv_shutdown();
        tdns_inited = 0;
    }
}

/* ------------------------------------------------------------------ */
/* TCP callbacks                                                       */
/* ------------------------------------------------------------------ */

static tdns_sock *tdns_sock_from_arg(void *arg)
{
    return (tdns_sock *)arg;
}

static err_t tdns_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    tdns_sock *s = tdns_sock_from_arg(arg);
    (void)pcb;
    if (err == ERR_OK)
        s->flags |= SF_CONN;
    else
        s->flags |= SF_DEAD;
    return ERR_OK;
}

static err_t tdns_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                       err_t err)
{
    tdns_sock *s = tdns_sock_from_arg(arg);
    (void)pcb; (void)err;

    if (p == NULL) {
        s->flags |= SF_FIN;       /* orderly close from peer */
        return ERR_OK;
    }
    if (s->rxq == NULL) {
        s->rxq = p;
    } else {
        pbuf_cat(s->rxq, p);      /* append chain; ref taken */
    }
    return ERR_OK;
}

static void tdns_err(void *arg, err_t err)
{
    tdns_sock *s = tdns_sock_from_arg(arg);
    (void)err;
    /* lwIP has already freed the pcb - do not touch it. */
    s->pcb = NULL;
    s->flags |= SF_DEAD;
}

static err_t tdns_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)pcb; (void)len;
    return ERR_OK;
}

/* lwIP accept callback on a LISTEN pcb. The new pcb is parked on the
 * listener's pend[] queue; tdn_accept() wraps it as a tdns_sock later.
 * Returning ERR_ABRT tells lwIP to abort the connection itself. */
static err_t tdns_on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    tdns_sock *s = tdns_sock_from_arg(arg);
    if (err != ERR_OK || !newpcb)
        return ERR_OK;
    if (s->pend_n >= TDNS_ACCEPT_Q)
        return ERR_ABRT;              /* backlog full: refuse */
    s->pend[s->pend_n++] = newpcb;
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Socket API                                                          */
/* ------------------------------------------------------------------ */

int tdn_socket(void)
{
    int i;
    tdns_sock *s;

    for (i = 0; i < TDNS_MAX_SOCK; i++)
        if (!(tdns_socks[i].flags & SF_USED))
            break;
    if (i == TDNS_MAX_SOCK)
        return -1;

    s = &tdns_socks[i];
    memset(s, 0, sizeof(*s));
    s->pcb = tcp_new();
    if (!s->pcb)
        return -1;

    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, tdns_recv);
    tcp_sent(s->pcb, tdns_sent);
    tcp_err(s->pcb, tdns_err);

    s->flags = SF_USED;
    return i;
}

int tdn_connect(int fd, uint32_t ip_host, uint16_t port)
{
    ip_addr_t ip;
    tdns_sock *s;
    err_t err;

    if (fd < 0 || fd >= TDNS_MAX_SOCK)
        return -1;
    s = &tdns_socks[fd];
    if (!(s->flags & SF_USED) || !s->pcb)
        return -1;

    ip4_addr_set_u32(ip_2_ip4(&ip), htonl(ip_host));
    err = tcp_connect(s->pcb, &ip, port, tdns_connected);
    if (err != ERR_OK) {
        s->flags |= SF_DEAD;
        return -1;
    }
    return 0;                     /* attempt started; non-blocking */
}

int tdn_send(int fd, const void *buf, int len)
{
    tdns_sock *s;
    u16_t room, n;
    err_t err;

    if (fd < 0 || fd >= TDNS_MAX_SOCK)
        return -1;
    s = &tdns_socks[fd];
    if (!(s->flags & SF_USED) || !s->pcb ||
        (s->flags & (SF_DEAD | SF_LISTEN)))
        return -1;
    if (!(s->flags & SF_CONN))
        return -1;                /* would block until connected */

    room = tcp_sndbuf(s->pcb);
    if (room == 0)
        return -1;                /* would block */
    n = (len > room) ? room : (u16_t)len;

    err = tcp_write(s->pcb, buf, n, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM)
        return -1;                /* would block */
    if (err != ERR_OK) {
        s->flags |= SF_DEAD;
        return -1;
    }
    tcp_output(s->pcb);
    return n;
}

int tdn_recv(int fd, void *buf, int len)
{
    tdns_sock *s;
    unsigned got = 0;

    if (fd < 0 || fd >= TDNS_MAX_SOCK)
        return -1;
    s = &tdns_socks[fd];
    if (!(s->flags & SF_USED) || (s->flags & SF_LISTEN))
        return -1;

    while (got < (unsigned)len && s->rxq) {
        struct pbuf *p = s->rxq;
        unsigned avail = p->len - s->rx_off;
        unsigned n = avail;
        if (n > (unsigned)len - got) n = (unsigned)len - got;
        memcpy((uint8_t *)buf + got,
               (const uint8_t *)p->payload + s->rx_off, n);
        got += n;
        s->rx_off += n;
        if (s->rx_off >= p->len) {
            s->rxq = p->next;
            p->next = NULL;
            pbuf_free(p);
            s->rx_off = 0;
        }
    }
    /* advertise the consumed bytes so the window re-opens */
    if (got && s->pcb)
        tcp_recved(s->pcb, (u16_t)got);

    if (got)
        return (int)got;
    if (s->flags & (SF_FIN | SF_DEAD))
        return 0;                 /* orderly close / drained */
    return -1;                    /* would block */
}

int tdn_close(int fd)
{
    tdns_sock *s;

    if (fd < 0 || fd >= TDNS_MAX_SOCK)
        return -1;
    s = &tdns_socks[fd];
    if (!(s->flags & SF_USED))
        return -1;

    if (s->pcb) {
        if (s->flags & SF_LISTEN) {
            /* listen pcbs are struct tcp_pcb_listen (smaller) - only the
             * common fields (callback_arg via tcp_arg, accept) exist */
            tcp_arg(s->pcb, NULL);
            tcp_accept(s->pcb, NULL);
        } else {
            tcp_arg(s->pcb, NULL);
            tcp_recv(s->pcb, NULL);
            tcp_sent(s->pcb, NULL);
            tcp_err(s->pcb, NULL);
        }
        if (tcp_close(s->pcb) != ERR_OK)
            tcp_abort(s->pcb);
        s->pcb = NULL;
    }
    /* listeners: abort connections still waiting for tdn_accept() */
    while (s->pend_n > 0)
        tcp_abort(s->pend[--s->pend_n]);
    while (s->rxq) {
        struct pbuf *p = s->rxq;
        s->rxq = p->next;
        p->next = NULL;
        pbuf_free(p);
    }
    s->flags = 0;
    s->rx_off = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Server sockets: tdn_listen / tdn_accept / tdn_peerip                */
/* ------------------------------------------------------------------ */

int tdn_listen(uint16_t port)
{
    int i;
    tdns_sock *s;
    struct tcp_pcb *lpcb;

    for (i = 0; i < TDNS_MAX_SOCK; i++)
        if (!(tdns_socks[i].flags & SF_USED))
            break;
    if (i == TDNS_MAX_SOCK)
        return -1;

    s = &tdns_socks[i];
    memset(s, 0, sizeof(*s));
    s->pcb = tcp_new();
    if (!s->pcb)
        return -1;

    if (tcp_bind(s->pcb, IP_ANY_TYPE, port) != ERR_OK) {
        tcp_close(s->pcb);
        s->pcb = NULL;
        return -1;
    }
    /* tcp_listen() frees s->pcb and returns a smaller listen pcb.
     * On failure the original pcb is still valid - close it. */
    lpcb = tcp_listen(s->pcb);
    if (!lpcb) {
        tcp_close(s->pcb);
        s->pcb = NULL;
        return -1;
    }
    s->pcb = lpcb;
    tcp_arg(s->pcb, s);
    tcp_accept(s->pcb, tdns_on_accept);

    s->flags = SF_USED | SF_LISTEN;
    return i;
}

int tdn_accept(int lfd)
{
    tdns_sock *ls, *s;
    struct tcp_pcb *newpcb;
    int i;

    if (lfd < 0 || lfd >= TDNS_MAX_SOCK)
        return -1;
    ls = &tdns_socks[lfd];
    if (!(ls->flags & SF_LISTEN) || ls->pend_n <= 0)
        return -1;

    newpcb = ls->pend[0];
    for (i = 1; i < ls->pend_n; i++)
        ls->pend[i - 1] = ls->pend[i];
    ls->pend_n--;

    for (i = 0; i < TDNS_MAX_SOCK; i++)
        if (!(tdns_socks[i].flags & SF_USED))
            break;
    if (i == TDNS_MAX_SOCK) {
        tcp_abort(newpcb);
        return -1;
    }

    s = &tdns_socks[i];
    memset(s, 0, sizeof(*s));
    s->pcb     = newpcb;
    s->peer_ip = ntohl(ip_addr_get_ip4_u32(&newpcb->remote_ip));
    s->flags   = SF_USED | SF_CONN;    /* accepted pcbs are ESTABLISHED */

    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, tdns_recv);
    tcp_sent(s->pcb, tdns_sent);
    tcp_err(s->pcb, tdns_err);
    return i;
}

uint32_t tdn_peerip(int fd)
{
    tdns_sock *s;
    if (fd < 0 || fd >= TDNS_MAX_SOCK)
        return 0;
    s = &tdns_socks[fd];
    if (!(s->flags & SF_USED) || (s->flags & SF_LISTEN))
        return 0;
    return s->peer_ip;
}

uint16_t tdn_localport(int fd)
{
    tdns_sock *s;
    if (fd < 0 || fd >= TDNS_MAX_SOCK)
        return 0;
    s = &tdns_socks[fd];
    if (!(s->flags & SF_USED) || !s->pcb)
        return 0;
    /* local_port sits at the same offset in tcp_pcb_listen (the smaller
     * struct tcp_listen() returns) and tcp_pcb - both carry
     * TCP_PCB_COMMON - so this is safe for listeners and conns. */
    return s->pcb->local_port;
}

/* ------------------------------------------------------------------ */
/* tdn_select                                                          */
/* ------------------------------------------------------------------ */

int tdn_select(int nfds, fd_set *rset, fd_set *wset)
{
    int count = 0, i;

    tdns_pump();                  /* keep stack moving while apps poll */

    for (i = 0; i < nfds && i < TDNS_MAX_SOCK; i++) {
        tdns_sock *s = &tdns_socks[i];
        int r_in = rset && FD_ISSET(i, rset);
        int w_in = wset && FD_ISSET(i, wset);
        if (!r_in && !w_in)
            continue;

        if (s->flags & SF_DEAD) {
            if (rset) FD_CLR(i, rset);
            if (wset) FD_CLR(i, wset);
            return -1;            /* socket died */
        }

        if (r_in) {
            if ((s->flags & SF_LISTEN) ? (s->pend_n > 0)
                                      : (s->rxq || (s->flags & SF_FIN))) {
                /* readable: data/conn pending or peer closed (recv -> 0) */
                count++;
            } else {
                FD_CLR(i, rset);
            }
        }
        if (w_in) {
            if ((s->flags & SF_CONN) && s->pcb &&
                tcp_sndbuf(s->pcb) > 0) {
                count++;
            } else {
                FD_CLR(i, wset);
            }
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* tdn_resolve                                                         */
/* ------------------------------------------------------------------ */

static volatile int dns_done;
static ip4_addr_t dns_result;
static int        dns_ok;

static void tdns_dns_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name; (void)arg;
    if (addr && IP_IS_V4(addr)) {
        dns_result = *ip_2_ip4(addr);
        dns_ok = 1;
    }
    dns_done = 1;
}

static int pred_dns(void *arg)
{
    (void)arg;
    return dns_done;
}

int tdn_resolve(const char *name, uint32_t *ip_out)
{
    ip_addr_t addr;
    err_t err;

    if (!name || !*name || !ip_out)
        return -1;

    /* numeric dotted quad resolves without DNS traffic */
    {
        unsigned long a = tdns_parse_quad(name);
        if (a != ~0UL) {
            *ip_out = (uint32_t)a;
            return 0;
        }
    }

    dns_done = 0;
    dns_ok = 0;
    err = dns_gethostbyname(name, &addr, tdns_dns_cb, NULL);
    if (err == ERR_OK) {
        dns_result = *ip_2_ip4(&addr);
    } else if (err == ERR_INPROGRESS) {
        if (!tdns_wait(pred_dns, NULL, 15000))
            return -1;
        if (!dns_ok)
            return -1;
    } else {
        return -1;
    }

    /* lwIP stores network byte order; the API boundary is host order. */
    *ip_out = ntohl(dns_result.addr);
    return 0;
}
