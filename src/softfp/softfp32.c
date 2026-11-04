/*
 * softfp32.c
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
 * softfp32.c - IEEE-754 single-precision soft-float runtime (no FPU)
 *
 * Supplies the libgcc entry points GCC references when a translation unit is
 * built with -msoft-float. i386 GCC assumes an x87 FPU and therefore ships no
 * soft-fp routines in libgcc, so without these -msoft-float compiles cleanly
 * but fails to link.
 *
 * IMPORTANT INVARIANT
 * -------------------
 * This file must never perform floating-point *arithmetic*. Every value is
 * manipulated as raw bits through the `fu` union. If arithmetic crept in, the
 * compiler would emit a call to __addsf3 (etc.) from inside __addsf3 and
 * recurse until the stack died. Only bit operations, integer arithmetic, and
 * union load/store (a plain register move, not an FP op) are permitted.
 * Verification: objdump this file and confirm there are no calls to
 * __*sf3/__*df3 and no x87 opcodes.
 *
 * NUMERIC POLICY
 * --------------
 * - Rounding is round-to-nearest, ties-to-even (the IEEE-754 default, and
 *   what the x87 does in its default control word). This matters because the
 *   engine was developed and tuned against hardware floats; anything else
 *   would make results drift relative to the desktop builds.
 * - Subnormals are supported on both input and output (gradual underflow).
 * - NaNs propagate; a quiet NaN is produced for invalid operations.
 * - float->int conversions truncate toward zero (C semantics) and *saturate*
 *   on overflow. Note this deliberately differs from the x87, which yields
 *   0x80000000 ("integer indefinite") for any out-of-range or NaN input.
 *   Saturating was chosen because the rasteriser clamps coordinates derived
 *   from these conversions, and a saturated value stays on the correct side
 *   of the clamp whereas 0x80000000 flips a large positive to a large
 *   negative and produces off-screen garbage.
 *
 * REPRESENTATION USED INTERNALLY
 * ------------------------------
 * Intermediates carry the significand in `sig` with the implicit leading 1 at
 * bit 26 and three extra low bits (guard/round/sticky), i.e. a normalized
 * nonzero sig lies in [2^26, 2^27). With `exp` as the biased exponent, the
 * represented value is  sig * 2^-26 * 2^(exp - 127).  All three arithmetic
 * routines produce that form and hand it to round_pack(), which does the
 * normalization, gradual-underflow shift, rounding and encoding.
 */

#include "softfp32.h"
#include "softfp_internal.h"

/* `int` may be 16 bits on some compilers, where
 * `unsigned int` would silently truncate every value and
 * every shift past bit 15 would be undefined. Pick a 32-bit type. */
#if defined(__SIZEOF_INT__) && __SIZEOF_INT__ < 4
typedef unsigned long u32;
typedef long i32;
#define U32C(x) x##UL
#else
typedef unsigned int u32;
typedef int i32;
#define U32C(x) x##u
#endif
typedef unsigned long long u64;

/* Bit-level view of a float. Copying through this union is a register move;
 * it emits no floating-point instructions. */
typedef union {
    float f;
    u32 u;
} fu;

#define F32_SIGN_BIT 0x80000000u
#define F32_EXP_MAX  255
#define F32_FRAC     0x007FFFFFu
#define F32_HIDDEN   0x00800000u   /* implicit 1 at bit 23 (packed form) */

#define SIG_ONE      (U32C(1) << 26)    /* implicit 1 position in intermediates */
#define SIG_TOP      (U32C(1) << 27)

#define round_pack relic_softfp_round_pack32

static u32 bits_of(float x) {
    fu v;
    v.f = x;
    return v.u;
}

static float float_of(u32 b) {
    fu v;
    v.u = b;
    return v.f;
}

static float make(u32 sign, u32 exp, u32 frac) {
    return float_of(sign | (exp << 23) | frac);
}

/* Canonical quiet NaN. */
static float qnan(void) {
    return float_of(0x7FC00000u);
}

static float infinity(u32 sign) {
    return float_of(sign | 0x7F800000u);
}

/* Round `sig` (implicit bit at 26, 3 guard bits) at biased exponent `exp`
 * and pack. Handles renormalization, gradual underflow, overflow to
 * infinity, and ties-to-even. `exp` may be <= 0 or >= 255 on entry.
 *
 * Shared with softfp64.c via softfp_internal.h -- see the note there on why
 * this must not be duplicated. `round_pack` remains as a local alias so the
 * arithmetic routines below stay readable. */
