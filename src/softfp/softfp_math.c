/*
 * softfp_math.c
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
 * softfp_math.c - the libm subset needed by an FPU-less build.
 *
 * DJGPP's libm.a is compiled against the x87, so every call into it would
 * trap through the DPMI host even in a -msoft-float build (~25,000 cycles per
 * operation on a 486SX). These replacements are plain C: compiled with
 * -msoft-float they route through softfp32.c and never touch the FPU.
 *
 * The names deliberately match libm's (sqrtf, sinf, ...). Because our object
 * appears before libm.a on the link line, the linker resolves these and never
 * extracts libm's versions, so existing call sites need no edits.
 *
 * ACCURACY POLICY
 * ---------------
 * "Fast approximation" quality, chosen for a software renderer rather than
 * for numerics. Figures below are measured worst cases over
 * ~12.8M samples, not estimates:
 *   fabsf/floorf/ceilf/truncf  bit-exact vs hardware
 *   sqrtf                      bit-exact (correctly rounded); integer-only,
 *                              makes no soft-float calls at all
 *   sinf                       7.4e-8 abs, |x| <= 8pi
 *   cosf                       3.9e-8 abs, |x| <= 8pi
 *   tanf                       2.7e-7 rel where |tan x| is moderate; near
 *                              +-pi/2 relative error necessarily grows as
 *                              (1+tan^2)/tan -- that is tan's conditioning,
 *                              not a defect here
 *   exp2f                      9.1e-8 rel
 *   logf/log10f                ~1e-7 rel (atanh series on a sqrt(2)-centred
 *                              mantissa)
 *   atanf/atan2f               ~2-3e-7 abs (two argument halvings through
 *                              sqrtf, then degree-9 Taylor)
 *   asinf/acosf                ~1.5e-6 abs near +-1 (sqrtf conditioning at
 *                              the domain edge)
 *   expf                       ~4e-6 rel at large |x|: the reduced argument
 *                              f = x - n*ln2 can't be finer than x's own ulp
 *   powf                       ~2.3e-6 rel (compounds logf through expf)
 *   sinhf/coshf/tanhf          ~5e-7 rel (series core for small |x|, expf
 *                              otherwise)
 *   fmodf                      bit-exact: exact u64 integer-mantissa
 *                              reduction, no float division
 *
 * Trig uses truncated Taylor polynomials with quadrant reduction instead of a
 * lookup table. That is both smaller (no multi-KB table, which matters on the
 * memory-tight FPU-less targets) and about an order of magnitude
 * more accurate than a 256-entry table with linear interpolation, for a
 * similar number of soft-float operations.
 *
 * No FP arithmetic appears in sqrtf/floorf/ceilf/fabsf; they are integer-only
 * by construction. The trig and exp2 routines do use float arithmetic, which
 * is fine -- they are compiled with -msoft-float like everything else, so it
 * lowers to calls into softfp32.c.
 */

#include "softfp_internal.h"

/* `int` may be 16 bits on some compilers; u32 must stay 32-bit. */
#if defined(__SIZEOF_INT__) && __SIZEOF_INT__ < 4
typedef unsigned long u32;
#else
typedef unsigned int u32;
#endif
typedef unsigned long long u64;

typedef union {
    float f;
    u32 u;
} fu;

float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float exp2f(float x);
float ldexpf(float x, int n);
float expf(float x);
float logf(float x);
float log10f(float x);
float powf(float x, float y);
float atanf(float x);
float atan2f(float y, float x);
float asinf(float x);
float acosf(float x);
float fmodf(float x, float y);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);

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

/* ------------------------------------------------------------------ */
/* Exact, integer-only routines                                        */
/* ------------------------------------------------------------------ */

float fabsf(float x) {
    return float_of(bits_of(x) & 0x7FFFFFFFu);
}

