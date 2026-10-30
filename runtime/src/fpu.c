/* x87 FPU helpers for recompiled code.
 *
 * Registers hold doubles rather than 80-bit extended values. That matches the x87 running
 * at 53-bit precision; 80-bit memory operands are converted on load and store. Rounding
 * for integer stores and FRNDINT follows the control word, implemented in software so it
 * doesn't depend on the host's floating-point environment.
 */
#include <math.h>
#include <string.h>

#include "rt.h"

#define C0 0x0100
#define C1 0x0200
#define C2 0x0400
#define C3 0x4000
#define SW_IE 0x0001
#define SW_SF 0x0040
#define SW_ES 0x0080

void fpu_init(CPU *c)
{
    c->fpu.cw = 0x037F;
    c->fpu.sw = 0;
    c->fpu.top = 0;
    c->fpu.tags = 0xFF;
    memset(c->fpu.st, 0, sizeof(c->fpu.st));
}

void fpu_push(CPU *c, double v)
{
    c->fpu.top = (c->fpu.top - 1) & 7;
    if (!(c->fpu.tags & (1u << c->fpu.top)))
        c->fpu.sw |= SW_IE | SW_SF | C1;           /* stack overflow (masked) */
    c->fpu.st[c->fpu.top] = v;
    c->fpu.tags &= (uint16_t)~(1u << c->fpu.top);
}

void fpu_pop(CPU *c)
{
    c->fpu.tags |= (uint16_t)(1u << c->fpu.top);
    c->fpu.top = (c->fpu.top + 1) & 7;
}

uint16_t fpu_status(CPU *c)
{
    return (uint16_t)((c->fpu.sw & ~0x3800) | (c->fpu.top & 7) << 11);
}

/* ---- memory formats ---------------------------------------------------------------------- */

