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
 * tdosnet.h - minimal TCP socket API for DOS, backed by lwIP over a
 * packet driver.
 *
 * Cooperative, non-blocking API designed around the DOS HTTP backend's
 * needs: no table of socket options, no sockaddr plumbing - fds are
 * plain small integers and every socket is a non-blocking TCP client.
 *
 * Configuration: reads my_ip/netmask/gateway/nameserver keys
 * (my_ip=dhcp|bootp|auto selects DHCP) from a config file found at:
 *   1. $TDNETCFG            (full path to config file)
 *   2. ./tdnet.cfg
 * If no config is found, DHCP is attempted.
 */

#ifndef TDOSNET_H
#define TDOSNET_H

#include <sys/types.h>   /* fd_set, FD_* */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the packet driver, lwIP and IP configuration.
 * Returns 0 on success, <0 on failure. */
int  tdn_init(void);

/* Tear everything down. */
void tdn_exit(void);

/* Pump the stack: drain the packet-driver ring into lwIP and run
 * timers. Call frequently. */
void tdn_poll(void);

/* Allocate a non-blocking TCP socket. Returns fd >= 0, -1 on error. */
int  tdn_socket(void);

/* Start a non-blocking connect. ip is host byte order (as returned by
 * tdn_resolve), port host byte order. Returns 0 once the attempt is
 * under way, -1 on immediate failure. Completion is detected via
 * tdn_select write-ready. */
int  tdn_connect(int fd, uint32_t ip, uint16_t port);

/* Send/receive. tdn_send returns bytes queued, -1 would-block/failed.
 * tdn_recv returns bytes read, 0 on orderly close, -1 would-block. */
int  tdn_send(int fd, const void *buf, int len);
int  tdn_recv(int fd, void *buf, int len);

/* Close and free a socket. */
int  tdn_close(int fd);

/* Readiness poll over the read/write fd_sets. Returns ready-count,
 * 0 if none, <0 if a socket in the sets has died. */
int  tdn_select(int nfds, fd_set *rset, fd_set *wset);

/* Hostname lookup via lwIP DNS (pumps internally, ~15s timeout).
 * Numeric dotted-quad strings resolve without DNS. On success returns
 * 0 and stores the IPv4 address host byte order in *ip_out. */
int  tdn_resolve(const char *name, uint32_t *ip_out);

/* Server side: bind + listen on all interfaces. Returns a listener fd
 * >= 0, <0 on failure. Listen sockets only work with tdn_accept /
 * tdn_close / tdn_select (read-ready = a pending connection). */
int  tdn_listen(uint16_t port);

/* Pop one pending connection off a listener and wrap it as a normal
 * connected socket fd (tdn_send/recv/close/select all apply). Returns
 * the new fd, -1 when nothing is pending. */
int  tdn_accept(int lfd);

/* Bound local port of a socket (for tdn_listen(0) this is the
 * ephemeral port lwIP assigned). 0 when the fd is invalid. */
uint16_t tdn_localport(int fd);

/* Remote IPv4 address of a connected socket, host byte order.
 * 0 when unknown/not connected. */
uint32_t tdn_peerip(int fd);

#ifdef __cplusplus
}
#endif

#endif /* TDOSNET_H */