float relic_softfp_round_pack32(u32 sign, int exp, u32 sig) {
    u32 round_bits;

    if (sig == 0)
        return make(sign, 0, 0);

    /* Bring sig into [2^26, 2^27). Shifting right ORs the discarded bit into
     * the low bit so stickiness is never lost. */
    while (sig >= SIG_TOP) {
        sig = (sig >> 1) | (sig & 1u);
        exp++;
    }
    while (sig < SIG_ONE) {
        sig <<= 1;
        exp--;
    }

    /* Gradual underflow: shift right so the result encodes with exponent 0. */
    if (exp <= 0) {
        int sh = 1 - exp;
        if (sh >= 27) {
            sig = 1u;               /* everything is sticky; rounds to 0 */
        } else {
            u32 lost = sig & ((U32C(1) << sh) - U32C(1));
            sig = (sig >> sh) | (lost != 0u);
        }
        exp = 0;
    }

    round_bits = sig & 7u;
    sig >>= 3;
    if (round_bits > 4u || (round_bits == 4u && (sig & 1u) != 0u))
        sig++;

    if (exp == 0) {
        /* Subnormal, unless rounding carried it up to the smallest normal.
         * In that case sig == 0x800000 exactly, so the frac field is 0 and
         * the exponent field becomes 1. */
        if (sig >= F32_HIDDEN)
            return make(sign, 1, sig & F32_FRAC);
        return make(sign, 0, sig);
    }

    /* Rounding may have carried out of bit 23. */
    if (sig >= (F32_HIDDEN << 1)) {
        sig >>= 1;
        exp++;
    }
    if (exp >= F32_EXP_MAX)
        return infinity(sign);
    return make(sign, (u32) exp, sig & F32_FRAC);
}

/* Unpack into sign / biased exponent / 24-bit significand (implicit bit
 * included for normals). Subnormals are reported with exp == 1 and no
 * implicit bit, which is the correct scaling and lets the arithmetic paths
 * treat them uniformly with normals. Returns the raw exponent field so
 * callers can still detect the 0 and 255 special cases. */
typedef struct {
    u32 sign;
    int exp;      /* biased, adjusted to 1 for subnormals */
    u32 sig;      /* 24-bit significand, implicit bit set iff normal */
    int raw_exp;  /* untouched exponent field: 0 => sub/zero, 255 => inf/nan */
    u32 frac;
} unpacked;

static void unpack(u32 b, unpacked *o) {
    o->sign = b & F32_SIGN_BIT;
    o->raw_exp = (int) ((b >> 23) & 0xFFu);
    o->frac = b & F32_FRAC;
    if (o->raw_exp == 0) {
        o->exp = 1;
        o->sig = o->frac;
    } else {
        o->exp = o->raw_exp;
        o->sig = o->frac | F32_HIDDEN;
    }
}

static int is_nan(const unpacked *v) {
    return v->raw_exp == F32_EXP_MAX && v->frac != 0u;
}

static int is_inf(const unpacked *v) {
    return v->raw_exp == F32_EXP_MAX && v->frac == 0u;
}

/* ------------------------------------------------------------------ */
/* Addition / subtraction                                              */
/* ------------------------------------------------------------------ */

/* Core add where `b_sign` has already been flipped by __subsf3 if needed. */
static float addsub(u32 ua, u32 ub) {
    unpacked a, b;
    int exp;
    u32 siga, sigb, sign;

    unpack(ua, &a);
    unpack(ub, &b);

    if (is_nan(&a) || is_nan(&b))
        return qnan();

    if (is_inf(&a)) {
        /* inf + (-inf) is invalid; inf + anything else is inf. */
        if (is_inf(&b) && a.sign != b.sign)
            return qnan();
        return infinity(a.sign);
    }
    if (is_inf(&b))
        return infinity(b.sign);

    /* Shift into the 3-guard-bit intermediate form. */
    siga = a.sig << 3;
    sigb = b.sig << 3;

    /* Align the smaller exponent up, accumulating a sticky bit. */
    if (a.exp > b.exp) {
        int sh = a.exp - b.exp;
        exp = a.exp;
        if (sh > 27)
            sigb = (sigb != 0u);
        else
            sigb = (sigb >> sh) | ((sigb & ((U32C(1) << sh) - U32C(1))) != 0u);
    } else if (b.exp > a.exp) {
        int sh = b.exp - a.exp;
        exp = b.exp;
        if (sh > 27)
            siga = (siga != 0u);
        else
            siga = (siga >> sh) | ((siga & ((U32C(1) << sh) - U32C(1))) != 0u);
    } else {
        exp = a.exp;
    }

    if (a.sign == b.sign) {
        sign = a.sign;
        return round_pack(sign, exp, siga + sigb);
    }

    /* Opposite signs: subtract the smaller magnitude from the larger. */
    if (siga > sigb)
        return round_pack(a.sign, exp, siga - sigb);
    if (sigb > siga)
        return round_pack(b.sign, exp, sigb - siga);
    /* Exact cancellation yields +0 under round-to-nearest. */
    return make(0, 0, 0);
}

