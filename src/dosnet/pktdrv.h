/*
 * pktdrv.h
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
 * pktdrv.h - DOS packet-driver client (FTP Software spec), DJGPP/DPMI.
 */

#ifndef DOSNET_PKTDRV_H
#define DOSNET_PKTDRV_H

#include <stdint.h>

/* Find + open the packet driver, register the receive handler and
 * fetch the MAC address. Returns 0 on success. */
int  pktdrv_init(uint8_t mac[6]);

/* Transmit one Ethernet frame. Returns 0 on success. */
int  pktdrv_send(const void *frame, unsigned len);

/* Poll for a received frame. Copies up to buflen into buf, returns
 * frame length, 0 if none pending. Frames larger than buflen are
 * truncated (and the excess dropped). */
int  pktdrv_poll(uint8_t *buf, unsigned buflen);

/* Unregister and release all resources. */
void pktdrv_shutdown(void);

#endif /* DOSNET_PKTDRV_H */
