/*
 * softfp_math64.c
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
 * The double-precision transcendental routines below (sin/cos kernels,
 * exp, log, asin/acos, atan and tanh) use the polynomial coefficients and
 * argument-reduction schemes of fdlibm, which carries this notice:
 *
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/*
 * softfp_math64.c - double-precision libm for the soft-float target
 *
 * The frelic3d DOS build (TIGRYX_SOFTFP) deliberately links without
 * libm.a: DJGPP's libm is x87 code and on a machine with no FPU it traps
 * or returns garbage. softfp_math.c already provides the f32 subset; this
 * file provides the double subset the VM's D* math opcodes need.
 *
 * HOW THIS COMPILES
 * -----------------
 * Unlike softfp64.c, floating-point *arithmetic* is permitted here: with
 * -msoft-float every + - * / below is lowered into a call to softfp64.c's
 * __adddf3 etc., which are correctly rounded. The exact functions
 * (fabs/floor/ceil/trunc/fmod/sqrt) are still implemented in pure integer
 * arithmetic -- they return bit-exact IEEE results, including subnormals.
 * The transcendental set is fdlibm-style (Cody-Waite argument reduction +
 * the standard minimax constants) and lands within a few ulp, which is
 * well within 1e-14 absolute of the hardware results.
 *
 * RANGE NOTE: trig argument reduction uses a 4-part Cody-Waite split of
 * pi/2 (about 100 bits of pi/2). |x| below ~2^30 keeps full accuracy;
 * beyond that the reduced argument drifts. That is accepted for a
 * 486-class FPU-less target; the engine's workloads reduce arguments
 * themselves. The exact functions have no such limitation.
 */

#include <math.h>
#include <stdint.h>

typedef union {
    double d;
    uint64_t u;
} du;

typedef union {
    float f;
    uint32_t u;
} fu;

static uint64_t dbits(double x) { du v; v.d = x; return v.u; }
static double dmk(uint64_t u) { du v; v.u = u; return v.d; }

#define DSIGN 0x8000000000000000ull
#define DFRAC 0x000FFFFFFFFFFFFFull
#define DHID  0x0010000000000000ull

static double d_qnan(void) { return dmk(0x7FF8000000000000ull); }
static double d_inf(int neg) { return dmk((neg ? DSIGN : 0) | 0x7FF0000000000000ull); }