float __addsf3(float a, float b) {
    return addsub(bits_of(a), bits_of(b));
}

float __subsf3(float a, float b) {
    return addsub(bits_of(a), bits_of(b) ^ F32_SIGN_BIT);
}

float __negsf2(float a) {
    return float_of(bits_of(a) ^ F32_SIGN_BIT);
}

/* ------------------------------------------------------------------ */
/* Multiplication                                                      */
/* ------------------------------------------------------------------ */

/* Left-align a subnormal significand so bit 23 is set, adjusting exp.
 * Only called for subnormal inputs, so `sig` is nonzero and < 2^23. */
static void norm_subnormal(u32 *sig, int *exp) {
    while ((*sig & F32_HIDDEN) == 0u) {
        *sig <<= 1;
        (*exp)--;
    }
}

float __mulsf3(float a, float b) {
    unpacked x, y;
    u32 sign;
    int exp;
    u64 prod;
    u32 sig;

    unpack(bits_of(a), &x);
    unpack(bits_of(b), &y);
    sign = x.sign ^ y.sign;

    if (is_nan(&x) || is_nan(&y))
        return qnan();
    if (is_inf(&x)) {
        if (y.sig == 0u && y.raw_exp == 0)   /* inf * 0 */
            return qnan();
        return infinity(sign);
    }
    if (is_inf(&y)) {
        if (x.sig == 0u && x.raw_exp == 0)
            return qnan();
        return infinity(sign);
    }
    if (x.sig == 0u || y.sig == 0u)
        return make(sign, 0, 0);

    if (x.raw_exp == 0) norm_subnormal(&x.sig, &x.exp);
    if (y.raw_exp == 0) norm_subnormal(&y.sig, &y.exp);

    /* 24x24 -> 48 bits. Both operands are 32-bit, so this is a single 386
     * `mul` instruction, not a call into libgcc's __muldi3. */
    prod = (u64) x.sig * (u64) y.sig;
    exp = x.exp + y.exp - 127;

    /* prod's leading bit is at 46 or 47. Reduce straight to the 27-bit
     * intermediate form, folding every discarded bit into a sticky bit.
     * Doing it in one step (rather than pre-shifting by 1 to normalize) is
     * what keeps the sticky exact -- a pre-shift would silently drop a bit
     * that rounding needs. */
    if (prod >= ((u64) 1 << 47)) {
        sig = (u32) (prod >> 21);
        if ((prod & (((u64) 1 << 21) - 1)) != 0)
            sig |= 1u;
        exp++;
    } else {
        sig = (u32) (prod >> 20);
        if ((prod & (((u64) 1 << 20) - 1)) != 0)
            sig |= 1u;
    }

    return round_pack(sign, exp, sig);
}

/* ------------------------------------------------------------------ */
/* Division                                                            */
/* ------------------------------------------------------------------ */

float __divsf3(float a, float b) {
    unpacked x, y;
    u32 sign, quot, rem;
    int exp, i;

    unpack(bits_of(a), &x);
    unpack(bits_of(b), &y);
    sign = x.sign ^ y.sign;

    if (is_nan(&x) || is_nan(&y))
        return qnan();
    if (is_inf(&x)) {
        if (is_inf(&y))
            return qnan();          /* inf / inf */
        return infinity(sign);
    }
    if (is_inf(&y))
        return make(sign, 0, 0);    /* finite / inf == signed zero */

    if (y.sig == 0u) {
        if (x.sig == 0u)
            return qnan();          /* 0 / 0 */
        return infinity(sign);      /* x / 0 */
    }
    if (x.sig == 0u)
        return make(sign, 0, 0);

    if (x.raw_exp == 0) norm_subnormal(&x.sig, &x.exp);
    if (y.raw_exp == 0) norm_subnormal(&y.sig, &y.exp);

    exp = x.exp - y.exp + 127;

    /* Restoring shift-subtract division producing 27 quotient bits.
     *
     * A 64-bit divide (__udivdi3) would also work but costs several hundred
     * cycles on a 486 and pulls in a libgcc helper; 27 iterations of 32-bit
     * compare/subtract is comparable and keeps this file self-contained.
     *
     * With both significands in [2^23, 2^24) the first quotient bit is the
     * units bit, so after 27 iterations quot == floor((x/y) * 2^26), exactly
     * the intermediate form round_pack expects.
     */
    rem = x.sig;
    quot = 0;
    for (i = 0; i < 27; i++) {
        quot <<= 1;
        if (rem >= y.sig) {
            rem -= y.sig;
            quot |= 1u;
        }
        rem <<= 1;
    }
    if (rem != 0u)
        quot |= 1u;                 /* sticky: quotient was inexact */

    return round_pack(sign, exp, quot);
}

