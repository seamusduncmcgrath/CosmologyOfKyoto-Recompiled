/* Regions as lists of non-overlapping rectangles, kept in bands (rows of rectangles with
 * equal top and bottom, sorted by y then x).
 *
 * Boolean combination works on the grid formed by every edge of both inputs: each cell is
 * wholly inside or outside each input, so the result is exact. That's quadratic in the
 * number of edges, which is fine for the small regions of this program (window visibility,
 * clip rectangles, the odd ellipse).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "win.h"

int rect_intersect(IRect *out, const IRect *a, const IRect *b)
{
    IRect r = irect(a->l > b->l ? a->l : b->l, a->t > b->t ? a->t : b->t,
                    a->r < b->r ? a->r : b->r, a->b < b->b ? a->b : b->b);
    if (rect_empty(&r))
        r = irect(0, 0, 0, 0);
    *out = r;
    return !rect_empty(&r);
}

void rect_union(IRect *out, const IRect *a, const IRect *b)
{
    if (rect_empty(a)) { *out = *b; return; }
    if (rect_empty(b)) { *out = *a; return; }
    *out = irect(a->l < b->l ? a->l : b->l, a->t < b->t ? a->t : b->t,
                 a->r > b->r ? a->r : b->r, a->b > b->b ? a->b : b->b);
}

void rgn_init(Region *g)
{
    memset(g, 0, sizeof(*g));
}

void rgn_free(Region *g)
{
    free(g->r);
    rgn_init(g);
}

static void reserve(Region *g, int n)
{
    if (n <= g->cap)
        return;
    g->cap = n < 8 ? 8 : n * 2;
    g->r = realloc(g->r, (size_t)g->cap * sizeof(IRect));
}

static void update_box(Region *g)
{
    g->box = irect(0, 0, 0, 0);
    for (int i = 0; i < g->n; i++)
        rect_union(&g->box, &g->box, &g->r[i]);
}

void rgn_set_rect(Region *g, IRect r)
{
    g->n = 0;
    if (!rect_empty(&r)) {
        reserve(g, 1);
        g->r[g->n++] = r;
    }
    update_box(g);
}

void rgn_copy(Region *dst, const Region *src)
{
    if (dst == src)
        return;
    reserve(dst, src->n);
    if (src->n)
        memcpy(dst->r, src->r, (size_t)src->n * sizeof(IRect));
    dst->n = src->n;
    dst->box = src->box;
}

void rgn_add(Region *g, IRect r)
{
    if (rect_empty(&r))
        return;
    reserve(g, g->n + 1);
    g->r[g->n++] = r;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static int uniq(int *v, int n)
{
    qsort(v, (size_t)n, sizeof(int), cmp_int);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || v[m - 1] != v[i])
            v[m++] = v[i];
    return m;
}

/* rectangles of g that cover row y, into list; returns count */
static int band_rects(const Region *g, int y, const IRect **list)
{
    int k = 0;
    for (int i = 0; i < g->n; i++)
        if (g->r[i].t <= y && y < g->r[i].b)
            list[k++] = &g->r[i];
    return k;
}

static int covered(const IRect **list, int n, int x)
{
    for (int i = 0; i < n; i++)
        if (list[i]->l <= x && x < list[i]->r)
            return 1;
    return 0;
}

static void grid_combine(Region *dst, const Region *a, const Region *b, int op);

void rgn_combine(Region *dst, const Region *a, const Region *b, int op)
{
    if (op == RGN_COPY) {
        rgn_copy(dst, a);
        return;
    }
    /* quick cases */
    if (op == RGN_AND && (!a->n || !b->n)) { rgn_set_rect(dst, irect(0, 0, 0, 0)); return; }
    if (op == RGN_AND && a->n == 1 && b->n == 1) {
        IRect r;
        rect_intersect(&r, &a->r[0], &b->r[0]);
        rgn_set_rect(dst, r);
        return;
    }
    if ((op == RGN_OR || op == RGN_XOR) && !b->n) { rgn_copy(dst, a); return; }
    if ((op == RGN_OR || op == RGN_XOR) && !a->n) { rgn_copy(dst, b); return; }
    if (op == RGN_DIFF && (!a->n || !b->n)) { rgn_copy(dst, a); return; }
    if (op == RGN_DIFF) {
        IRect r;
        if (!rect_intersect(&r, &a->box, &b->box)) { rgn_copy(dst, a); return; }
    }
    grid_combine(dst, a, b, op);
}

