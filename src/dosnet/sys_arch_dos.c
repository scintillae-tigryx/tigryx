/*
 * sys_arch_dos.c
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
 * sys_arch_dos.c - NO_SYS sys_arch glue for DJGPP.
 *
 * sys_now() uses DJGPP's uclock(), which reads PIT counter 0 on demand
 * (~microsecond resolution, no reprogramming). Interrupt protection is
 * cli/sti; nothing in lwIP runs from interrupt context here - the
 * packet-driver upcall only touches tdosnet's own ring buffer - but
 * protecting is cheap insurance.
 */

#include <lwip/sys.h>
#include <time.h>

u32_t sys_now(void)
{
    return (u32_t)(uclock() / (UCLOCKS_PER_SEC / 1000));
}

sys_prot_t sys_arch_protect(void)
{
    int eflags;
    __asm__ volatile ("pushfl\n\tpopl %0\n\tcli" : "=r"(eflags));
    return (eflags & 0x200) ? 1 : 0;   /* previous IF state */
}

void sys_arch_unprotect(sys_prot_t pval)
{
    if (pval)
        __asm__ volatile ("sti");
}