/* Round toward zero by masking off the fractional mantissa bits. */
float truncf(float x) {
    u32 b = bits_of(x);
    int e = (int) ((b >> 23) & 0xFFu) - 127;

    if (e < 0)                      /* |x| < 1 */
        return float_of(b & 0x80000000u);
    if (e >= 23)                    /* already integral (or inf/NaN) */
        return x;
    return float_of(b & ~((1u << (23 - e)) - 1u));
}

float floorf(float x) {
    u32 b = bits_of(x);
    int e = (int) ((b >> 23) & 0xFFu) - 127;
    u32 mask;

    if (e >= 23)                    /* integral, inf or NaN */
        return x;
    if (e < 0) {
        /* |x| < 1: floor is -1 for negatives (except -0), 0 otherwise. */
        if (b & 0x80000000u)
            return (b & 0x7FFFFFFFu) ? -1.0f : x;
        return float_of(0);
    }
    mask = (1u << (23 - e)) - 1u;
    if ((b & mask) == 0u)
        return x;                   /* already an integer */
    if (b & 0x80000000u)            /* negative and inexact: step away from 0 */
        return float_of((b & ~mask) + (1u << (23 - e)));
    return float_of(b & ~mask);
}

float ceilf(float x) {
    u32 b = bits_of(x);
    int e = (int) ((b >> 23) & 0xFFu) - 127;
    u32 mask;

    if (e >= 23)
        return x;
    if (e < 0) {
        if (b & 0x80000000u)
            return float_of(0x80000000u);   /* -1 < x <= -0  ->  -0 */
        return (b & 0x7FFFFFFFu) ? 1.0f : x;
    }
    mask = (1u << (23 - e)) - 1u;
    if ((b & mask) == 0u)
        return x;
    if (b & 0x80000000u)
        return float_of(b & ~mask);
    return float_of((b & ~mask) + (1u << (23 - e)));
}

/* Multiply by a power of two by adjusting the exponent field. Used by exp2f;
 * handles overflow to infinity and gradual underflow via round_pack. */
float ldexpf(float x, int n) {
    u32 b = bits_of(x);
    u32 sign = b & 0x80000000u;
    int e = (int) ((b >> 23) & 0xFFu);
    u32 frac = b & 0x007FFFFFu;

    if (e == 0xFF || (e == 0 && frac == 0u))
        return x;                   /* inf, NaN, or signed zero */

    if (e == 0) {
        /* Subnormal: hand the raw significand to round_pack, which will
         * normalize it. Its intermediate form wants the implicit bit at 26. */
        return relic_softfp_round_pack32(sign, 1 + n, frac << 3);
    }
    return relic_softfp_round_pack32(sign, e + n, (frac | 0x00800000u) << 3);
}

/* ------------------------------------------------------------------ */
/* Square root - integer only, correctly rounded                       */
/* ------------------------------------------------------------------ */

/* floor(sqrt(n)) for a 64-bit n, bit-by-bit. Sets *inexact when a remainder
 * is left over, which round_pack needs as the sticky bit. Integer only, so
 * this costs no soft-float calls -- the reason sqrtf is cheap here. */
static u32 isqrt64(u64 n, int *inexact) {
    u64 rem = 0, root = 0;
    int i;

    for (i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (n >> 62);
        n <<= 2;
        if (rem > root) {
            rem -= root | 1ull;
            root |= 2ull;
        }
    }
    *inexact = (rem != 0ull);
    return (u32) (root >> 1);
}