static void grid_combine(Region *dst, const Region *a, const Region *b, int op)
{
    int ne = 2 * (a->n + b->n) + 1;
    int *xs = malloc(sizeof(int) * (size_t)ne), *ys = malloc(sizeof(int) * (size_t)ne);
    int nx = 0, ny = 0;
    for (int i = 0; i < a->n; i++) { xs[nx++] = a->r[i].l; xs[nx++] = a->r[i].r; ys[ny++] = a->r[i].t; ys[ny++] = a->r[i].b; }
    for (int i = 0; i < b->n; i++) { xs[nx++] = b->r[i].l; xs[nx++] = b->r[i].r; ys[ny++] = b->r[i].t; ys[ny++] = b->r[i].b; }
    nx = uniq(xs, nx);
    ny = uniq(ys, ny);
    const IRect **la = malloc(sizeof(IRect *) * (size_t)(a->n + 1));
    const IRect **lb = malloc(sizeof(IRect *) * (size_t)(b->n + 1));

    Region out;
    rgn_init(&out);
    int prev_start = -1, prev_count = 0;     /* rectangles of the previous band in `out` */
    for (int j = 0; j + 1 < ny; j++) {
        int y0 = ys[j], y1 = ys[j + 1];
        int na = band_rects(a, y0, la), nb = band_rects(b, y0, lb);
        int start = out.n;
        int run_l = 0, in_run = 0;
        for (int i = 0; i + 1 < nx; i++) {
            int x = xs[i];
            int ia = covered(la, na, x), ib = covered(lb, nb, x);
            int in = op == RGN_AND ? ia && ib : op == RGN_OR ? ia || ib : op == RGN_XOR ? ia != ib : ia && !ib;
            if (in && !in_run) { run_l = x; in_run = 1; }
            if (!in && in_run) { rgn_add(&out, irect(run_l, y0, x, y1)); in_run = 0; }
        }
        if (in_run)
            rgn_add(&out, irect(run_l, y0, xs[nx - 1], y1));
        int count = out.n - start;
        /* same runs as the band just above and touching it: extend that band instead */
        if (count && count == prev_count && out.r[prev_start].b == y0) {
            int same = 1;
            for (int k = 0; k < count && same; k++)
                same = out.r[prev_start + k].l == out.r[start + k].l && out.r[prev_start + k].r == out.r[start + k].r;
            if (same) {
                for (int k = 0; k < count; k++)
                    out.r[prev_start + k].b = y1;
                out.n = start;
                continue;
            }
        }
        if (count) {
            prev_start = start;
            prev_count = count;
        }
    }
    free(xs); free(ys); free(la); free(lb);
    update_box(&out);
    rgn_free(dst);
    *dst = out;
}

void rgn_finish(Region *g)
{
    Region empty;
    rgn_init(&empty);
    grid_combine(g, g, &empty, RGN_OR);
}

void rgn_offset(Region *g, int dx, int dy)
{
    for (int i = 0; i < g->n; i++)
        rect_offset(&g->r[i], dx, dy);
    if (g->n)
        rect_offset(&g->box, dx, dy);
}

void rgn_clip_rect(Region *g, IRect r)
{
    int m = 0;
    for (int i = 0; i < g->n; i++) {
        IRect x;
        if (rect_intersect(&x, &g->r[i], &r))
            g->r[m++] = x;
    }
    g->n = m;
    update_box(g);
}

int rgn_pt_in(const Region *g, int x, int y)
{
    for (int i = 0; i < g->n; i++)
        if (rect_has_pt(&g->r[i], x, y))
            return 1;
    return 0;
}

int rgn_rect_in(const Region *g, IRect r)
{
    for (int i = 0; i < g->n; i++) {
        IRect x;
        if (rect_intersect(&x, &g->r[i], &r))
            return 1;
    }
    return 0;
}

int rgn_kind(const Region *g)
{
    return g->n == 0 ? NULLREGION : g->n == 1 ? SIMPLEREGION : COMPLEXREGION;
}

int rgn_equal(const Region *a, const Region *b)
{
    if (a->n != b->n)
        return 0;
    for (int i = 0; i < a->n; i++)
        if (memcmp(&a->r[i], &b->r[i], sizeof(IRect)))
            return 0;
    return 1;
}

/* ---- shapes as spans ------------------------------------------------------------------------ */