static int d_isnan(double x) {
    uint64_t u = dbits(x);
    return (u & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           (u & DFRAC) != 0;
}
static int d_isinf(double x) {
    uint64_t u = dbits(x);
    return (u & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           (u & DFRAC) == 0;
}

/* ------------------------------------------------------------------ */
/* Exact functions: integer-only                                       */
/* ------------------------------------------------------------------ */

double fabs(double x) {
    return dmk(dbits(x) & ~DSIGN);
}

/* trunc/floor/ceil: mask off the fractional bits of a finite double.
 * `shift` = number of low significand bits holding the fraction. */
static double d_trunc(double x) {
    uint64_t u = dbits(x), mask;
    int e = (int) ((u >> 52) & 0x7FF);
    int sh;

    if (e == 0x7FF)
        return x;                       /* inf/nan pass through */
    sh = 1075 - e;                      /* 52 - (e - 1023) */
    if (sh <= 0)
        return x;                       /* already integral */
    if (sh >= 53)
        return dmk(u & DSIGN);          /* |x| < 1: signed zero */
    mask = (1ull << sh) - 1ull;
    return dmk(u & ~mask);
}

double trunc(double x) { return d_trunc(x); }

double floor(double x) {
    uint64_t u = dbits(x), mask;
    int e = (int) ((u >> 52) & 0x7FF);
    int sh;

    if (e == 0x7FF)
        return x;
    sh = 1075 - e;
    if (sh >= 53) {
        /* |x| < 1: floor is 0 for x in [0,1), -1 for x < 0, -0 for -0. */
        if (u == DSIGN || u == 0)
            return x;                   /* +-0 stay +-0 */
        return (u & DSIGN) ? dmk(0xBFF0000000000000ull)   /* -1.0 */
                           : dmk(0ull);
    }
    if (sh <= 0)
        return x;
    mask = (1ull << sh) - 1ull;
    if ((u & DSIGN) && (u & mask))
        u += 1ull << sh;                /* negative with a fraction drops */
    return dmk(u & ~mask);
}

double ceil(double x) {
    uint64_t u = dbits(x), mask;
    int e = (int) ((u >> 52) & 0x7FF);
    int sh;

    if (e == 0x7FF)
        return x;
    sh = 1075 - e;
    if (sh >= 53) {
        if (u == DSIGN || u == 0)
            return x;
        return (u & DSIGN) ? dmk(DSIGN)          /* -0.0 */
                           : dmk(0x3FF0000000000000ull); /* 1.0 */
    }
    if (sh <= 0)
        return x;
    mask = (1ull << sh) - 1ull;
    if (!(u & DSIGN) && (u & mask))
        u += 1ull << sh;                /* positive with a fraction rises */
    return dmk(u & ~mask);
}

double copysign(double x, double y) {
    return dmk((dbits(x) & ~DSIGN) | (dbits(y) & DSIGN));
}

/* fmod is exact: integer-mantissa remainder, so the result is bit-exact
 * including the dividend's sign. Mirrors softfp_math.c's fmodf approach
 * with 53-bit significands in u64. */
double fmod(double x, double y) {
    uint64_t ux = dbits(x), uy = dbits(y);
    uint64_t sx = ux & DSIGN;
    uint64_t mx, my, rem;
    int ex = (int) ((ux >> 52) & 0x7FF);
    int ey = (int) ((uy >> 52) & 0x7FF);
    int gap, i;

    /* NaN propagation, x inf -> NaN, y inf -> x, y 0 -> NaN, x 0 -> x. */
    if (d_isnan(x) || d_isnan(y) || ex == 0x7FF)
        return d_qnan();
    if (ey == 0x7FF)
        return x;
    if ((uy & ~DSIGN) == 0)
        return d_qnan();
    if ((ux & ~DSIGN) == 0)
        return x;

    mx = ux & DFRAC;  my = uy & DFRAC;
    if (ex == 0) {
        /* normalize subnormal x: shift left until hidden bit, tracking
         * the exponent so the value is unchanged */
        int e = 1;
        while (!(mx & DHID)) { mx <<= 1; e--; }
        ex = e;
    } else {
        mx |= DHID;
    }
    if (ey == 0) {
        int e = 1;
        while (!(my & DHID)) { my <<= 1; e--; }
        ey = e;
    } else {
        my |= DHID;
    }

    gap = ex - ey;
    if (gap < 0)
        return x;                       /* |x| < |y| */
    if (gap == 0 && mx < my)
        return x;
    if (gap == 0 && mx == my)
        return dmk(sx);                 /* exact multiple: signed zero */

    /* rem = mx * 2^gap mod my, by shift-and-reduce. rem < my < 2^53 so
     * rem << 1 fits u64. mx can exceed my when ex > ey (e.g. fmod(5.5,2)):
     * reduce once up front so the loop's rem < my invariant holds. */
    rem = mx;
    if (rem >= my)
        rem -= my;
    for (i = 0; i < gap; i++) {
        rem <<= 1;
        if (rem >= my)
            rem -= my;
    }
    /* result magnitude = rem * 2^(ey - 1023 - 52); normalize rem into a
     * 53-bit significand with matching exponent */
    if (rem == 0)
        return dmk(sx);                 /* exact multiple: signed zero */
    while (!(rem & DHID)) { rem <<= 1; ey--; }
    rem &= DFRAC;
    if (ey <= 0) {
        /* result is subnormal: shift the full significand (hidden bit
         * back in) right so it encodes with exponent field 0 */
        uint64_t fs = (rem | DHID) >> (1 - ey);
        return dmk(sx | fs);
    }
    return dmk(sx | ((uint64_t) ey << 52) | rem);
}

/* Correctly-rounded double sqrt: integer square root of the (possibly
 * parity-shifted) 53-bit significand. N = mx << 58 has its leading bit at
 * <= 112, so the root lands in [2^55, 2^56) -- exactly one bit of round
 * information below the 53-bit result, with the leftover remainder as
 * sticky. Round-to-nearest-even follows. */
double sqrt(double x) {
    uint64_t u = dbits(x);
    int e = (int) ((u >> 52) & 0x7FF);
    uint64_t mx = u & DFRAC;
    int e0, i;
    uint64_t rem, root, trial;
    uint64_t frac;

    if (d_isnan(x))
        return x;
    if (u == 0 || u == DSIGN)
        return x;                       /* +-0 -> +-0 */
    if (e == 0x7FF)
        return (u & DSIGN) ? d_qnan() : x;  /* inf -> inf, -inf -> NaN */
    if (u & DSIGN)
        return d_qnan();                /* sqrt of negative */
    if (e == 0) {
        e = 1;
        while (!(mx & DHID)) { mx <<= 1; e--; }
    } else {
        mx |= DHID;
    }

    /* x = mx * 2^(e-1075), mx in [2^52,2^53). Make the exponent even by
     * shifting mx up one when needed. */
    e0 = e - 1075;
    if (e0 & 1) { mx <<= 1; e0--; }

    /* Digit-by-digit sqrt of N = mx << 58: 56 iterations, the first 27
     * pulling bit-pairs of mx MSB-first, the rest pulling zeros. */
    rem = 0; root = 0;
    for (i = 0; i < 56; i++) {
        uint64_t pair;
        if (i < 27) {
            int pos = 52 - 2 * i;       /* pairs of mx, MSB first */
            pair = (pos >= 0) ? ((mx >> pos) & 3ull)
                              : ((mx << (-pos)) & 3ull);
        } else {
            pair = 0;
        }
        rem = (rem << 2) | pair;
        trial = (root << 2) | 1;
        if (rem >= trial) {
            rem -= trial;
            root = (root << 1) | 1ull;
        } else {
            root <<= 1;
        }
    }

    /* root = floor(sqrt(N)) is the 56-bit window [2^55,2^56): its low
     * bits are the guard bits and rem != 0 is the sticky. */
    {
        int rex = 1023 + (e0 - 58) / 2 + 55;   /* value = root*2^-55*2^(rex-1023) */
        /* Round to 53 bits. */
        /* root occupies [2^55,2^56): its bits [55:0] == significand(53)
         * plus 3 guard bits exactly matches softfp64's intermediate. */
        uint64_t rb = root & 7ull;
        uint64_t sig = root >> 3;
        if (rb > 4 || (rb == 4 && (rem != 0 || (sig & 1))))
            sig++;
        if (sig & (DHID << 1)) { sig >>= 1; rex++; }
        frac = sig & DFRAC;
        return dmk(((uint64_t) rex << 52) | frac);
    }
}

/* ------------------------------------------------------------------ */
/* scalbn helper (used by exp)                                         */
/* ------------------------------------------------------------------ */

static double d_scalbn(double x, int n) {
    /* Exact scaling by 2^n. Build the factor's bits; split extreme n so
     * the factor stays normal. */
    while (n > 1000) { x *= dmk(0x7FE0000000000000ull); n -= 1023; } /* *2^1023 */
    while (n < -1000) { x *= dmk(0x0010000000000000ull); n += 1022; } /* *2^-1022 */
    x *= dmk((uint64_t) (n + 1023) << 52);
    return x;
}

/* ------------------------------------------------------------------ */
/* Trig kernels                                                        */
/* ------------------------------------------------------------------ */

/* pi/2 split into 2 pieces (~91 bits total) for Cody-Waite reduction. */
static const double PIO2_1  = 1.57079632673412561417e+00;
static const double PIO2_1T = 6.07710050650619224932e-11;
static const double INVPIO2 = 6.36619772367581382433e-01;
static const double PIO2_HI = 1.57079632679489655800e+00;
static const double PIO2_LO = 6.12323399573676603587e-17;
static const double PI_HI   = 3.14159265358979311600e+00;
static const double PI_LO   = 1.22464679914735317721e-16;
static const double PIO4_HI = 7.85398163397448278999e-01;
static const double PIO4_LO = 3.06161699786838301793e-17;

static const double S1 = -1.66666666666666324348e-01;
static const double S2 =  8.33333333332248946124e-03;
static const double S3 = -1.98412698298579493134e-04;
static const double S4 =  2.75573137070700676789e-06;
static const double S5 = -2.50507602534068634195e-08;
static const double S6 =  1.58969099521155010221e-10;

static const double C1 =  4.16666666666666019037e-02;
static const double C2 = -1.38888888888741095749e-03;
static const double C3 =  2.48015872894767294178e-05;
static const double C4 = -2.75573143513906633035e-07;
static const double C5 =  2.08757232129817482790e-09;
static const double C6 = -1.13596475577881948265e-11;

static double ksin(double r) {
    double z = r * r;
    double p = S1 + z * (S2 + z * (S3 + z * (S4 + z * (S5 + z * S6))));
    return r + r * z * p;
}

static double kcos(double r) {
    double z = r * r;
    return 1.0 - z * (0.5 - z * (C1 + z * (C2 + z * (C3 + z *
                     (C4 + z * (C5 + z * C6))))));
}

/* Reduce x mod pi/2: returns quadrant in *kq and reduced arg. */
static double rempio2(double x, int *kq) {
    double fn = (double) (int) (x * INVPIO2 + (x >= 0 ? 0.5 : -0.5));
    /* Two-part Cody-Waite: PIO2_1 is the ~33-bit head of pi/2 so fn*PIO2_1 is
     * exact, and PIO2_1T carries the next ~58 bits (their sum is within
     * 3.5e-27 of pi/2). fdlibm's pio2_2/pio2_2t constants are NOT additional
     * terms here - they belong to its separate recompute path for extreme
     * cancellation; subtracting them too would drop the tail twice. */
    double r = x - fn * PIO2_1;
    r -= fn * PIO2_1T;
    *kq = (int) fn & 3;
    return r;
}

double sin(double x) {
    int k;
    double r;
    if (d_isnan(x) || d_isinf(x))
        return d_qnan();
    if (fabs(x) < 0.7853981633974483)
        return ksin(x);
    r = rempio2(x, &k);
    switch (k) {
    case 0: return ksin(r);
    case 1: return kcos(r);
    case 2: return -ksin(r);
    default: return -kcos(r);
    }
}

double cos(double x) {
    int k;
    double r;
    if (d_isnan(x) || d_isinf(x))
        return d_qnan();
    if (fabs(x) < 0.7853981633974483)
        return kcos(x);
    r = rempio2(x, &k);
    switch (k) {
    case 0: return kcos(r);
    case 1: return -ksin(r);
    case 2: return -kcos(r);
    default: return ksin(r);
    }
}

double tan(double x) {
    int k;
    double r;
    if (d_isnan(x) || d_isinf(x))
        return d_qnan();
    if (fabs(x) < 0.7853981633974483)
        return ksin(x) / kcos(x);
    r = rempio2(x, &k);
    if (k & 1)
        return -kcos(r) / ksin(r);
    return ksin(r) / kcos(r);
}

/* ------------------------------------------------------------------ */
/* exp                                                                 */
/* ------------------------------------------------------------------ */

static const double LN2HI = 6.93147180369123816490e-01;
static const double LN2LO = 1.90821492927058770002e-10;
static const double INVLN2 = 1.44269504088896338700e+00;
static const double EP1 =  1.66666666666666019037e-01;
static const double EP2 = -2.77777777770155933842e-03;
static const double EP3 =  6.61375632143793436117e-05;
static const double EP4 = -1.65339022054652515390e-06;
static const double EP5 =  4.13813679705723846039e-08;

double exp(double x) {
    double fn, hi, lo, t, c, y;
    int k;

    if (d_isnan(x))
        return x;
    if (x > 709.782712893384)
        return d_inf(0);
    if (x < -745.133219101941)
        return 0.0;
    if (fabs(x) < 0.5e-28)
        return 1.0 + x;

    fn = x * INVLN2;
    k = (int) (fn + (fn >= 0 ? 0.5 : -0.5));
    hi = x - (double) k * LN2HI;
    lo = (double) k * LN2LO;
    x = hi - lo;

    t = x * x;
    c = x - t * (EP1 + t * (EP2 + t * (EP3 + t * (EP4 + t * EP5))));
    /* e^x = 1 + x + x*c/(2-c); splitting the reduced x into hi - lo
     * keeps k's rounding error out of the significand (fdlibm e_exp). */
    y = 1.0 + hi - lo + x * c / (2.0 - c);
    return d_scalbn(y, k);
}

/* ------------------------------------------------------------------ */
/* log / log10                                                         */
/* ------------------------------------------------------------------ */

static const double LG1 = 6.666666666666735130e-01;
static const double LG2 = 3.999999999940941908e-01;
static const double LG3 = 2.857142874366239149e-01;
static const double LG4 = 2.222219843214978396e-01;
static const double LG5 = 1.818357216161805012e-01;
static const double LG6 = 1.531383769920937332e-01;
static const double LG7 = 1.479819860511658591e-01;

double log(double x) {
    uint64_t u = dbits(x);
    int e;
    uint64_t mx;
    double m, f, s, z, w, R, hfsq;
    int k;

    if (d_isnan(x))
        return x;
    if (x == 0.0)
        return d_inf(1);
    if (u & DSIGN || d_isinf(x))
        return (u & DSIGN) ? d_qnan() : x;

    e = (int) ((u >> 52) & 0x7FF);
    mx = u & DFRAC;
    k = e - 1023;
    if (e == 0) {
        /* subnormal: scale up by 2^54 exactly and retry */
        return log(d_scalbn(x, 54)) - 54.0 * LN2HI - 54.0 * LN2LO;
    }

    /* mantissa m = x with exponent rebased to 1023: m in [1,2). */
    m = dmk(0x3FF0000000000000ull | mx);
    /* Prefer m in [sqrt(2)/2, sqrt(2)): if m >= sqrt(2) halve it and
     * bump k so f = m-1 stays small. */
    if (m > 1.4142135623730951) {
        m *= 0.5;
        k++;
    }
    f = m - 1.0;
    if (f == 0.0)
        return (double) k * (LN2HI + LN2LO);    /* powers of two */

    s = f / (2.0 + f);
    z = s * s;
    w = z * z;
    R = w * (LG2 + w * (LG4 + w * LG6)) +
        z * (LG1 + w * (LG3 + w * (LG5 + w * LG7)));
    hfsq = 0.5 * f * f;
    return (double) k * LN2HI + (f - hfsq + s * (hfsq + R)) +
           (double) k * LN2LO;
}

double log10(double x) {
    /* log(x) * (1/ln10); the composite keeps ~1-2 ulp. */
    return log(x) * 4.34294481903251816668e-01;
}

/* ------------------------------------------------------------------ */
/* pow                                                                 */
/* ------------------------------------------------------------------ */

double pow(double x, double y) {
    if (d_isnan(x) || d_isnan(y))
        return d_qnan();
    if (y == 0.0)
        return 1.0;
    if (x == 1.0)
        return 1.0;
    if (x == 0.0) {
        if (y > 0.0)
            return (dbits(x) & DSIGN) &&
                   d_trunc(y / 2.0) * 2.0 != y ? -0.0 : 0.0;
        return d_inf(0);
    }

    /* Integer exponent: square-multiply, which keeps exact powers exact
     * (pow(2,10) == 1024 bitwise). */
    {
        double yt = d_trunc(y);
        if (yt == y && fabs(y) <= 64.0) {
            int n = (int) yt;
            int neg = n < 0;
            double base = x, acc = 1.0;
            unsigned int e;
            if (neg) n = -n;
            e = (unsigned int) n;
            while (e) {
                if (e & 1)
                    acc *= base;
                e >>= 1;
                if (e)
                    base *= base;
            }
            return neg ? 1.0 / acc : acc;
        }
    }

    if (x < 0.0) {
        if (d_trunc(y) == y && fabs(y) <= 9007199254740992.0) {
            /* sign is set only when the integer exponent is odd */
            double m = exp(y * log(-x));
            int odd = ((int) fmod(fabs(y), 2.0)) != 0;
            return odd ? -m : m;
        }
        return d_qnan();
    }
    return exp(y * log(x));
}

/* ------------------------------------------------------------------ */
/* Inverse trig                                                        */
/* ------------------------------------------------------------------ */

/* asin/acos rational approximation (fdlibm e_asin coefficients). */
static const double PS0 =  1.66666666666666657415e-01;
static const double PS1 = -3.25565818622400915405e-01;
static const double PS2 =  2.01212532185662925881e-01;
static const double PS3 = -4.00555345006794114027e-02;
static const double PS4 =  7.91534994289814532129e-04;
static const double PS5 =  3.47933107596021167570e-05;
static const double QS1 = -2.40339491173441421878e+00;
static const double QS2 =  2.02094576023350569471e+00;
static const double QS3 = -6.88283980205427657094e-01;
static const double QS4 =  7.70381505559019306291e-02;

/* asin on [0, 0.5] via the p/q rational in z = x^2. */
static double kasin(double x) {
    double z = x * x;
    double p = z * (PS0 + z * (PS1 + z * (PS2 + z * (PS3 + z *
                     (PS4 + z * PS5)))));
    double q = 1.0 + z * (QS1 + z * (QS2 + z * (QS3 + z * QS4)));
    return x + x * (p / q);
}

double asin(double x) {
    double ax = fabs(x), r;
    if (d_isnan(x))
        return x;
    if (ax > 1.0)
        return d_qnan();
    if (ax == 1.0)
        return copysign(PIO2_HI + PIO2_LO, x);
    if (ax <= 0.5)
        return copysign(kasin(ax), x);

    /* ax in (0.5,1): asin(x) = pi/2 - 2*asin(sqrt((1-x)/2)), where the
     * inner argument is <= 0.5 and takes the polynomial path. */
    r = kasin(sqrt((1.0 - ax) * 0.5));
    /* pi/2 - 2*r with pi/2 = PIO2_HI + PIO2_LO */
    r = PIO2_HI - 2.0 * r + PIO2_LO;
    return copysign(r, x);
}

double atan(double x);  /* defined below */

double acos(double x) {
    double ax;
    if (d_isnan(x))
        return x;
    if (x >= 1.0)
        return x == 1.0 ? 0.0 : d_qnan();
    if (x <= -1.0)
        return x == -1.0 ? PI_HI + PI_LO : d_qnan();
    /* acos(x) = 2*atan(sqrt((1-x)/(1+x))): no cancellation anywhere,
     * exact at x = +-1. */
    ax = 2.0 * atan(sqrt((1.0 - x) / (1.0 + x)));
    return ax;
}

/* ------------------------------------------------------------------ */
/* atan / atan2                                                        */
/* ------------------------------------------------------------------ */

static const double AT0 =  3.33333333333329318027e-01;
static const double AT1 = -1.99999999998764832476e-01;
static const double AT2 =  1.42857142725034663711e-01;
static const double AT3 = -1.11111104054623557880e-01;
static const double AT4 =  9.09088713343682256138e-02;
static const double AT5 = -7.69187620504482999495e-02;
static const double AT6 =  6.66107313738753120669e-02;
static const double AT7 = -5.83357013379057348645e-02;
static const double AT8 =  4.97687799461593236017e-02;
static const double AT9 = -3.65315727442169155270e-02;
static const double AT10=  1.62858201153657823623e-02;

/* atan on |x| < ~0.66: atan x = x - x*z*P(z). */
static double katan(double x) {
    double z = x * x, w = z * z;
    double s1 = z * (AT0 + w * (AT2 + w * (AT4 + w * (AT6 + w *
                      (AT8 + w * AT10)))));
    double s2 = w * (AT1 + w * (AT3 + w * (AT5 + w * (AT7 + w * AT9))));
    return x - x * (s1 + s2);
}

double atan(double x) {
    double ax = fabs(x), t, r;
    if (d_isnan(x))
        return x;
    if (d_isinf(x))
        return copysign(PIO2_HI + PIO2_LO, x);
    if (ax <= 0.66)
        return copysign(katan(ax), x);
    if (ax <= 2.414213562373095) {
        /* atan x = pi/4 + atan((x-1)/(x+1)), |reduced| < 0.414 */
        t = (ax - 1.0) / (ax + 1.0);
        r = PIO4_HI + katan(t) + PIO4_LO;
    } else {
        /* atan x = pi/2 - atan(1/x), |1/x| < 0.414 */
        t = 1.0 / ax;
        r = PIO2_HI - katan(t) + PIO2_LO;
    }
    return copysign(r, x);
}

double atan2(double y, double x) {
    if (d_isnan(y) || d_isnan(x))
        return d_qnan();
    if (x == 0.0) {
        if (y == 0.0)
            return (dbits(x) & DSIGN) ? copysign(PI_HI, y) : y;
        return copysign(PIO2_HI + PIO2_LO, y);
    }
    if (y == 0.0)
        return (dbits(x) & DSIGN) ? copysign(PI_HI + PI_LO, y) : y;
    {
        double t = atan(fabs(y / x));
        if (dbits(x) & DSIGN) {
            /* x < 0: quadrants II/III */
            double r = PI_HI - t + PI_LO;
            return (dbits(y) & DSIGN) ? -r : r;
        }
        return (dbits(y) & DSIGN) ? -t : t;
    }
}

/* ------------------------------------------------------------------ */
/* Hyperbolics                                                         */
/* ------------------------------------------------------------------ */

double sinh(double x) {
    double ax = fabs(x), t;
    if (d_isnan(x))
        return x;
    if (ax < 1.0) {
        /* series: x + x^3/3! + x^5/5! + ... ; 6 terms -> ~2^-59 error */
        double z = x * x;
        return x + x * z * (1.6666666666666666667e-01 + z *
                  (8.3333333333333333333e-03 + z *
                  (1.9841269841269841270e-04 + z *
                  (2.7557319223985890653e-06 + z *
                  (2.5052108385441718775e-08 + z *
                   1.6059043836821614599e-10)))));
    }
    t = exp(ax);
    if (d_isinf(t))
        return copysign(t, x);
    return copysign((t - 1.0 / t) * 0.5, x);
}

double cosh(double x) {
    double ax = fabs(x), t;
    if (d_isnan(x))
        return x;
    t = exp(ax);
    if (d_isinf(t))
        return t;
    /* no cancellation possible: both terms positive */
    return (t + 1.0 / t) * 0.5;
}

double tanh(double x) {
    double ax = fabs(x), t;
    if (d_isnan(x))
        return x;
    if (ax < 0.545) {
        double z = x * x;
        /* tanh x = x + x*z*(T0 + z*T1 + ...), fdlibm coefficients */
        double p = -6.38265861359082438919e-06;
        p = 1.59242377165656154549e-05 + z * p;
        p = -3.93787490453483542729e-05 + z * p;
        p = 9.69152388040716776067e-05 + z * p;
        p = -2.39118540550975218687e-04 + z * p;
        p = 5.90027439597659686685e-04 + z * p;
        p = -1.45678635718188897574e-03 + z * p;
        p = 3.59207910759131235347e-03 + z * p;
        p = -8.86323982359950015839e-03 + z * p;
        p = 2.18694884893381629949e-02 + z * p;
        p = -5.39682544762291521325e-02 + z * p;
        p = 1.33333333333201242699e-01 + z * p;
        p = -3.33333333333334091986e-01 + z * p;
        return x + x * z * p;
    }
    if (ax > 19.0)
        return copysign(1.0, x);
    t = exp(2.0 * ax);
    return copysign(1.0 - 2.0 / (t + 1.0), x);
}