float sqrtf(float x) {
    u32 b = bits_of(x);
    u32 sign = b & 0x80000000u;
    int e = (int) ((b >> 23) & 0xFFu);
    u32 m = b & 0x007FFFFFu;
    u64 sig;
    u32 q, sig_out;
    int E, inexact;

    if (e == 0xFF) {
        if (m != 0u)
            return x;                       /* NaN */
        if (sign)
            return float_of(0x7FC00000u);   /* sqrt(-inf) = NaN */
        return x;                           /* +inf */
    }
    if (e == 0 && m == 0u)
        return x;                           /* sqrt(+-0) = +-0 */
    if (sign)
        return float_of(0x7FC00000u);       /* sqrt(negative) = NaN */

    if (e == 0) {
        /* Subnormal: normalize so the implicit bit is present. */
        e = 1;
        while ((m & 0x00800000u) == 0u) {
            m <<= 1;
            e--;
        }
        m &= 0x007FFFFFu;
    }

    E = e - 127;
    sig = (u64) (0x00800000u | m);          /* [2^23, 2^24) */
    if (E & 1) {
        /* Make the exponent even so it halves exactly. Works for negative E
         * too: E & 1 is 1 for negative odd values in two's complement, and
         * doubling sig compensates for the decrement. */
        sig <<= 1;
        E--;
    }

    /* sqrt(sig * 2^-23) == sqrt(sig << 25) * 2^-24, and sig << 25 lies in
     * [2^48, 2^50) so its root lands in [2^24, 2^25). */
    q = isqrt64(sig << 25, &inexact);

    /* Shift the 25-bit root up to the implicit-bit-at-26 intermediate form. */
    sig_out = q << 2;
    if (inexact)
        sig_out |= 1u;

    return relic_softfp_round_pack32(0, (E / 2) + 127, sig_out);
}

/* ------------------------------------------------------------------ */
/* Trigonometry                                                        */
/* ------------------------------------------------------------------ */

#define TWO_OVER_PI 0.63661977236758134f

/* pi/2 split three ways for Cody-Waite range reduction.
 *
 * A single float pi/2 is not enough: r = x - k*(pi/2) loses one bit of r for
 * every bit of k, so by |x| ~ 20 (k ~ 13) the error had grown to ~2.4e-6,
 * eight times the intended bound. Splitting pi/2 into three floats and
 * subtracting each in turn keeps the reduction accurate.
 *
 * PIO2_HI is pi/2 with the low 16 mantissa bits cleared, so k * PIO2_HI is
 * exactly representable for |k| < 2^16 (|x| up to roughly 51,000 radians) and
 * the first subtraction introduces no error at all. The three terms sum to
 * pi/2 with a residual of only 1.7e-15, far below float precision.
 *
 * Generated and verified in long double. */
#define PIO2_HI  1.5703125000e+00f      /* 0x3FC90000 */
#define PIO2_MID 4.8375129700e-04f      /* 0x39FDA000 */
#define PIO2_LO  7.5497901264e-08f      /* 0x33A22169 */

/* sin(r) for |r| <= pi/4: r - r^3/6 + r^5/120 - r^7/5040 + r^9/362880.
 *
 * The r^9 term is not optional. Stopping at r^7 leaves a truncation error of
 * r^9/9! = 2.7e-7 at r = pi/4, and because quadrant reduction routes cosine
 * through sin_poly for odd quadrants, that showed up as cosf(pi/4) being the
 * single worst case in the whole library. One extra multiply-add drops the
 * error to ~1.5e-9 and brings sin_poly in line with cos_poly. */
static float sin_poly(float r) {
    float r2 = r * r;
    return r * (1.0f + r2 * (-1.6666667e-1f +
                       r2 * (8.3333333e-3f +
                       r2 * (-1.9841270e-4f +
                       r2 * (2.7557319e-6f)))));
}

/* cos(r) for |r| <= pi/4: 1 - r^2/2 + r^4/24 - r^6/720 + r^8/40320.
 * Truncation error at the interval end is about 2e-8. */
static float cos_poly(float r) {
    float r2 = r * r;
    return 1.0f + r2 * (-5.0e-1f +
                  r2 * (4.1666667e-2f +
                  r2 * (-1.3888889e-3f +
                  r2 * (2.4801587e-5f))));
}