typedef struct Edge { double x0, y0, x1, y1; int dir; } Edge;
typedef struct Cross { double x; int dir; } Cross;

static int cmp_cross(const void *a, const void *b)
{
    double x = ((const Cross *)a)->x, y = ((const Cross *)b)->x;
    return x < y ? -1 : x > y;
}

void rgn_polygon(Region *g, const int *xy, const int *counts, int npolys, int mode)
{
    int total = 0;
    for (int p = 0; p < npolys; p++)
        total += counts[p];
    Edge *e = malloc(sizeof(Edge) * (size_t)(total + 1));
    Cross *xs = malloc(sizeof(Cross) * (size_t)(total + 1));
    int ne = 0, miny = 0x7FFFFFFF, maxy = -0x7FFFFFFF;
    const int *v = xy;
    for (int p = 0; p < npolys; p++) {
        int n = counts[p];
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            double x0 = v[2 * i], y0 = v[2 * i + 1], x1 = v[2 * j], y1 = v[2 * j + 1];
            if (y0 == y1)
                continue;
            int dir = 1;
            if (y0 > y1) { double t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
            e[ne++] = (Edge){ x0, y0, x1, y1, dir };
            if ((int)y0 < miny) miny = (int)y0;
            if ((int)ceil(y1) > maxy) maxy = (int)ceil(y1);
        }
        v += 2 * n;
    }
    g->n = 0;
    for (int y = miny; y < maxy; y++) {
        double sy = y + 0.5;
        int k = 0;
        for (int i = 0; i < ne; i++)
            if (sy >= e[i].y0 && sy < e[i].y1)
                xs[k++] = (Cross){ e[i].x0 + (sy - e[i].y0) * (e[i].x1 - e[i].x0) / (e[i].y1 - e[i].y0), e[i].dir };
        qsort(xs, (size_t)k, sizeof(Cross), cmp_cross);
        if (mode == 1) {                         /* ALTERNATE: between pairs of crossings */
            for (int i = 0; i + 1 < k; i += 2)
                rgn_add(g, irect((int)ceil(xs[i].x - 0.5), y, (int)ceil(xs[i + 1].x - 0.5), y + 1));
        } else {                                 /* WINDING: where the winding number is nonzero */
            int wind = 0;
            double start = 0;
            for (int i = 0; i < k; i++) {
                int before = wind;
                wind += xs[i].dir;
                if (!before && wind)
                    start = xs[i].x;
                else if (before && !wind)
                    rgn_add(g, irect((int)ceil(start - 0.5), y, (int)ceil(xs[i].x - 0.5), y + 1));
            }
        }
    }
    free(xs);
    free(e);
    rgn_finish(g);
}

void rgn_ellipse(Region *g, IRect r)
{
    g->n = 0;
    double cx = (r.l + r.r) / 2.0, cy = (r.t + r.b) / 2.0, ra = (r.r - r.l) / 2.0, rb = (r.b - r.t) / 2.0;
    if (ra <= 0 || rb <= 0) {
        update_box(g);
        return;
    }
    for (int y = r.t; y < r.b; y++) {
        double yy = (y + 0.5 - cy) / rb;
        if (yy * yy >= 1)
            continue;
        double hw = ra * sqrt(1 - yy * yy);
        rgn_add(g, irect((int)ceil(cx - hw - 0.5), y, (int)ceil(cx + hw - 0.5), y + 1));
    }
    rgn_finish(g);
}

void rgn_roundrect(Region *g, IRect r, int ew, int eh)
{
    g->n = 0;
    if (ew > r.r - r.l) ew = r.r - r.l;
    if (eh > r.b - r.t) eh = r.b - r.t;
    double ra = ew / 2.0, rb = eh / 2.0;
    for (int y = r.t; y < r.b; y++) {
        double inset = 0;
        double dy = 0;
        if (y + 0.5 < r.t + rb)
            dy = (r.t + rb - (y + 0.5)) / rb;
        else if (y + 0.5 > r.b - rb)
            dy = ((y + 0.5) - (r.b - rb)) / rb;
        if (dy > 0 && rb > 0) {
            if (dy >= 1)
                continue;
            inset = ra - ra * sqrt(1 - dy * dy);
        }
        rgn_add(g, irect((int)ceil(r.l + inset - 0.5), y, (int)ceil(r.r - inset - 0.5), y + 1));
    }
    rgn_finish(g);
}
