/*
 * pktdrv.c
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
 * pktdrv.c - DOS packet-driver client for DJGPP/DPMI.
 *
 * Implements the FTP Software packet-driver spec subset that tdosnet
 * needs: discovery, access_type, send_pkt, get_address, release_type.
 *
 * DJGPP specifics:
 *  - The driver's receive upcall arrives in REAL mode, so the receiver
 *    is a DPMI real-mode callback allocated with
 *    _go32_dpmi_allocate_real_mode_callback_retf(). info.pm_offset must
 *    point at the handler BEFORE the allocate call - DPMI installs a
 *    wrapper that calls (*pm_offset)(&regs), and a zero pm_offset gives
 *    "Page Fault ... in RMCB at eip=0" the instant a frame arrives.
 *  - Buffers the driver writes to must live below 1MB, so the receive
 *    ring itself is a pool of PD_RX_RING conventional-memory slots.
 *  - The callback runs at interrupt time and may therefore only touch
 *    LOCKED memory and call NO libc function (movedata/dosmemget are
 *    not locked). It only hands out a slot address and records the
 *    length; the copy up into protected-mode memory happens later in
 *    pktdrv_poll(), in normal context.
 *
 * Receive protocol (per spec): the driver calls the upcall twice.
 *   First:  AX=0, CX=frame length, DS:SI -> frame in driver memory.
 *           We return ES:DI -> a free conventional-memory slot
 *           (or 0:0 to drop the frame when the ring is full).
 *   Second: AX=1, CX=bytes copied, DS:SI -> that slot.
 *           We commit the slot by recording its length.
 */

#include "pktdrv.h"

#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#include <string.h>

/* Packet-driver entry signature follows a 3-byte jump at the handler. */
#define PD_SIG_OFFS   3
#define PD_SIG        "PKT DRVR"
#define PD_SIG_LEN    8

#define PD_FIRST_INT  0x60
#define PD_LAST_INT   0x80

/* Packet-driver function numbers (in AH via the vector INT call). */
#define PD_ACCESS_TYPE   2
#define PD_RELEASE_TYPE  3
#define PD_SEND_PKT      4
#define PD_GET_ADDRESS   6

/* Ethernet class for access_type's if_class parameter. */
#define PD_CLASS_ETHERNET 1

#define PD_FRAME_MAX  1536          /* 1514-byte frame + slack, paragraph-aligned */
#define PD_RX_RING    8             /* frames queued between polls */

static int     pd_found;            /* packet-driver INT vector, 0 = none */
static int     pd_handle;           /* our access_type handle */

static _go32_dpmi_seginfo   pd_rmcb;   /* real-mode callback descriptor */
static _go32_dpmi_registers pd_regs;

/* One conventional-memory block holding PD_RX_RING frame slots plus one
 * TX staging slot at the end. Keeping it in a single allocation means
 * one DOS-memory block to manage and paragraph-aligned slots. */
static _go32_dpmi_seginfo pd_pool;
static unsigned long pd_pool_lin;      /* linear address of the pool */
static unsigned short pd_pool_seg;     /* real-mode segment of the pool */

/* Slot bookkeeping. The callback writes pd_len[]/pd_head, poll reads
 * them and advances pd_tail - all locked, all plain integer writes. */
static volatile unsigned short pd_len[PD_RX_RING];
static volatile unsigned       pd_head;   /* callback commits here */
static volatile unsigned       pd_tail;   /* poll consumes here */

/* Frame slot i lives at pool offset i * PD_FRAME_MAX; the TX slot is
 * the one past the end. Offsets are multiples of 16 so each slot has
 * its own real-mode segment with offset 0. */
#define PD_SLOT_PARAS   (PD_FRAME_MAX / 16)
#define PD_SLOT_SEG(i)  ((unsigned short)(pd_pool_seg + (i) * PD_SLOT_PARAS))
#define PD_SLOT_LIN(i)  (pd_pool_lin + (unsigned long)(i) * PD_FRAME_MAX)
#define PD_TX_SLOT      PD_RX_RING

/* ------------------------------------------------------------------ */
/* Real-mode receiver upcall - INTERRUPT CONTEXT                       */
/* ------------------------------------------------------------------ */