double fpu_rd_f32(uint16_t s, uint32_t o)
{
    uint32_t bits = rd32(s, o);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

double fpu_rd_f64(uint16_t s, uint32_t o)
{
    uint64_t bits = (uint64_t)rd32(s, o) | (uint64_t)rd32(s, o + 4) << 32;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

void fpu_wr_f32(uint16_t s, uint32_t o, double v)
{
    float f = (float)v;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    wr32(s, o, bits);
}

void fpu_wr_f64(uint16_t s, uint32_t o, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    wr32(s, o, (uint32_t)bits);
    wr32(s, o + 4, (uint32_t)(bits >> 32));
}

double fpu_rd_f80(uint16_t s, uint32_t o)
{
    uint64_t mant = (uint64_t)rd32(s, o) | (uint64_t)rd32(s, o + 4) << 32;
    uint16_t se = rd16(s, o + 8);
    int sign = se >> 15, exp = se & 0x7FFF;
    double v;
    if (exp == 0x7FFF)
        v = (mant << 1) ? NAN : INFINITY;
    else if (exp == 0 && mant == 0)
        v = 0.0;
    else
        v = ldexp((double)mant, exp - 16383 - 63);   /* rounds the 64-bit mantissa to 53 bits */
    return sign ? -v : v;
}

void fpu_wr_f80(uint16_t s, uint32_t o, double v)
{
    uint64_t mant = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ull;
    } else if (isinf(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ull;
    } else if (v != 0.0) {
        int e;
        double m = frexp(fabs(v), &e);              /* m in [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64);              /* exact: m has at most 53 bits */
        se |= (uint16_t)(e - 1 + 16383);
    }
    wr32(s, o, (uint32_t)mant);
    wr32(s, o + 4, (uint32_t)(mant >> 32));
    wr16(s, o + 8, se);
}

/* ---- rounding and integer stores ---------------------------------------------------------- */

static double round_cw(CPU *c, double v)
{
    switch ((c->fpu.cw >> 10) & 3) {
    case 1: return floor(v);
    case 2: return ceil(v);
    case 3: return trunc(v);
    default: {                                      /* nearest, ties to even */
        double r = floor(v), d = v - r;
        if (d > 0.5 || (d == 0.5 && fmod(r, 2.0) != 0.0))
            r += 1.0;
        return r;
    }
    }
}

int64_t fpu_round(CPU *c, double v)
{
    return (int64_t)round_cw(c, v);
}

void fpu_wr_int(CPU *c, uint16_t s, uint32_t o, double v, int size)
{
    double r = round_cw(c, v);
    double lo = size == 2 ? -32768.0 : size == 4 ? -2147483648.0 : -9223372036854775808.0;
    double hi = size == 2 ? 32767.0 : size == 4 ? 2147483647.0 : 9223372036854775807.0;
    int64_t i;
    if (isnan(r) || r < lo || r > hi) {             /* integer indefinite */
        c->fpu.sw |= SW_IE;
        i = size == 2 ? INT16_MIN : size == 4 ? INT32_MIN : INT64_MIN;
    } else {
        i = (int64_t)r;
    }
    if (size == 2) {
        wr16(s, o, (uint16_t)i);
    } else if (size == 4) {
        wr32(s, o, (uint32_t)i);
    } else {
        wr32(s, o, (uint32_t)(uint64_t)i);
        wr32(s, o + 4, (uint32_t)((uint64_t)i >> 32));
    }
}

/* ---- status ------------------------------------------------------------------------------- */

void fpu_compare(CPU *c, double a, double b)
{
    uint16_t sw = c->fpu.sw & ~(C0 | C1 | C2 | C3);
    if (isnan(a) || isnan(b))
        sw |= C0 | C2 | C3;
    else if (a < b)
        sw |= C0;
    else if (a == b)
        sw |= C3;
    c->fpu.sw = sw;
}

void fpu_examine(CPU *c)
{
    uint16_t sw = c->fpu.sw & ~(C0 | C1 | C2 | C3);
    double v = ST(0);
    if (signbit(v))
        sw |= C1;
    if (c->fpu.tags & (1u << c->fpu.top))
        sw |= C3 | C0;                              /* empty */
    else if (isnan(v))
        sw |= C0;
    else if (isinf(v))
        sw |= C2 | C0;
    else if (v == 0.0)
        sw |= C3;
    else if (fpclassify(v) == FP_SUBNORMAL)
        sw |= C3 | C2;
    else
        sw |= C2;
    c->fpu.sw = sw;
}

void fpu_store_env(CPU *c, uint16_t s, uint32_t o)
{
    uint16_t tw = 0;
    for (unsigned i = 0; i < 8; i++) {
        unsigned tag;
        if (c->fpu.tags & (1u << i))
            tag = 3;
        else if (c->fpu.st[i] == 0.0)
            tag = 1;
        else if (!isfinite(c->fpu.st[i]))
            tag = 2;
        else
            tag = 0;
        tw |= (uint16_t)(tag << (2 * i));
    }
    wr16(s, o, c->fpu.cw);
    wr16(s, o + 2, fpu_status(c));
    wr16(s, o + 4, tw);
    for (unsigned i = 6; i < 14; i += 2)
        wr16(s, o + i, 0);                          /* instruction/operand pointers */
    c->fpu.cw |= 0x003F;                            /* FSTENV masks all exceptions */
}

/* ---- transcendental and miscellaneous ----------------------------------------------------- */

void fpu_op(CPU *c, const char *name)
{
    double x = ST(0);
    c->fpu.sw &= ~C2;
    if (!strcmp(name, "fsqrt")) {
        ST(0) = sqrt(x);
    } else if (!strcmp(name, "fsin")) {
        ST(0) = sin(x);
    } else if (!strcmp(name, "fcos")) {
        ST(0) = cos(x);
    } else if (!strcmp(name, "fsincos")) {
        ST(0) = sin(x);
        fpu_push(c, cos(x));
    } else if (!strcmp(name, "fptan")) {
        ST(0) = tan(x);
        fpu_push(c, 1.0);
    } else if (!strcmp(name, "fpatan")) {
        double y = ST(1);
        fpu_pop(c);
        ST(0) = atan2(y, x);
    } else if (!strcmp(name, "f2xm1")) {
        ST(0) = expm1(x * 0.693147180559945309417);
    } else if (!strcmp(name, "fyl2x")) {
        double y = ST(1);
        fpu_pop(c);
        ST(0) = y * log2(x);
    } else if (!strcmp(name, "fyl2xp1")) {
        double y = ST(1);
        fpu_pop(c);
        ST(0) = y * log1p(x) / 0.693147180559945309417;
    } else if (!strcmp(name, "fscale")) {
        ST(0) = ldexp(x, (int)trunc(ST(1)));
    } else if (!strcmp(name, "frndint")) {
        ST(0) = round_cw(c, x);
    } else if (!strcmp(name, "fxtract")) {
        int e;
        double m = frexp(x, &e);                    /* x = m * 2^e, m in [0.5, 1) */
        ST(0) = x == 0.0 ? -INFINITY : (double)(e - 1);
        fpu_push(c, x == 0.0 ? x : m * 2.0);
    } else if (!strcmp(name, "fprem") || !strcmp(name, "fprem1")) {
        double y = ST(1);
        double r = name[5] ? remainder(x, y) : fmod(x, y);
        double q = name[5] ? nearbyint((x - r) / y) : trunc(x / y);
        uint64_t qi = (uint64_t)fabs(q);
        c->fpu.sw &= ~(C0 | C1 | C3);
        if (qi & 1) c->fpu.sw |= C1;
        if (qi & 2) c->fpu.sw |= C3;
        if (qi & 4) c->fpu.sw |= C0;
        ST(0) = r;
    } else {
        rt_fatal("unimplemented x87 operation %s", name);
    }
}