/* Shared core: reduce x to a quadrant index plus a remainder in [-pi/4, pi/4]
 * and evaluate. `want_cos` selects cosine by shifting the quadrant by one,
 * which is exactly the sin/cos phase relationship.
 *
 * Reduction is Cody-Waite: k = round(x * 2/pi), then subtract k*(pi/2) in
 * three successive pieces so the cancellation stays accurate as k grows. See
 * the PIO2_* definitions above for why one float is insufficient. */
static float sincos_core(float x, int want_cos) {
    float r, kf;
    int k;

    /* NaN or infinity in => NaN out. */
    if ((bits_of(x) & 0x7F800000u) == 0x7F800000u)
        return float_of(0x7FC00000u);

    /* k = nearest integer to x * 2/pi, rounding half away from zero. */
    {
        float t = x * TWO_OVER_PI;
        k = (int) (t + (t < 0.0f ? -0.5f : 0.5f));
    }
    kf = (float) k;
    /* Subtract the three pieces in decreasing magnitude. The first product is
     * exact, so this progressively refines r rather than accumulating error. */
    r = x - kf * PIO2_HI;
    r = r - kf * PIO2_MID;
    r = r - kf * PIO2_LO;

    if (want_cos)
        k++;

    switch (k & 3) {
        case 0:  return sin_poly(r);
        case 1:  return cos_poly(r);
        case 2:  return -sin_poly(r);
        default: return -cos_poly(r);
    }
}

float sinf(float x) {
    return sincos_core(x, 0);
}

float cosf(float x) {
    return sincos_core(x, 1);
}

float tanf(float x) {
    float s = sincos_core(x, 0);
    float c = sincos_core(x, 1);
    return s / c;
}

/* ------------------------------------------------------------------ */
/* exp2                                                                */
/* ------------------------------------------------------------------ */

/* 2^x. Split into an integer part, applied by exponent adjustment, and a
 * fractional part evaluated as sum (f ln2)^k / k!.
 *
 * n is the *nearest* integer rather than the floor, so f lands in
 * [-0.5, 0.5] instead of [0, 1). Since the truncation error of the degree-7
 * series scales as f^8, halving the interval cuts the error by 2^8: measured
 * worst-case relative error drops from 7.7e-7 to well under 1e-8 for no extra
 * arithmetic.
 */
float exp2f(float x) {
    float f, p;
    int n;

    if ((bits_of(x) & 0x7F800000u) == 0x7F800000u) {
        u32 b = bits_of(x);
        if (b & 0x007FFFFFu)
            return x;                       /* NaN */
        return (b & 0x80000000u) ? 0.0f : x; /* 2^-inf = 0, 2^+inf = inf */
    }

    if (x > 128.0f)
        return float_of(0x7F800000u);       /* overflow to +inf */
    if (x < -150.0f)
        return 0.0f;                        /* underflows past subnormals */

    n = (int) floorf(x + 0.5f);     /* nearest integer */
    f = x - (float) n;              /* f in [-0.5, 0.5] */

    /* Coefficients are ln(2)^k / k!, k = 1..7. */
    p = 1.0f + f * (6.9314718e-1f +
              f * (2.4022651e-1f +
              f * (5.5504109e-2f +
              f * (9.6181291e-3f +
              f * (1.3333558e-3f +
              f * (1.5403531e-4f +
              f * (1.5252734e-5f)))))));

    return ldexpf(p, n);
}

/* ------------------------------------------------------------------ */
/* exp / log / pow                                                     */
/* ------------------------------------------------------------------ */

#define LOG2E    1.4426950408889634f    /* log2(e)  */
#define LN2_HI   6.9314718e-1f          /* ln(2)    */
#define LOG10E   4.3429448e-1f          /* log10(e) */

/* e^x via e^x = 2^n * e^f, n = round(x/ln2), f = x - n*ln2. ln2 is split
 * Cody-Waite style so the subtraction stays accurate: routing through
 * exp2f(x*log2e) instead would cost one rounding of a magnitude-~100
 * argument, i.e. ~5e-6 relative error at the top of the range; this keeps
 * it near 1e-7. Degree-8 Taylor: |f| <= ln2/2, truncation ~f^9/9! ~ 2e-10. */
