/*
 * softfp32.h
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
 * softfp32.h - IEEE-754 single-precision soft-float runtime (no FPU)
 *
 * Declares the libgcc-compatible entry points that GCC emits calls to when
 * a translation unit is compiled with -msoft-float. Nothing here is called
 * directly by hand-written code; the compiler generates the references.
 *
 * Why this exists: on i386 GCC assumes an x87 FPU, so its libgcc ships no
 * soft-fp routines and -msoft-float fails to link. Supplying them ourselves
 * is what makes FPU-less targets (486SX, 386SX) usable, because the alternative -- trap-and-emulate via the DPMI
 * host (-lemu) -- costs on the order of 25,000 cycles per operation, versus
 * tens to a few hundred cycles here.
 *
 * Scope is deliberately single-precision only. See softfp32.c for the
 * rounding and NaN policy these routines implement.
 */
#ifndef RELIC_SOFTFP32_H
#define RELIC_SOFTFP32_H

/* GCC represents soft-float values as `float` in the ABI, but with
 * -msoft-float they are passed/returned in integer registers. Declaring the
 * routines with `float` parameters keeps the prototypes matching what the
 * compiler expects; it does not reintroduce FPU use. */

#ifdef __cplusplus
extern "C" {
#endif

/* Arithmetic */
float __addsf3(float a, float b);
float __subsf3(float a, float b);
float __mulsf3(float a, float b);
float __divsf3(float a, float b);
float __negsf2(float a);

/* Comparisons.
 *
 * libgcc's contract: each returns a signed int whose sign encodes the
 * ordering (<0, 0, >0). GCC then compares that result against zero using
 * the condition matching the source-level operator. The distinct names
 * exist so the unordered (NaN) case can return the value that makes the
 * caller's test come out false:
 *   __eqsf2/__nesf2 return nonzero when unordered
 *   __ltsf2/__lesf2 return a positive value when unordered
 *   __gtsf2/__gesf2 return a negative value when unordered
 */
int __eqsf2(float a, float b);
int __nesf2(float a, float b);
int __ltsf2(float a, float b);
int __lesf2(float a, float b);
int __gtsf2(float a, float b);
int __gesf2(float a, float b);
int __unordsf2(float a, float b);

/* Conversions (f32 <-> integer). Truncate toward zero, per C semantics.
 * Out-of-range and NaN inputs saturate (see softfp32.c's NUMERIC POLICY
 * comment for why that differs from the x87's "integer indefinite"). */
/* SI-mode args/returns are always 32-bit, even where `int` is
 * 16-bit - use fixed-width types here. */
#if defined(__SIZEOF_INT__) && __SIZEOF_INT__ < 4
long           __fixsfsi(float a);
unsigned long  __fixunssfsi(float a);
float __floatsisf(long a);
float __floatunsisf(unsigned long a);
#else
int            __fixsfsi(float a);
unsigned int   __fixunssfsi(float a);
float __floatsisf(int a);
float __floatunsisf(unsigned int a);
#endif

/* 64-bit counterparts, for (int64_t)/(uint64_t) casts of a float. */
long long __fixsfdi(float a);
unsigned long long __fixunssfdi(float a);
float __floatdisf(long long a);
float __floatundisf(unsigned long long a);

#ifdef __cplusplus
}
#endif

#endif /* RELIC_SOFTFP32_H */
