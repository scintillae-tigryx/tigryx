/*
 * softfp_strtod.c
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
 * softfp_strtod.c - correctly-rounded decimal -> double, no FPU
 *
 * Float literals compiled ON a soft-float host cannot use the platform
 * libc strtod: DJGPP's is x87 code, which is very slow on an FPU-less
 * machine and returns garbage under emulators that do not model the
 * coprocessor trap (DOSBox).
 *
 * This file implements a correctly-rounded decimal->double. Correctness
 * is obtained by evaluating M * 10^E exactly with a small big-integer and
 * extracting the top 56 quotient bits by binary long division, then letting
 * the shared round_pack do round-to-nearest-even. There is no Clinger
 * fast-path-and-hope: every input, including 100-digit significands and
 * extreme exponents, goes through the exact path, so the result is
 * bit-identical to glibc strtod.
 *
 * The parse: value = M * 10^E10 where M is the digit string as an integer
 * (buf is at most ~60 chars) and E10 folds in the decimal point and the
 * e-notation exponent. 10^E10 = 5^E10 * 2^E10, so:
 *   E10 >= 0:  value = (M * 5^E10) * 2^E10        -> Num = M*5^E10, B = E10
 *   E10 <  0:  value = M / (5^-E10 * 2^-E10)     -> Den = 5^-E10, B = E10
 * Then value = (Num/Den) * 2^B is written via a 56-bit window of Num/Den
 * plus a sticky bit, which is exactly round_pack64's input contract.
 *
 * Like every file in src/softfp this performs NO floating-point arithmetic.
 */

#include "softfp_internal.h"

/* `int` may be 16 bits on some compilers; u32 must stay 32-bit. */
#if defined(__SIZEOF_INT__) && __SIZEOF_INT__ < 4
typedef unsigned long u32;
#else
typedef unsigned int u32;
#endif
typedef unsigned long long u64;

/* Big unsigned integer, little-endian u32 limbs. 40 limbs = 1280 bits,
 * comfortably above the worst case (~1130 bits for Num = 63-digit M *
 * 5^330, or Den = 5^390 plus alignment slack). */
#define BN_CAP 40

typedef struct {
    u32 w[BN_CAP];
    int n;          /* limbs in use; n==0 means value 0 */
} bnum;

static void bn_zero(bnum *b) { b->n = 0; }

static int bn_iszero(const bnum *b) {
    return b->n == 0;
}

/* Drop unused high limbs. */
static void bn_norm(bnum *b) {
    while (b->n > 0 && b->w[b->n - 1] == 0u)
        b->n--;
}

/* b *= m (small multiplier), then optionally add. No-op-safe on overflow:
 * callers clamp E10 so limbs never exceed BN_CAP; a defensive clamp keeps
 * a stray huge exponent from scribbling. */
static void bn_mul_small(bnum *b, u32 m) {
    u64 carry = 0;
    int i;
    for (i = 0; i < b->n; i++) {
        u64 t = (u64) b->w[i] * m + carry;
        b->w[i] = (u32) t;
        carry = t >> 32;
    }
    while (carry && b->n < BN_CAP) {
        b->w[b->n++] = (u32) carry;
        carry >>= 32;
    }
    /* carry still nonzero => overflowed capacity; only reachable for
     * exponents the caller has already proven produce inf/0 */
}

static void bn_add_small(bnum *b, u32 a) {
    u64 t;
    int i = 0;
    /* Keep the n==0 <=> value-0 invariant: adding a 0 digit to an empty
     * number must leave it empty, or "0.0" never takes the +-0 early
     * return and spins forever in the Num >= Den alignment loop. */
    if (b->n == 0) { if (a) { b->w[0] = a; b->n = 1; } return; }
    t = (u64) b->w[0] + a;
    b->w[0] = (u32) t;
    while (t >> 32) {
        if (++i >= b->n) { b->w[b->n++] = 1; return; }
        t = (u64) b->w[i] + 1;
        b->w[i] = (u32) t;
    }
}

/* b <<= k bits. */
static void bn_shl(bnum *b, int k) {
    int limb = k >> 5, bit = k & 31, i;
    if (b->n == 0 || k == 0)
        return;
    if (b->n + limb > BN_CAP)
        limb = BN_CAP - b->n;           /* defensive: overflow => inf/0 */
    for (i = b->n - 1; i >= 0; i--)
        b->w[i + limb] = b->w[i];
    for (i = 0; i < limb; i++)
        b->w[i] = 0;
    b->n += limb;
    if (bit) {
        u32 carry = 0;
        for (i = 0; i < b->n; i++) {
            u32 t = (b->w[i] << bit) | carry;
            carry = b->w[i] >> (32 - bit);
            b->w[i] = t;
        }
        if (carry && b->n < BN_CAP)
            b->w[b->n++] = carry;
    }
    bn_norm(b);
}

/* -1 / 0 / +1 comparison. */
static int bn_cmp(const bnum *a, const bnum *b) {
    int i;
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i])
            return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* a -= b; requires a >= b. */
