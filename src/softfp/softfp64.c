/*
 * softfp64.c
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
 * softfp64.c - IEEE-754 double-precision soft-float runtime (no FPU)
 *
 * Supplies the libgcc entry points GCC references for `double` when a
 * translation unit is built with -msoft-float: arithmetic, comparisons,
 * and the int/float conversion routines. i386 GCC assumes an x87 FPU and
 * ships no soft-fp routines in libgcc, so without these an -msoft-float
 * build that touches a double fails to link.
 *
 * IMPORTANT INVARIANT
 * -------------------
 * This file must never perform floating-point *arithmetic*. Every value is
 * manipulated as raw bits through the `fu64` union. If arithmetic crept in,
 * the compiler would emit a call to __adddf3 (etc.) from inside __adddf3 and
 * recurse until the stack died. Only bit operations, integer arithmetic,
 * and union load/store are permitted. Verification: objdump this file and
 * confirm there are no calls to __*df3/__*sf3 and no x87 opcodes.
 *
 * NUMERIC POLICY
 * --------------
 * Same contract as softfp32.c:
 * - Round-to-nearest, ties-to-even on every operation that can round.
 * - Subnormals supported on input and output (gradual underflow).
 * - NaNs propagate; invalid operations produce a quiet NaN.
 * - double->int truncates toward zero and *saturates* on overflow.
 *
 * REPRESENTATION USED INTERNALLY
 * ------------------------------
 * Intermediates carry the significand in `sig` with the implicit leading 1
 * at bit 55 and three low guard/round/sticky bits, i.e. a normalized
 * nonzero sig lies in [2^55, 2^56). With `exp` the biased exponent, the
 * represented value is  sig * 2^-55 * 2^(exp - 1023).  All arithmetic
 * routines produce that form and hand it to round_pack(), which does
 * normalization, gradual-underflow shift, rounding, and encoding.
 */

#include "softfp_internal.h"

/* `int` may be 16 bits on some compilers; the libgcc SI-mode
 * libcall args/returns are 32-bit regardless. */
#if defined(__SIZEOF_INT__) && __SIZEOF_INT__ < 4
typedef unsigned long u32;
typedef long i32;
#else
typedef unsigned int u32;
typedef int i32;
#endif
typedef unsigned long long u64;

typedef union {
    float f;
    u32 u;
} fu32;

typedef union {
    double d;
    u64 u;
} fu64;

#define F32_SIGN 0x80000000u
#define F32_FRAC 0x007FFFFFu

#define F64_SIGN   0x8000000000000000ull
#define F64_FRAC   0x000FFFFFFFFFFFFFull
#define F64_HID    0x0010000000000000ull   /* implicit 1 at bit 52 */
#define F64_EXPMAX 2047

#define SIG_ONE    (1ull << 55)    /* implicit 1 position in intermediates */
#define SIG_TOP    (1ull << 56)

#define round_pack relic_softfp_round_pack64

static u64 bits_of(double x) {
    fu64 v;
    v.d = x;
    return v.u;
}

static double double_of(u64 b) {
    fu64 v;
    v.u = b;
    return v.d;
}

static double make(u32 sign, u32 exp, u64 frac) {
    /* `sign` is a u32 with its bit at position 31, matching f32 style;
     * moving it into the u64 result needs the << 32. */
    return double_of(((u64) sign << 32) |
                     ((u64) exp << 52) | (frac & F64_FRAC));
}

/* Canonical quiet NaN. */
static double qnan(void) {
    return double_of(0x7FF8000000000000ull);
}

static double infinity(u32 sign) {
    return double_of(((u64) sign << 32) | 0x7FF0000000000000ull);
}

/* Round `sig` (implicit bit at 55, 3 guard bits) at biased exponent `exp`
 * and pack. Identical structure to relic_softfp_round_pack32: normalize,
 * gradual underflow, round-to-nearest-even, overflow to infinity. */