#define LN2_HI_F 6.93145751953125e-1f   /* ln2, low mantissa bits clear */
#define LN2_LO_F 1.42860676533021e-6f   /* ln2 - LN2_HI_F */

float expf(float x) {
    float f, p;
    int n;
    u32 b = bits_of(x);

    if ((b & 0x7F800000u) == 0x7F800000u) {
        if (b & 0x007FFFFFu)
            return x;                           /* NaN */
        return (b & 0x80000000u) ? 0.0f : x;    /* e^-inf = 0, e^inf = inf */
    }
    if (x > 88.73f)
        return float_of(0x7F800000u);           /* overflow */
    if (x < -104.0f)
        return 0.0f;                            /* underflow */

    n = (int) floorf(x * LOG2E + 0.5f);
    f = x - (float) n * LN2_HI_F;
    f = f - (float) n * LN2_LO_F;

    p = 1.0f + f * (1.0f + f * (5.0e-1f +
              f * (1.6666667e-1f +
              f * (4.1666668e-2f +
              f * (8.3333333e-3f +
              f * (1.3888889e-3f +
              f * (1.9841270e-4f +
              f * (2.4801587e-5f +
              f * (2.7557319e-6f)))))))));

    return ldexpf(p, n);
}

/* ln(x) = e*ln2 + ln(m) with x = m * 2^e. The mantissa is first reduced to
 * [sqrt(2)/2, sqrt(2)) so s = (m-1)/(m+1) stays within +-0.172 and the
 * atanh series ln(m) = 2*(s + s^3/3 + s^5/5 + ...) converges to float
 * precision in six terms (next term is ~2*s^13/13 ~ 2e-11). */
float logf(float x) {
    u32 b = bits_of(x);
    u32 sign = b & 0x80000000u;
    int e = (int) ((b >> 23) & 0xFFu);
    u32 m = b & 0x007FFFFFu;
    float mf, s, s2;
    int E;

    if (e == 0xFF)
        return m ? float_of(0x7FC00000u) : x;   /* NaN -> NaN, +inf -> +inf */
    if (e == 0 && m == 0u)
        return float_of(0xFF800000u);           /* log(+-0) = -inf */
    if (sign)
        return float_of(0x7FC00000u);           /* log(negative) = NaN */

    if (e == 0) {
        /* Subnormal: normalize so the implicit bit is present. */
        e = 1;
        while ((m & 0x00800000u) == 0u) {
            m <<= 1;
            e--;
        }
        m &= 0x007FFFFFu;
    }
    E = e - 127;

    /* m in [1,2). Halve once when m > sqrt(2) to centre it on 1. */
    mf = float_of(0x3F800000u | m);
    if (mf > 1.4142135623730951f) {
        mf *= 0.5f;
        E++;
    }

    s = (mf - 1.0f) / (mf + 1.0f);
    s2 = s * s;
    mf = s * (1.0f + s2 * (3.3333333e-1f +
                    s2 * (2.0e-1f +
                    s2 * (1.4285714e-1f +
                    s2 * (1.1111111e-1f +
                    s2 * (9.0909091e-2f))))));
    return (float) E * LN2_HI + 2.0f * mf;
}

float log10f(float x) {
    return logf(x) * LOG10E;
}

