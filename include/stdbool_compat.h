/*
 * stdbool_compat.h
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
 * stdbool_compat.h - Compatibility header for stdbool.h on MSVC 2012
 * 
 * MSVC 2012 doesn't support C99 and doesn't have stdbool.h.
 * This header provides the necessary definitions for bool, true, and false.
 */
#ifndef STDBOOL_COMPAT_H
#define STDBOOL_COMPAT_H

#if defined(_MSC_VER) && _MSC_VER < 1800
/* MSVC 2012 and earlier don't have stdbool.h */
#ifndef __cplusplus
typedef int bool;
#define true 1
#define false 0
#endif
#else
/* Use the standard header for other compilers */
#include <stdbool.h>
#endif

#endif /* STDBOOL_COMPAT_H */