double relic_softfp_round_pack64(u32 sign, int exp, u64 sig) {
    u64 round_bits;

    if (sig == 0ull)
        return make(sign, 0, 0);

    while (sig >= SIG_TOP) {
        sig = (sig >> 1) | (sig & 1ull);
        exp++;
    }
    while (sig < SIG_ONE) {
        sig <<= 1;
        exp--;
    }

    /* Gradual underflow: shift right so the result encodes with exp 0. */
    if (exp <= 0) {
        int sh = 1 - exp;
        if (sh >= 56) {
            sig = 1ull;              /* everything is sticky; rounds to 0 */
        } else {
            u64 lost = sig & ((1ull << sh) - 1ull);
            sig = (sig >> sh) | (lost != 0ull);
        }
        exp = 0;
    }

    round_bits = sig & 7ull;
    sig >>= 3;
    if (round_bits > 4ull || (round_bits == 4ull && (sig & 1ull) != 0ull))
        sig++;

    if (exp == 0) {
        /* Subnormal, unless rounding carried it up to the smallest normal
         * (sig == 2^52 exactly -> frac 0 with exponent field 1). */
        if (sig >= F64_HID)
            return make(sign, 1, sig & F64_FRAC);
        return make(sign, 0, sig);
    }

    if (sig >= (F64_HID << 1)) {
        sig >>= 1;
        exp++;
    }
    if (exp >= F64_EXPMAX)
        return infinity(sign);
    return make(sign, (u32) exp, sig & F64_FRAC);
}

/* Unpack into sign / biased exponent / 53-bit significand (implicit bit
 * included for normals). Subnormals report exp == 1 with no implicit bit,
 * matching softfp32.c's convention. */
typedef struct {
    u32 sign;
    int exp;      /* biased, adjusted to 1 for subnormals */
    u64 sig;      /* 53-bit significand, implicit bit set iff normal */
    int raw_exp;  /* untouched exponent field */
    u64 frac;
} unpacked;

static void unpack(u64 b, unpacked *o) {
    o->sign = (u32) (b >> 32) & F32_SIGN;
    o->raw_exp = (int) ((b >> 52) & 0x7FFull);
    o->frac = b & F64_FRAC;
    if (o->raw_exp == 0) {
        o->exp = 1;
        o->sig = o->frac;
    } else {
        o->exp = o->raw_exp;
        o->sig = o->frac | F64_HID;
    }
}

static int is_nan(const unpacked *v) {
    return v->raw_exp == F64_EXPMAX && v->frac != 0ull;
}

static int is_inf(const unpacked *v) {
    return v->raw_exp == F64_EXPMAX && v->frac == 0ull;
}

/* Left-align a subnormal significand so bit 52 is set, adjusting exp.
 * Only called for nonzero subnormal inputs. */
static void norm_subnormal(u64 *sig, int *exp) {
    while ((*sig & F64_HID) == 0ull) {
        *sig <<= 1;
        (*exp)--;
    }
}

/* ------------------------------------------------------------------ */
/* Addition / subtraction                                              */
/* ------------------------------------------------------------------ */

static double addsub(u64 ua, u64 ub) {
    unpacked a, b;
    int exp;
    u64 siga, sigb;
    u32 sign;

    unpack(ua, &a);
    unpack(ub, &b);

    if (is_nan(&a) || is_nan(&b))
        return qnan();

    if (is_inf(&a)) {
        if (is_inf(&b) && a.sign != b.sign)
            return qnan();
        return infinity(a.sign);
    }
    if (is_inf(&b))
        return infinity(b.sign);

    siga = a.sig << 3;
    sigb = b.sig << 3;

    /* Align the smaller exponent up, accumulating a sticky bit. */
    if (a.exp > b.exp) {
        int sh = a.exp - b.exp;
        exp = a.exp;
        if (sh > 56)
            sigb = (sigb != 0ull);
        else
            sigb = (sigb >> sh) | ((sigb & ((1ull << sh) - 1ull)) != 0ull);
    } else if (b.exp > a.exp) {
        int sh = b.exp - a.exp;
        exp = b.exp;
        if (sh > 56)
            siga = (siga != 0ull);
        else
            siga = (siga >> sh) | ((siga & ((1ull << sh) - 1ull)) != 0ull);
    } else {
        exp = a.exp;
    }

    if (a.sign == b.sign) {
        sign = a.sign;
        return round_pack(sign, exp, siga + sigb);
    }

    if (siga > sigb)
        return round_pack(a.sign, exp, siga - sigb);
    if (sigb > siga)
        return round_pack(b.sign, exp, sigb - siga);
    /* Exact cancellation yields +0 under round-to-nearest. */
    return make(0, 0, 0);
}