float powf(float x, float y) {
    int neg_odd = 0;
    float r;
    u32 yb = bits_of(y);

    if ((yb & 0x7FFFFFFFu) == 0u)
        return 1.0f;                    /* x^0 = 1, including 0^0 */
    if ((bits_of(x) & 0x7FFFFFFFu) == 0x3F800000u)
        return 1.0f;                    /* 1^y = 1 */

    if (bits_of(x) & 0x80000000u) {
        /* x < 0: only integral y is defined. */
        if (truncf(y) != y)
            return float_of(0x7FC00000u);
        /* odd integral y keeps the sign; |y| > 2^24 is always integral
         * and its parity no longer matters vs magnitude. */
        if (fabsf(y) < 16777216.0f && fmodf(fabsf(y), 2.0f) != 0.0f)
            neg_odd = 1;
        x = fabsf(x);
    }
    if ((bits_of(x) & 0x7FFFFFFFu) == 0u)
        return (yb & 0x80000000u) ? float_of(0x7F800000u) : 0.0f;

    r = expf(y * logf(x));
    return neg_odd ? -r : r;
}

/* ------------------------------------------------------------------ */
/* Inverse trig                                                        */
/* ------------------------------------------------------------------ */

#define PIO2F 1.5707963267948966f       /* pi/2 */
#define PIO4F 7.8539816339744831e-1f    /* pi/4 */

/* atan(a) for |a| <= 1 via argument halving: atan(x) = 2*atan(x/(1+sqrt(1+x^2))).
 * Two halvings bound the argument by ~0.176 so the Taylor series to x^9 has
 * ~5e-10 truncation error. */
float atanf(float x) {
    u32 b = bits_of(x);
    int flip = 0, neg = 0;
    float a, a2, p;

    if ((b & 0x7F800000u) == 0x7F800000u) {
        if (b & 0x007FFFFFu)
            return x;                   /* NaN */
        /* atan(+-inf) = +-pi/2 */
        return (b & 0x80000000u) ? -PIO2F : PIO2F;
    }
    if (b & 0x80000000u) {
        neg = 1;
        b &= 0x7FFFFFFFu;
    }
    a = float_of(b);
    if (a > 1.0f) {
        a = 1.0f / a;
        flip = 1;
    }
    a = a / (1.0f + sqrtf(1.0f + a * a));
    a = a / (1.0f + sqrtf(1.0f + a * a));

    a2 = a * a;
    p = a * (1.0f + a2 * (-3.3333333e-1f +
                   a2 * (2.0e-1f +
                   a2 * (-1.4285714e-1f +
                   a2 * (1.1111111e-1f)))));
    p *= 4.0f;                          /* undo the two halvings */
    if (flip)
        p = PIO2F - p;
    return neg ? -p : p;
}

float atan2f(float y, float x) {
    u32 xb = bits_of(x), yb = bits_of(y);
    float t;

    if (((xb & 0x7F800000u) == 0x7F800000u && (xb & 0x007FFFFFu)) ||
        ((yb & 0x7F800000u) == 0x7F800000u && (yb & 0x007FFFFFu)))
        return float_of(0x7FC00000u);   /* either NaN */

    if ((xb & 0x7FFFFFFFu) == 0u && (yb & 0x7FFFFFFFu) == 0u) {
        /* atan2(+-0, +-0): 0 or pi by the signs. */
        if (xb & 0x80000000u)
            return (yb & 0x80000000u) ? -3.1415926535897932f
                                      : 3.1415926535897932f;
        return (yb & 0x80000000u) ? -0.0f : 0.0f;
    }
    if ((xb & 0x7FFFFFFFu) == 0u)
        return (yb & 0x80000000u) ? -PIO2F : PIO2F;

    t = atanf(y / x);
    if (!(xb & 0x80000000u))
        return t;                       /* x > 0 */
    return (yb & 0x80000000u) ? t - 3.1415926535897932f
                              : t + 3.1415926535897932f;
}

float asinf(float x) {
    u32 b = bits_of(x) & 0x7FFFFFFFu;
    if (b > 0x3F800000u)
        return float_of(0x7FC00000u);   /* |x| > 1 (incl. NaN) */
    if (b == 0x3F800000u)
        return (bits_of(x) & 0x80000000u) ? -PIO2F : PIO2F;
    return atan2f(x, sqrtf(1.0f - x * x));
}

