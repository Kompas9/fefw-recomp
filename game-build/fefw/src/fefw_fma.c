/* SPDX-License-Identifier: MIT
 * fma and fmaf for the compatible build of fefw.exe (processors without FMA3: tools/build/fefw_build.py, variant "compat").
 *
 * The guest's FMADD / FMLA family is fused: one rounding. The lifted code calls std::fma / fmaf for it (recomp/include/nxrecomp
 * fpexact.h, cxx.h, neon.h); with FMA3 that is one vfmadd instruction, without it a call into the C library. The fma that the
 * bundled Zig links by default (its compiler_rt port of an older FreeBSD fma) is not exact: a product more than about 2^1024 below
 * the addend (fma(0x1p-828, 0x1p-392, 0x1p+1017)) comes back as a NaN, and its fmaf leaves the double-rounding case as a TODO. These
 * two are musl's (src/math/fma.c, rewritten 2017 by Szabolcs Nagy, and src/math/fmaf.c), which are correctly rounded in every case,
 * so the compatible build computes bit for bit what the FMA3 build does. Linked into fefw.exe ahead of the libraries, so these win.
 *
 * musl: Copyright (c) 2005-2020 Rich Felker, et al. MIT license: https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT
 * Compile without contraction (-ffp-contract=off): the code relies on every operation rounding on its own. */
#include <fenv.h>
#include <float.h>
#include <stdint.h>

static inline uint64_t fefw_asuint64(double x) {
    union { double f; uint64_t i; } u = {x};
    return u.i;
}
static inline double fefw_asdouble(uint64_t i) {
    union { uint64_t i; double f; } u = {i};
    return u.f;
}

/* musl src/math/scalbn.c */
static double fefw_scalbn(double x, int n) {
    double y = x;
    if (n > 1023) {
        y *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            y *= 0x1p1023;
            n -= 1023;
            if (n > 1023) n = 1023;
        }
    } else if (n < -1022) {
        /* make sure final n < -53 to avoid double rounding in the subnormal range */
        y *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            y *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022) n = -1022;
        }
    }
    return y * fefw_asdouble((uint64_t)(0x3ff + n) << 52);
}

#define ZEROINFNAN (0x7ff - 0x3ff - 52 - 1)

struct fefw_num {
    uint64_t m;
    int e;
    int sign;
};

static struct fefw_num fefw_normalize(double x) {
    uint64_t ix = fefw_asuint64(x);
    int e = (int)(ix >> 52);
    int sign = e & 0x800;
    e &= 0x7ff;
    if (!e) {
        ix = fefw_asuint64(x * 0x1p63);
        e = (int)(ix >> 52 & 0x7ff);
        e = e ? e - 63 : 0x800;
    }
    ix &= (1ull << 52) - 1;
    ix |= 1ull << 52;
    ix <<= 1;
    e -= 0x3ff + 52 + 1;
    struct fefw_num r = {ix, e, sign};
    return r;
}

static void fefw_mul(uint64_t* hi, uint64_t* lo, uint64_t x, uint64_t y) {
    uint64_t t1, t2, t3;
    uint64_t xlo = (uint32_t)x, xhi = x >> 32;
    uint64_t ylo = (uint32_t)y, yhi = y >> 32;
    t1 = xlo * ylo;
    t2 = xlo * yhi + xhi * ylo;
    t3 = xhi * yhi;
    *lo = t1 + (t2 << 32);
    *hi = t3 + (t2 >> 32) + (t1 > *lo);
}

