/*
 * lwipopts.h
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
 * lwipopts.h - lwIP configuration for the DOS packet-driver port.
 *
 * NO_SYS poll-driven port: the application calls sys_check_timeouts()
 * and pktdrv_poll() from its main loop (tdosnet.c tdn_poll()).
 */

#ifndef DOSNET_LWIPOPTS_H
#define DOSNET_LWIPOPTS_H

#define NO_SYS                 1
#define SYS_LIGHTWEIGHT_PROT   1

/* Use libc malloc for everything - DJGPP flat memory has plenty of it. */
#define MEM_LIBC_MALLOC        1
#define MEMP_MEM_MALLOC        1
#define MEM_ALIGNMENT          4

/* Protocols: IPv4 TCP client + UDP DNS + DHCP + ICMP(ping replies) + ARP */
#define LWIP_IPV4              1
#define LWIP_IPV6              0
#define LWIP_ARP               1
#define LWIP_ETHERNET          1
#define LWIP_ICMP              1
#define LWIP_RAW               0
#define LWIP_UDP               1
#define LWIP_TCP               1
#define LWIP_DNS               1
#define LWIP_DHCP              1
#define DHCP_DOES_ARP_CHECK    0
#define LWIP_DHCP_MAX_DNS_SERVERS 2
#define DNS_MAX_SERVERS        2

/* No sequential/socket APIs - tdosnet.c talks to the raw API directly. */
#define LWIP_SOCKET            0
#define LWIP_NETCONN           0

/* Modest TCP sizing - HTTP client only, short-lived connections.
 * DJGPP runs in a flat address space so we can afford a decent window. */
#define TCP_MSS                1460
#define TCP_WND                (4 * TCP_MSS)
#define TCP_SND_BUF            (4 * TCP_MSS)
#define TCP_SND_QUEUELEN       8
#define MEMP_NUM_TCP_PCB       16
#define MEMP_NUM_TCP_SEG       16
#define MEMP_NUM_UDP_PCB       4

#define PBUF_POOL_SIZE         16
#define MEMP_NUM_REASSDATA     4
#define IP_REASS_MAX_PBUFS     4

#define LWIP_STATS             0
#define LWIP_STATS_DISPLAY     0
#define MIB2_STATS             0

#define LWIP_CHECKSUM_CTRL_PER_NETIF 0
#define CHECKSUM_GEN_IP        1
#define CHECKSUM_GEN_UDP       1
#define CHECKSUM_GEN_TCP       1
#define CHECKSUM_GEN_ICMP      1
#define CHECKSUM_CHECK_IP      1
#define CHECKSUM_CHECK_UDP     1
#define CHECKSUM_CHECK_TCP     1

#define LWIP_RAND()            ((u32_t)rand())

#endif /* DOSNET_LWIPOPTS_H */