double __adddf3(double a, double b) {
    return addsub(bits_of(a), bits_of(b));
}

double __subdf3(double a, double b) {
    return addsub(bits_of(a), bits_of(b) ^ F64_SIGN);
}

double __negdf2(double a) {
    return double_of(bits_of(a) ^ F64_SIGN);
}

/* ------------------------------------------------------------------ */
/* Multiplication                                                      */
/* ------------------------------------------------------------------ */

double __muldf3(double a, double b) {
    unpacked x, y;
    u32 sign;
    int exp;
    u64 a1, a0, b1, b0;
    u64 p2, p1, p0, mid, hi, lo, carry;
    u64 sig;

    unpack(bits_of(a), &x);
    unpack(bits_of(b), &y);
    sign = x.sign ^ y.sign;

    if (is_nan(&x) || is_nan(&y))
        return qnan();
    if (is_inf(&x)) {
        if (y.sig == 0ull && y.raw_exp == 0)   /* inf * 0 */
            return qnan();
        return infinity(sign);
    }
    if (is_inf(&y)) {
        if (x.sig == 0ull && x.raw_exp == 0)
            return qnan();
        return infinity(sign);
    }
    if (x.sig == 0ull || y.sig == 0ull)
        return make(sign, 0, 0);

    if (x.raw_exp == 0) norm_subnormal(&x.sig, &x.exp);
    if (y.raw_exp == 0) norm_subnormal(&y.sig, &y.exp);

    /* 53x53 -> 106-bit product. Split each significand at bit 26:
     *   a = a1*2^26 + a0  (a1 < 2^27, a0 < 2^26)
     *   p = a1b1*2^52 + (a1b0 + a0b1)*2^26 + a0b0
     * and assemble a (hi:52, lo) pair in two u64s. */
    a1 = x.sig >> 26;  a0 = x.sig & ((1ull << 26) - 1ull);
    b1 = y.sig >> 26;  b0 = y.sig & ((1ull << 26) - 1ull);

    p0 = a0 * b0;                       /* < 2^52 */
    p1 = a1 * b0 + a0 * b1;             /* < 2^54 */
    p2 = a1 * b1;                       /* < 2^54 */

    mid = (p1 & ((1ull << 26) - 1ull)) << 26;
    lo = p0 + mid;                      /* < 2^53; bit 52 may carry */
    carry = lo >> 52;
    lo &= (1ull << 52) - 1ull;
    hi = p2 + (p1 >> 26) + carry;       /* < 2^55 */

    exp = x.exp + y.exp - 1023;

    /* Product leading bit is at 104 or 105 (hi bit 52 or 53). Reduce to
     * the 56-bit intermediate (implicit 1 at bit 55), folding discarded
     * bits into sticky. */
    if (hi >= (1ull << 53)) {
        /* Leading bit at 105: keep product bits [105:50], fold [49:0]
         * into sticky. hi holds product bits >= 52, so the kept window
         * is (hi << 2) | (lo >> 50) and the sticky is exactly lo's low
         * 50 bits (bits 50,51 of the product live in lo's top). */
        sig = (hi << 2) | (lo >> 50);
        if ((lo & ((1ull << 50) - 1ull)) != 0ull)
            sig |= 1ull;
        exp++;
    } else {
        /* leading at 104: product >> 49, sticky = lo's low 49 bits */
        sig = (hi << 3) | (lo >> 49);
        if ((lo & ((1ull << 49) - 1ull)) != 0ull)
            sig |= 1ull;
    }

    return round_pack(sign, exp, sig);
}