static void bn_sub(bnum *a, const bnum *b) {
    u64 borrow = 0;
    int i;
    for (i = 0; i < a->n; i++) {
        u64 bb = i < b->n ? b->w[i] : 0u;
        u64 t = (u64) a->w[i] - bb - borrow;
        a->w[i] = (u32) t;
        borrow = (t >> 32) & 1u;
    }
    bn_norm(a);
}

static int bn_bitlen(const bnum *b) {
    u32 top;
    int bits;
    if (b->n == 0)
        return 0;
    top = b->w[b->n - 1];
    bits = b->n * 32;
    while ((top & 0x80000000u) == 0) { top <<= 1; bits--; }
    return bits;
}

double softfp_strtod(const char *s) {
    bnum num, den, ds, rem;
    int neg = 0, saw = 0, e10 = 0;
    int nb, db, t, i;
    u64 q;
    u32 sign;

    /* ---------------- parse "digits[.digits][eE[+-]digits]" ---------- */
    if (*s == '-' || *s == '+')
        neg = (*s++ == '-');
    sign = neg ? 0x80000000u : 0u;

    bn_zero(&num);
    while (*s >= '0' && *s <= '9') {
        bn_mul_small(&num, 10);
        bn_add_small(&num, (u32) (*s - '0'));
        s++;
        saw = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            bn_mul_small(&num, 10);
            bn_add_small(&num, (u32) (*s - '0'));
            e10--;
            s++;
            saw = 1;
        }
    }
    if (!saw || bn_iszero(&num))
        return relic_softfp_round_pack64(sign, 0, 0);   /* +-0 */
    if (*s == 'e' || *s == 'E') {
        int eneg = 0, e2 = 0;
        s++;
        if (*s == '-' || *s == '+')
            eneg = (*s++ == '-');
        while (*s >= '0' && *s <= '9') {
            if (e2 < 10000)
                e2 = e2 * 10 + (*s - '0');
            s++;
        }
        e10 += eneg ? -e2 : e2;
    }

    /* Beyond these bounds the result is certainly inf or 0 (M >= 1 and
     * < 10^64, |DBL| range ~ 4.9e-324 .. 1.8e308). */
    if (e10 > 400)
        return relic_softfp_round_pack64(sign, 2047, 1ull << 55);
    if (e10 < -450)
        return relic_softfp_round_pack64(sign, 0, 0);

    /* ---------------- scale: Num/Den * 2^B --------------------------- */
    bn_zero(&den);
    if (e10 >= 0) {
        /* Num = M * 5^E10, B = E10, Den = 1 */
        int k;
        for (k = 0; k < e10; k++)
            bn_mul_small(&num, 5);
        den.w[0] = 1; den.n = 1;
        /* B = e10 folded into the exponent below */
        t = e10;
    } else {
        /* Den = 5^-E10, B = E10 (negative), Num = M */
        int k;
        den.w[0] = 1; den.n = 1;
        for (k = 0; k < -e10; k++)
            bn_mul_small(&den, 5);
        t = e10;
    }

    /* ---------------- divide: 56-bit window + sticky ----------------- */
    /* Ensure Num >= Den so the quotient's leading bit is at position
     * t >= 0; each left shift of Num is compensated in t (which now
     * tracks the binary exponent, B, and alignment). */
    while (bn_cmp(&num, &den) < 0) {
        bn_shl(&num, 1);
        t--;
    }

    nb = bn_bitlen(&num);
    db = bn_bitlen(&den);
    /* quotient bit positions: t + (nb - db), stepping down. Ds = Den
     * shifted so its MSB aligns with Num's MSB. */
    {
        int align = nb - db;
        t += align;
        ds = den;
        bn_shl(&ds, align);
        /* Ds <= Num < 2*Ds guaranteed? Num >= Den and Num may still be
         * < Ds by at most one bit - check and back off. */
        if (bn_cmp(&ds, &num) > 0) {
            ds = den;
            bn_shl(&ds, --align);
            t--;
        }
    }

    /* Restoring division: 56 quotient bits. rem < 2*Ds invariant holds
     * because Num < 2*Ds (Num < 2^nb and Ds >= 2^(nb-1) by alignment). */
    rem = num;
    q = 0;
    for (i = 0; i < 56; i++) {
        q <<= 1;
        if (bn_cmp(&rem, &ds) >= 0) {
            bn_sub(&rem, &ds);
            q |= 1ull;
        }
        /* shift rem left one and ds's implied window right: equivalent
         * to rem <<= 1 against fixed Ds. */
        bn_shl(&rem, 1);
    }
    if (!bn_iszero(&rem))
        q |= 1ull;                  /* sticky: division was inexact */

    /* value = (q + delta) * 2^(t - 55), delta in [0,1): sig carries the
     * implicit bit at 55 -> exp - 1023 = t */
    return relic_softfp_round_pack64(sign, t + 1023, q);
}
