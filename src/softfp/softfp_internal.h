/*
 * softfp_internal.h
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
 * softfp_internal.h - shared internals of the soft-float runtime.
 *
 * Not a public interface. Only softfp32.c / softfp64.c include this; it
 * exists so the f32 rounding-and-pack step has exactly one implementation.
 * Duplicating that logic is the single most likely source of subtle
 * rounding divergence between routines, so it is deliberately shared.
 */
#ifndef RELIC_SOFTFP_INTERNAL_H
#define RELIC_SOFTFP_INTERNAL_H

/* Fixed-width types. `int` may be 16 bits on some compilers:
 * the libgcc libcall ABI's SI-mode args and returns
 * are always 32-bit regardless, and the `sign` words below carry bit 31. */
#if defined(__SIZEOF_INT__) && __SIZEOF_INT__ < 4
typedef unsigned long softfp_u32;
typedef long          softfp_i32;
#else
typedef unsigned int  softfp_u32;
typedef int           softfp_i32;
#endif

/* Round and encode an f32 from the intermediate form used throughout this
 * library: `sig` carries the significand with its implicit leading 1 at bit
 * 26 plus three low guard/round/sticky bits, and `exp` is the biased
 * exponent, so the value represented is  sig * 2^-26 * 2^(exp - 127).
 *
 * `exp` may be out of range on entry (<= 0 or >= 255); gradual underflow and
 * overflow to infinity are handled here. Rounding is to nearest, ties to
 * even. `sign` is 0 or 0x80000000.
 *
 * Any bits discarded by the caller when reducing a wider intermediate down to
 * this form MUST be OR-ed into sig's low bit, or rounding will be wrong in
 * the tie cases.
 */
float relic_softfp_round_pack32(softfp_u32 sign, int exp, softfp_u32 sig);

/* Same contract, f64: `sig` carries the significand with its implicit
 * leading 1 at bit 55 plus three low guard/round/sticky bits, so the
 * value is  sig * 2^-55 * 2^(exp - 1023).  `sign` is 0 or 0x80000000. */
double relic_softfp_round_pack64(softfp_u32 sign, int exp,
                                 unsigned long long sig);

#endif /* RELIC_SOFTFP_INTERNAL_H */