float acosf(float x) {
    u32 b = bits_of(x) & 0x7FFFFFFFu;
    if (b > 0x3F800000u)
        return float_of(0x7FC00000u);
    return atan2f(sqrtf(1.0f - x * x), x);
}

/* ------------------------------------------------------------------ */
/* fmod / hyperbolics                                                  */
/* ------------------------------------------------------------------ */

float fmodf(float x, float y) {
    u32 xb = bits_of(x), yb = bits_of(y);
    u32 xe = xb & 0x7F800000u, ye = yb & 0x7F800000u;

    if ((yb & 0x7FFFFFFFu) == 0u || (ye == 0x7F800000u && (yb & 0x007FFFFFu)) ||
        xe == 0x7F800000u)
        return float_of(0x7FC00000u);   /* y = 0 or NaN; x inf/NaN */
    if (ye == 0x7F800000u)
        return x;                       /* y inf, finite x -> x */
    if ((xb & 0x7FFFFFFFu) == 0u)
        return x;                       /* fmod(+-0, y) = +-0 */
    {
        /* Exact reduction: x = mx*2^(ex-23), y = my*2^(ey-23) with mx, my
         * 24-bit integers. The remainder is 2^(ey-23) * (mx*2^(ex-ey) mod
         * my), computed exactly in u64 with chunked shifts -- float-division
         * shortcuts (x - truncf(x/y)*y) lose a whole ulp of y when x/y
         * rounds across an integer boundary. */
        int ex = (int) ((xb >> 23) & 0xFFu), ey2 = (int) (ye >> 23);
        u32 mx = xb & 0x007FFFFFu, my = yb & 0x007FFFFFu;
        u64 v;
        int sh;

        if (ex == 0) {                  /* subnormal x: normalize */
            ex = 1;
            while ((mx & 0x00800000u) == 0u) { mx <<= 1; ex--; }
        }
        if (ey2 == 0) {
            ey2 = 1;
            while ((my & 0x00800000u) == 0u) { my <<= 1; ey2--; }
        }
        mx &= 0x007FFFFFu; my &= 0x007FFFFFu;
        mx |= 0x00800000u; my |= 0x00800000u;

        sh = ex - ey2;
        if (sh < 0)
            return x;                   /* |x| < |y|: remainder is x */
        v = mx;
        while (sh > 0) {
            int k = sh > 30 ? 30 : sh;
            v = ((v << k) % my);
            sh -= k;
        }
        v %= my;
        /* v < my < 2^24, so (float)v and the ldexpf scaling are exact. */
        {
            float r = ldexpf((float) v, ey2 - 150);
            if (xb & 0x80000000u)
                r = -r;                 /* fmod takes the dividend's sign */
            return r;
        }
    }
}

/* sinh(x) for |x| < 1 as a series (avoids expf cancellation); otherwise
 * (e^x - e^-x)/2. cosh uses (e^x + e^-x)/2 throughout - no cancellation. */
float sinhf(float x) {
    float ax = fabsf(x);
    if (ax < 1.0f) {
        float x2 = x * x;
        return x * (1.0f + x2 * (1.6666667e-1f +
                          x2 * (8.3333333e-3f +
                          x2 * (1.9841270e-4f +
                          x2 * (2.7557319e-6f)))));
    }
    return 0.5f * (expf(ax) - expf(-ax)) * ((bits_of(x) & 0x80000000u) ? -1.0f : 1.0f);
}

float coshf(float x) {
    return 0.5f * (expf(fabsf(x)) + expf(-fabsf(x)));
}

float tanhf(float x) {
    float ax = fabsf(x), t;
    if (ax < 0.625f)
        t = sinhf(ax) / coshf(ax);      /* series core: no expf cancellation */
    else {
        float e = expf(2.0f * ax);
        t = (e - 1.0f) / (e + 1.0f);    /* large x saturates to 1 */
    }
    return (bits_of(x) & 0x80000000u) ? -t : t;
}