/* ------------------------------------------------------------------ */
/* Comparisons                                                         */
/* ------------------------------------------------------------------ */

/* Ordered comparison of two non-NaN floats: -1, 0 or +1. */
static int cmp_ordered(u32 ua, u32 ub) {
    /* +0 == -0 must compare equal. */
    if (((ua | ub) & ~F32_SIGN_BIT) == 0u)
        return 0;
    if (ua == ub)
        return 0;
    if ((ua & F32_SIGN_BIT) != (ub & F32_SIGN_BIT))
        return (ua & F32_SIGN_BIT) ? -1 : 1;
    /* Same sign: the remaining bits order monotonically by magnitude, which
     * for negatives means the ordering is reversed. */
    if ((ua & F32_SIGN_BIT) == 0u)
        return (ua < ub) ? -1 : 1;
    return (ua > ub) ? -1 : 1;
}

static int either_nan(u32 ua, u32 ub) {
    return ((ua & 0x7F800000u) == 0x7F800000u && (ua & F32_FRAC) != 0u) ||
           ((ub & 0x7F800000u) == 0x7F800000u && (ub & F32_FRAC) != 0u);
}

int __unordsf2(float a, float b) {
    return either_nan(bits_of(a), bits_of(b));
}

/* Equality: 0 when equal. Unordered must not compare equal, so return 1. */
int __eqsf2(float a, float b) {
    u32 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return 1;
    return cmp_ordered(ua, ub) != 0;
}

int __nesf2(float a, float b) {
    return __eqsf2(a, b);
}

/* For lt/le the caller tests `result < 0` / `result <= 0`, so unordered must
 * return a positive value to make the comparison false. */
int __ltsf2(float a, float b) {
    u32 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return 1;
    return cmp_ordered(ua, ub);
}

int __lesf2(float a, float b) {
    u32 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return 1;
    return cmp_ordered(ua, ub);
}

/* For gt/ge the caller tests `result > 0` / `result >= 0`, so unordered must
 * return a negative value. */
int __gtsf2(float a, float b) {
    u32 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return -1;
    return cmp_ordered(ua, ub);
}

int __gesf2(float a, float b) {
    u32 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return -1;
    return cmp_ordered(ua, ub);
}

/* ------------------------------------------------------------------ */
/* Conversions                                                         */
/* ------------------------------------------------------------------ */

float __floatunsisf(u32 a) {
    u32 sig, t;
    int sh = 0;

    if (a == 0u)
        return make(0, 0, 0);

    /* Reduce to at most 27 significant bits, folding the rest into sticky.
     * round_pack handles the opposite case (small values needing a left
     * shift), so nothing more is required here. */
    t = a;
    while (t >= SIG_TOP) {
        t >>= 1;
        sh++;
    }
    sig = a >> sh;
    if (sh > 0 && (a & ((U32C(1) << sh) - U32C(1))) != 0u)
        sig |= 1u;

    /* value == sig * 2^sh, and round_pack reads sig as sig * 2^-26 *
     * 2^(exp-127), hence exp = 127 + 26 + sh. */
    return round_pack(0, 127 + 26 + sh, sig);
}

float __floatsisf(i32 a) {
    if (a < 0) {
        /* Negate in unsigned space so INT_MIN is handled without overflow. */
        u32 mag = (u32) 0 - (u32) a;
        return float_of(bits_of(__floatunsisf(mag)) | F32_SIGN_BIT);
    }
    return __floatunsisf((u32) a);
}