/* ------------------------------------------------------------------ */
/* Division                                                            */
/* ------------------------------------------------------------------ */

double __divdf3(double a, double b) {
    unpacked x, y;
    u32 sign;
    u64 quot, rem;
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

    if (y.sig == 0ull) {
        if (x.sig == 0ull)
            return qnan();          /* 0 / 0 */
        return infinity(sign);      /* x / 0 */
    }
    if (x.sig == 0ull)
        return make(sign, 0, 0);

    if (x.raw_exp == 0) norm_subnormal(&x.sig, &x.exp);
    if (y.raw_exp == 0) norm_subnormal(&y.sig, &y.exp);

    exp = x.exp - y.exp + 1023;

    /* Restoring shift-subtract division producing 56 quotient bits, the
     * same shape as __divsf3 but on u64: both significands in
     * [2^52, 2^53) so quot == floor((x/y) * 2^55) after 56 iterations
     * plus sticky. rem stays < 2^53 throughout (rem < y before each
     * shift, so rem<<1 < 2^54) - no 128-bit arithmetic needed. */
    rem = x.sig;
    quot = 0;
    for (i = 0; i < 56; i++) {
        quot <<= 1;
        if (rem >= y.sig) {
            rem -= y.sig;
            quot |= 1ull;
        }
        rem <<= 1;
    }
    if (rem != 0ull)
        quot |= 1ull;               /* sticky: quotient was inexact */

    return round_pack(sign, exp, quot);
}

/* ------------------------------------------------------------------ */
/* Comparisons                                                         */
/* ------------------------------------------------------------------ */

/* Ordered comparison of two non-NaN doubles: -1, 0 or +1. */
static int cmp_ordered(u64 ua, u64 ub) {
    /* +0 == -0 must compare equal. */
    if (((ua | ub) & ~F64_SIGN) == 0ull)
        return 0;
    if (ua == ub)
        return 0;
    if ((ua & F64_SIGN) != (ub & F64_SIGN))
        return (ua & F64_SIGN) ? -1 : 1;
    if ((ua & F64_SIGN) == 0ull)
        return (ua < ub) ? -1 : 1;
    return (ua > ub) ? -1 : 1;
}