static void pktdrv_rx_callback(_go32_dpmi_registers *r)
{
    unsigned slot, next;

    if (r->x.ax == 0) {
        /* First call: hand the driver a free slot to copy into. */
        slot = pd_head;
        next = (slot + 1) % PD_RX_RING;
        if (r->x.cx > PD_FRAME_MAX || next == pd_tail) {
            r->x.es = 0;          /* oversized or ring full: drop it */
            r->x.di = 0;
        } else {
            r->x.es = PD_SLOT_SEG(slot);
            r->x.di = 0;
        }
    } else if (r->x.ax == 1) {
        /* Second call: the frame is in the slot we handed out. Commit
         * it - no copy, no libc, nothing unlocked. */
        unsigned len = r->x.cx;
        if (len > PD_FRAME_MAX) len = PD_FRAME_MAX;
        slot = pd_head;
        pd_len[slot] = (unsigned short)len;
        pd_head = (slot + 1) % PD_RX_RING;
    }
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

static int pktdrv_find(void)
{
    int i;
    for (i = PD_FIRST_INT; i <= PD_LAST_INT; i++) {
        _go32_dpmi_seginfo vec;
        unsigned long lin;
        char sig[PD_SIG_LEN];

        _go32_dpmi_get_real_mode_interrupt_vector(i, &vec);
        if (vec.rm_segment == 0 && vec.rm_offset == 0)
            continue;

        lin = ((unsigned long)vec.rm_segment << 4) + vec.rm_offset;
        dosmemget(lin + PD_SIG_OFFS, PD_SIG_LEN, sig);
        if (memcmp(sig, PD_SIG, PD_SIG_LEN) == 0)
            return i;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int pktdrv_init(uint8_t mac[6])
{
    __dpmi_regs r;

    pd_head = pd_tail = 0;

    pd_found = pktdrv_find();
    if (!pd_found)
        return -1;

    /* Conventional-memory pool: PD_RX_RING receive slots + 1 TX slot.
     * seginfo.size is in paragraphs. */
    pd_pool.size = PD_SLOT_PARAS * (PD_RX_RING + 1);
    if (_go32_dpmi_allocate_dos_memory(&pd_pool))
        return -2;
    pd_pool_seg = (unsigned short)pd_pool.rm_segment;
    pd_pool_lin = (unsigned long)pd_pool_seg << 4;

    /* Lock everything the upcall path touches, BEFORE the callback can
     * ever fire. The conventional-memory pool needs no locking - DOS
     * low memory is never paged. */
    _go32_dpmi_lock_code(pktdrv_rx_callback, 4096);
    _go32_dpmi_lock_data((void *)pd_len, sizeof(pd_len));
    _go32_dpmi_lock_data((void *)&pd_head, sizeof(pd_head));
    _go32_dpmi_lock_data((void *)&pd_tail, sizeof(pd_tail));
    _go32_dpmi_lock_data((void *)&pd_pool_seg, sizeof(pd_pool_seg));
    _go32_dpmi_lock_data((void *)&pd_regs, sizeof(pd_regs));

    /* Real-mode callback for the receiver upcall. pm_offset/pm_selector
     * must name the handler before the allocate call (see the file
     * header comment). */
    pd_rmcb.pm_offset = (unsigned long)pktdrv_rx_callback;
    pd_rmcb.pm_selector = _my_cs();
    if (_go32_dpmi_allocate_real_mode_callback_retf(&pd_rmcb, &pd_regs)) {
        pd_rmcb.rm_segment = pd_rmcb.rm_offset = 0;
        return -3;
    }

    /* access_type: class 1 (Ethernet), type -1 (all types), number 0.
     * typelen 0 with BX=0xffff receives every frame - fine, lwIP's
     * ethernet_input filters ARP/IP and drops the rest. */
    memset(&r, 0, sizeof(r));
    r.h.ah = PD_ACCESS_TYPE;
    r.h.al = PD_CLASS_ETHERNET;
    r.x.bx = 0xffff;
    r.h.dl = 0;
    r.x.cx = 0;
    r.x.ds = 0;
    r.x.si = 0;
    r.x.es = (unsigned short)pd_rmcb.rm_segment;
    r.x.di = (unsigned short)pd_rmcb.rm_offset;
    __dpmi_int(pd_found, &r);
    if (r.x.flags & 1)            /* carry set = error */
        return -4;
    pd_handle = r.x.ax;

    /* get_address: ES:DI -> 6-byte buffer. Use the TX slot, so an RX
     * frame landing between access_type and here cannot clobber it. */
    memset(&r, 0, sizeof(r));
    r.h.ah = PD_GET_ADDRESS;
    r.x.bx = pd_handle;
    r.x.cx = 6;
    r.x.es = PD_SLOT_SEG(PD_TX_SLOT);
    r.x.di = 0;
    __dpmi_int(pd_found, &r);
    if (r.x.flags & 1)
        return -5;
    dosmemget(PD_SLOT_LIN(PD_TX_SLOT), 6, mac);

    return 0;
}

int pktdrv_send(const void *frame, unsigned len)
{
    __dpmi_regs r;

    if (!pd_found || len == 0 || len > PD_FRAME_MAX)
        return -1;

    dosmemput(frame, len, PD_SLOT_LIN(PD_TX_SLOT));

    memset(&r, 0, sizeof(r));
    r.h.ah = PD_SEND_PKT;
    r.x.cx = len;
    r.x.ds = PD_SLOT_SEG(PD_TX_SLOT);
    r.x.si = 0;
    __dpmi_int(pd_found, &r);

    return (r.x.flags & 1) ? -1 : 0;
}

int pktdrv_poll(uint8_t *buf, unsigned buflen)
{
    unsigned slot, len;

    if (pd_head == pd_tail)
        return 0;

    slot = pd_tail;
    len = pd_len[slot];
    if (len > buflen)
        len = buflen;             /* truncate oversized frame */
    dosmemget(PD_SLOT_LIN(slot), len, buf);
    /* Release the slot only after the copy, so the callback cannot
     * hand the same conventional memory back to the driver mid-read. */
    pd_tail = (slot + 1) % PD_RX_RING;
    return (int)len;
}

void pktdrv_shutdown(void)
{
    __dpmi_regs r;

    if (pd_found && pd_handle) {
        memset(&r, 0, sizeof(r));
        r.h.ah = PD_RELEASE_TYPE;
        r.x.bx = pd_handle;
        __dpmi_int(pd_found, &r);
    }
    pd_handle = 0;

    /* Free the callback before the memory it hands out. */
    if (pd_rmcb.rm_offset || pd_rmcb.rm_segment) {
        _go32_dpmi_free_real_mode_callback(&pd_rmcb);
        pd_rmcb.rm_segment = pd_rmcb.rm_offset = 0;
    }
    if (pd_pool.rm_segment) {
        _go32_dpmi_free_dos_memory(&pd_pool);
        pd_pool.rm_segment = 0;
        pd_pool_seg = 0;
    }
    pd_found = 0;
    pd_head = pd_tail = 0;
}