i32 __fixsfsi(float a) {
    u32 b = bits_of(a);
    unpacked v;
    int shift;
    u32 mag;

    unpack(b, &v);
    if (is_nan(&v))
        return 0;
    /* Exponent of a value with magnitude < 1 => truncates to 0. */
    if (v.exp < 127)
        return 0;

    shift = v.exp - 127 - 23;       /* how far to shift the 24-bit sig */
    if (shift >= 8)                 /* magnitude >= 2^31 => saturate */
        return v.sign ? (-2147483647 - 1) : 2147483647;

    if (shift >= 0)
        mag = v.sig << shift;
    else
        mag = v.sig >> (-shift);

    if (v.sign) {
        if (mag > 0x80000000u)
            return -2147483647L - 1;
        return (i32) ((u32) 0 - mag);
    }
    if (mag > 0x7FFFFFFFu)
        return 2147483647;
    return (i32) mag;
}

u32 __fixunssfsi(float a) {
    u32 b = bits_of(a);
    unpacked v;
    int shift;

    unpack(b, &v);
    if (is_nan(&v))
        return 0u;
    if (v.sign)                     /* negative (and -0) => 0 */
        return 0u;
    if (v.exp < 127)
        return 0u;

    shift = v.exp - 127 - 23;
    if (shift >= 9)                 /* magnitude >= 2^32 => saturate */
        return 0xFFFFFFFFu;
    if (shift < 0)
        return v.sig >> (-shift);
    {
        /* shift <= 8 and sig is 24 bits, so this cannot exceed 32 bits;
         * the u64 makes the saturation test exact. */
        u64 wide = (u64) v.sig << shift;
        if (wide > 0xFFFFFFFFull)
            return 0xFFFFFFFFu;
        return (u32) wide;
    }
}

/* ------------------------------------------------------------------ */
/* 64-bit conversions                                                  */
/* ------------------------------------------------------------------ */

/* By the time `shift` is computed below, v.exp >= 127 has already been
 * checked, which means v.raw_exp != 0 (a subnormal's exp is forced to 1 by
 * unpack(), which is < 127 and would already have returned). So v.sig
 * always has the hidden bit set here, i.e. v.sig is in [2^23, 2^24) -- the
 * comment on each function below relies on that range to justify its
 * saturation threshold. */

long long __fixsfdi(float a) {
    u32 b = bits_of(a);
    unpacked v;
    int shift;
    u64 mag;

    unpack(b, &v);
    if (is_nan(&v))
        return 0;
    if (v.exp < 127)
        return 0;

    shift = v.exp - 127 - 23;
    /* sig >= 2^23, so shift >= 41 already implies mag >= 2^64: unsafe to
     * even form the shift in u64, so saturate before computing it. */
    if (shift >= 41)
        return v.sign ? (-9223372036854775807LL - 1) : 9223372036854775807LL;

    mag = (shift >= 0) ? ((u64) v.sig << shift) : ((u64) v.sig >> (-shift));

    if (v.sign) {
        if (mag > 0x8000000000000000ull)
            return -9223372036854775807LL - 1;
        return (long long) (0ull - mag);
    }
    if (mag > 0x7FFFFFFFFFFFFFFFull)
        return 9223372036854775807LL;
    return (long long) mag;
}

unsigned long long __fixunssfdi(float a) {
    u32 b = bits_of(a);
    unpacked v;
    int shift;

    unpack(b, &v);
    if (is_nan(&v))
        return 0ull;
    if (v.sign)
        return 0ull;
    if (v.exp < 127)
        return 0ull;

    shift = v.exp - 127 - 23;
    /* sig >= 2^23, so shift >= 41 already implies mag >= 2^64. */
    if (shift >= 41)
        return 0xFFFFFFFFFFFFFFFFull;
    if (shift < 0)
        return (u64) v.sig >> (-shift);
    return (u64) v.sig << shift;
}

float __floatdisf(long long a) {
    if (a < 0) {
        /* Negate in unsigned space so INT64_MIN is handled without
         * overflow, mirroring __floatsisf's INT_MIN handling. */
        u64 mag = 0ull - (u64) a;
        return float_of(bits_of(__floatundisf(mag)) | F32_SIGN_BIT);
    }
    return __floatundisf((u64) a);
}

float __floatundisf(unsigned long long a) {
    u64 t;
    u32 sig;
    int sh = 0;

    if (a == 0ull)
        return make(0, 0, 0);

    /* Same reduction as __floatunsisf, just scanning up to 64 bits instead
     * of 32 before landing in the 27-bit intermediate form. */
    t = a;
    while (t >= ((u64) SIG_TOP)) {
        t >>= 1;
        sh++;
    }
    sig = (u32) (a >> sh);
    if (sh > 0 && (a & (((u64) 1 << sh) - 1ull)) != 0ull)
        sig |= 1u;

    return round_pack(0, 127 + 26 + sh, sig);
}