static int either_nan(u64 ua, u64 ub) {
    return ((ua & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
            (ua & F64_FRAC) != 0ull) ||
           ((ub & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
            (ub & F64_FRAC) != 0ull);
}

int __unorddf2(double a, double b) {
    return either_nan(bits_of(a), bits_of(b));
}

int __eqdf2(double a, double b) {
    u64 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return 1;
    return cmp_ordered(ua, ub) != 0;
}

int __nedf2(double a, double b) {
    return __eqdf2(a, b);
}

/* lt/le: caller tests `result < 0` / `<= 0`; unordered must be positive. */
int __ltdf2(double a, double b) {
    u64 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return 1;
    return cmp_ordered(ua, ub);
}

int __ledf2(double a, double b) {
    u64 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return 1;
    return cmp_ordered(ua, ub);
}

/* gt/ge: caller tests `result > 0` / `>= 0`; unordered must be negative. */
int __gtdf2(double a, double b) {
    u64 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return -1;
    return cmp_ordered(ua, ub);
}

int __gedf2(double a, double b) {
    u64 ua = bits_of(a), ub = bits_of(b);
    if (either_nan(ua, ub))
        return -1;
    return cmp_ordered(ua, ub);
}

/* ------------------------------------------------------------------ */
/* Conversions                                                         */
/* ------------------------------------------------------------------ */

double __floatunsidf(u32 a) {
    u64 sig, t;
    int sh = 0;

    if (a == 0u)
        return make(0, 0, 0);

    /* A u32 fits in the 53-bit significand with room to spare; normalize
     * left into the intermediate form (implicit bit at 55). */
    t = (u64) a;
    while (t < SIG_ONE) {
        t <<= 1;
        sh--;
    }
    sig = t;
    /* value == a == sig * 2^-55 * 2^(exp-1023) with a ~ 2^(31+sh) scale:
     * sig = a * 2^-sh * 2^55 ... direct: exp = 1023 + 55 + sh where sh
     * counts left shifts negatively. */
    return round_pack(0, 1023 + 55 + sh, sig);
}

double __floatsidf(i32 a) {
    if (a < 0) {
        u32 mag = 0u - (u32) a;     /* unsigned negate handles INT_MIN */
        return double_of(bits_of(__floatunsidf(mag)) | F64_SIGN);
    }
    return __floatunsidf((u32) a);
}

i32 __fixdfsi(double a) {
    u64 b = bits_of(a);
    unpacked v;
    int shift;
    u64 mag;

    unpack(b, &v);
    if (is_nan(&v))
        return 0;
    if (v.exp < 1023)
        return 0;

    shift = v.exp - 1023 - 52;      /* how far to shift the 53-bit sig */
    if (shift >= 0) {
        /* sig >= 2^52, so shift >= 11 implies magnitude >= 2^63. */
        if (shift >= 11)
            return v.sign ? (-2147483647 - 1) : 2147483647;
        mag = v.sig << shift;
    } else {
        mag = v.sig >> (-shift);
    }

    if (v.sign) {
        if (mag > 0x80000000ull)
            return -2147483647L - 1;
        return (i32) (0u - (u32) mag);
    }
    if (mag > 0x7FFFFFFFull)
        return 2147483647;
    return (i32) mag;
}

u32 __fixunsdfsi(double a) {
    u64 b = bits_of(a);
    unpacked v;
    int shift;

    unpack(b, &v);
    if (is_nan(&v))
        return 0u;
    if (v.sign)                     /* negative (and -0) => 0 */
        return 0u;
    if (v.exp < 1023)
        return 0u;

    shift = v.exp - 1023 - 52;
    if (shift >= 0) {
        if (shift >= 12)            /* magnitude >= 2^64-ish: saturate */
            return 0xFFFFFFFFu;
        {
            u64 wide = v.sig << shift;
            if (wide > 0xFFFFFFFFull)
                return 0xFFFFFFFFu;
            return (u32) wide;
        }
    }
    return (u32) (v.sig >> (-shift));
}

/* 64-bit int <-> double: any translation unit with a `long long` <->
 * double conversion references these names under -msoft-float. */
double __floatundidf(unsigned long long a) {
    u64 sig;
    int bits = 0;
    u64 m;

    if (a == 0ull)
        return make(0, 0, 0);

    for (m = a; m; m >>= 1)
        bits++;                     /* bit length, 1..64 */

    if (bits <= 56) {
        sig = a << (56 - bits);     /* exact - no bits lost */
        return round_pack(0, 1023 + bits - 1, sig);
    }
    /* Keep the top 56 bits; everything below folds into sticky. */
    {
        int drop = bits - 56;
        sig = a >> drop;
        if ((a & ((1ull << drop) - 1ull)) != 0ull)
            sig |= 1ull;
        return round_pack(0, 1023 + bits - 1, sig);
    }
}

double __floatdidf(long long a) {
    if (a < 0) {
        u64 mag = 0ull - (u64) a;
        return double_of(bits_of(__floatundidf(mag)) | F64_SIGN);
    }
    return __floatundidf((u64) a);
}

long long __fixdfdi(double a) {
    u64 b = bits_of(a);
    unpacked v;
    int shift;
    u64 mag;

    unpack(b, &v);
    if (is_nan(&v))
        return 0;
    if (v.exp < 1023)
        return 0;

    shift = v.exp - 1023 - 52;
    /* sig >= 2^52 => shift >= 12 means magnitude >= 2^64: saturate. */
    if (shift >= 12)
        return v.sign ? (-9223372036854775807LL - 1) : 9223372036854775807LL;
    mag = (shift >= 0) ? (v.sig << shift) : (v.sig >> (-shift));

    if (v.sign) {
        if (mag > 0x8000000000000000ull)
            return -9223372036854775807LL - 1;
        return (long long) (0ull - mag);
    }
    if (mag > 0x7FFFFFFFFFFFFFFFull)
        return 9223372036854775807LL;
    return (long long) mag;
}

unsigned long long __fixunsdfdi(double a) {
    u64 b = bits_of(a);
    unpacked v;
    int shift;

    unpack(b, &v);
    if (is_nan(&v))
        return 0ull;
    if (v.sign)
        return 0ull;
    if (v.exp < 1023)
        return 0ull;

    shift = v.exp - 1023 - 52;
    if (shift >= 12)
        return 0xFFFFFFFFFFFFFFFFull;
    if (shift < 0)
        return v.sig >> (-shift);
    return v.sig << shift;
}

/* ------------------------------------------------------------------ */
/* f32 <-> f64 conversions                                             */
/* ------------------------------------------------------------------ */

double __extendsfdf2(float a);
float __truncdfsf2(double a);

/* f32 -> f64. Always exact: every finite float, including every subnormal,
 * is a normal double, since double's exponent range strictly contains
 * float's. No rounding is possible. */
double __extendsfdf2(float a) {
    fu32 in;
    fu64 out;
    u32 sign, frac;
    int exp;

    in.f = a;
    sign = in.u & F32_SIGN;
    exp = (int) ((in.u >> 23) & 0xFFu);
    frac = in.u & F32_FRAC;

    if (exp == 0) {
        if (frac == 0u) {
            /* Signed zero. */
            out.u = (u64) sign << 32;
            return out.d;
        }
        /* Subnormal float. Normalize it: shift the significand up until the
         * implicit bit appears, decrementing the exponent to match, then
         * encode as an ordinary (normal) double. */
        exp = 1;
        while ((frac & 0x00800000u) == 0u) {
            frac <<= 1;
            exp--;
        }
        frac &= F32_FRAC;   /* drop the now-explicit leading bit */
    } else if (exp == 0xFF) {
        /* Infinity (frac == 0) or NaN. Preserve the payload so a NaN stays
         * the same NaN, shifted into double's wider field. */
        out.u = ((u64) sign << 32) | 0x7FF0000000000000ull |
                ((u64) frac << 29);
        return out.d;
    }

    out.u = ((u64) sign << 32) |
            ((u64) (unsigned int) (exp - 127 + 1023) << 52) |
            ((u64) frac << 29);
    return out.d;
}

/* f64 -> f32, rounding to nearest with ties to even via the shared f32
 * rounding path, so this agrees exactly with the arithmetic routines. */
float __truncdfsf2(double a) {
    fu64 in;
    fu32 out;
    u32 sign32, sig32;
    u64 sig;
    int exp;

    in.d = a;
    sign32 = (u32) (in.u >> 32) & F32_SIGN;
    exp = (int) ((in.u >> 52) & 0x7FFull);
    sig = in.u & F64_FRAC;

    if (exp == 0x7FF) {
        if (sig != 0ull) {
            /* NaN: keep it quiet and carry the top of the payload across. */
            out.u = sign32 | 0x7F800000u | 0x00400000u |
                    (u32) ((sig >> 29) & 0x003FFFFFu);
        } else {
            out.u = sign32 | 0x7F800000u;
        }
        return out.f;
    }

    if (exp == 0) {
        /* Zero, or a double subnormal. The largest double subnormal is about
         * 2.2e-308, far below the smallest float subnormal (~1.4e-45), so
         * everything here underflows to a signed zero. */
        out.u = sign32;
        return out.f;
    }

    /* Reduce the 53-bit significand to the 27-bit intermediate form (implicit
     * bit at 26 plus three guard bits) that round_pack expects, folding the
     * 26 discarded bits into a sticky bit. */
    sig |= F64_HID;
    sig32 = (u32) (sig >> 26);
    if ((sig & ((1ull << 26) - 1ull)) != 0ull)
        sig32 |= 1u;

    /* Rebias: the value is sig * 2^-52 * 2^(exp-1023), and round_pack reads
     * its input as sig32 * 2^-26 * 2^(e-127). */
    return relic_softfp_round_pack32(sign32, exp - 1023 + 127, sig32);
}
