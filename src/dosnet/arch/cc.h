/*
 * cc.h
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
 * arch/cc.h - compiler/platform definitions for the DJGPP lwIP port.
 */

#ifndef DOSNET_ARCH_CC_H
#define DOSNET_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#define BYTE_ORDER LITTLE_ENDIAN

typedef uintptr_t mem_ptr_t;

/* In NO_SYS mode lwip/sys.h never includes arch/sys_arch.h, so the
 * protection type must be defined here in cc.h instead. */
typedef int sys_prot_t;

#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "lu"
#define S32_F "ld"
#define X32_F "lx"
#define SZT_F "u"

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    printf("lwIP assert failed: %s (%s:%d)\n", x, __FILE__, __LINE__); \
    abort(); \
} while (0)

#endif /* DOSNET_ARCH_CC_H */