double fma(double x, double y, double z) {
    /* normalize so top 10bits and last bit are 0 */
    struct fefw_num nx = fefw_normalize(x), ny = fefw_normalize(y), nz = fefw_normalize(z);

    if (nx.e >= ZEROINFNAN || ny.e >= ZEROINFNAN) return x * y + z;
    if (nz.e >= ZEROINFNAN) {
        if (nz.e > ZEROINFNAN) /* z==0 */
            return x * y + z;
        return z;
    }

    /* mul: r = x*y */
    uint64_t rhi, rlo, zhi, zlo;
    fefw_mul(&rhi, &rlo, nx.m, ny.m);
    /* either top 20 or 21 bits of rhi and last 2 bits of rlo are 0 */

    /* align exponents */
    int e = nx.e + ny.e;
    int d = nz.e - e;
    /* shift bits z<<=kz, r>>=kr, so kz+kr == d, set e = e+kr (== ez-kz) */
    if (d > 0) {
        if (d < 64) {
            zlo = nz.m << d;
            zhi = nz.m >> (64 - d);
        } else {
            zlo = 0;
            zhi = nz.m;
            e = nz.e - 64;
            d -= 64;
            if (d == 0) {
            } else if (d < 64) {
                rlo = rhi << (64 - d) | rlo >> d | !!(rlo << (64 - d));
                rhi = rhi >> d;
            } else {
                rlo = 1;
                rhi = 0;
            }
        }
    } else {
        zhi = 0;
        d = -d;
        if (d == 0) {
            zlo = nz.m;
        } else if (d < 64) {
            zlo = nz.m >> d | !!(nz.m << (64 - d));
        } else {
            zlo = 1;
        }
    }

    /* add */
    int sign = nx.sign ^ ny.sign;
    int samesign = !(sign ^ nz.sign);
    int nonzero = 1;
    if (samesign) {
        /* r += z */
        rlo += zlo;
        rhi += zhi + (rlo < zlo);
    } else {
        /* r -= z */
        uint64_t t = rlo;
        rlo -= zlo;
        rhi = rhi - zhi - (t < rlo);
        if (rhi >> 63) {
            rlo = -rlo;
            rhi = -rhi - !!rlo;
            sign = !sign;
        }
        nonzero = !!rhi;
    }

    /* set rhi to top 63bit of the result (last bit is sticky) */
    if (nonzero) {
        e += 64;
        d = __builtin_clzll(rhi) - 1;
        /* note: d > 0 */
        rhi = rhi << d | rlo >> (64 - d) | !!(rlo << d);
    } else if (rlo) {
        d = __builtin_clzll(rlo) - 1;
        if (d < 0)
            rhi = rlo >> 1 | (rlo & 1);
        else
            rhi = rlo << d;
    } else {
        /* exact +-0 */
        return x * y + z;
    }
    e -= d;

    /* convert to double */
    int64_t i = (int64_t)rhi; /* i is in [1<<62,(1<<63)-1] */
    if (sign) i = -i;
    double r = (double)i; /* |r| is in [0x1p62,0x1p63] */

    if (e < -1022 - 62) {
        /* result is subnormal before rounding */
        if (e == -1022 - 63) {
            double c = 0x1p63;
            if (sign) c = -c;
            if (r == c) {
                /* min normal after rounding, underflow depends on arch behaviour which can be imitated by a double to float
                   conversion */
                float fltmin = (float)(0x0.ffffff8p-63 * FLT_MIN * r);
                return DBL_MIN / FLT_MIN * fltmin;
            }
            /* one bit is lost when scaled, add another top bit to only round once at conversion if it is inexact */
            if (rhi << 53) {
                i = (int64_t)(rhi >> 1 | (rhi & 1) | 1ull << 62);
                if (sign) i = -i;
                r = (double)i;
                r = 2 * r - c; /* remove top bit */

                /* raise underflow portably, such that it cannot be optimized away */
                {
                    volatile double tiny = DBL_MIN / FLT_MIN * r;
                    r += (double)(tiny * tiny) * (r - r);
                }
            }
        } else {
            /* only round once when scaled */
            d = 10;
            i = (int64_t)((rhi >> d | !!(rhi << (64 - d))) << d);
            if (sign) i = -i;
            r = (double)i;
        }
    }
    return fefw_scalbn(r, e);
}

float fmaf(float x, float y, float z) {
    double xy, result;
    union { double f; uint64_t i; } u;
    int e;

    xy = (double)x * y;
    result = xy + z;
    u.f = result;
    e = (int)(u.i >> 52 & 0x7ff);
    /* Common case: The double precision result is fine. */
    if ((u.i & 0x1fffffff) != 0x10000000 || /* not a halfway case */
        e == 0x7ff ||                       /* NaN */
        (result - xy == z && result - z == xy) || /* exact */
        fegetround() != FE_TONEAREST)       /* not round-to-nearest */
    {
        return (float)result;
    }

    /* If result is inexact, and exactly halfway between two float values, we need to adjust the low-order bit in the direction
       of the error. */
    double err;
    int neg = (int)(u.i >> 63);
    if (neg == (z > xy))
        err = xy - result + z;
    else
        err = z - result + xy;
    if (neg == (err < 0))
        u.i++;
    else
        u.i--;
    return (float)u.f;
}
