#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace slop {

static double segDist(V p, V a, V b) {
    double dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
    double t = l > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}

std::vector<V> simplify(std::vector<V> const& p, double eps) {
    std::vector<V> out;
    for (auto i : simplifyIdx(p, eps)) out.push_back(p[i]);
    return out;
}

std::vector<std::size_t> simplifyIdx(std::vector<V> const& p, double eps) {
    std::vector<std::size_t> out;
    if (p.size() < 3) {
        for (std::size_t i = 0; i < p.size(); i++) out.push_back(i);
        return out;
    }
    std::vector<char> keep(p.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<std::size_t, std::size_t>> st{{0, p.size() - 1}};
    while (!st.empty()) {
        auto [a, b] = st.back();
        st.pop_back();
        double best = eps;
        std::size_t idx = 0;
        for (std::size_t i = a + 1; i < b; i++) {
            double d = segDist(p[i], p[a], p[b]);
            if (d > best) best = d, idx = i;
        }
        if (idx) {
            keep[idx] = 1;
            st.push_back({a, idx});
            st.push_back({idx, b});
        }
    }
    for (std::size_t i = 0; i < p.size(); i++)
        if (keep[i]) out.push_back(i);
    return out;
}

// Local point of an unrotated slope -> object space after flips and clockwise rotation.
static V xform(V v, bool fx, bool fy, int rot, bool baseRight) {
    if (!baseRight) v.x = -v.x;
    if (fx) v.x = -v.x;
    if (fy) v.y = -v.y;
    for (int r = 0; r < rot; r += 90) v = {v.y, -v.x};
    return v;
}

// Picks flips for a slope whose unrotated box is w x h so that after rotation the
// hypotenuse rises/falls as asked and the solid half lies below (floor) or above (ceiling).
static void orient(Piece& pc, double w, double h, int rot, bool rising, bool baseRight) {
    for (int f = 0; f < 4; f++) {
        V t[3] = {{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}}; // t[1] = right angle
        for (auto& v : t) v = xform(v, f & 1, f & 2, rot, baseRight);
        double hx = t[2].x - t[0].x, hy = t[2].y - t[0].y;
        bool above = t[1].y > t[0].y + (t[1].x - t[0].x) * hy / hx;
        if ((hx * hy > 0) != rising || above != pc.ceil) continue;
        pc.fx = f & 1;
        pc.fy = f & 2;
        pc.rot = rot;
        pc.n = 3;
        for (int i = 0; i < 3; i++) pc.poly[i] = {pc.c.x + t[i].x, pc.c.y + t[i].y};
        return;
    }
}

// Walks the simplified stroke with a pen that always aims at the next vertex from where
// the previous pieces really ended, so rounding never accumulates.
static void chain(std::vector<V> const& pts, Opts const& o, bool ceil, double lift, std::vector<Piece>& out) {
    constexpr double A63 = 63.43494882292201;
    V p{pts[0].x, pts[0].y + lift};
    for (std::size_t i = 1; i < pts.size(); i++) {
        double dx = pts[i].x - p.x, dy = pts[i].y + lift - p.y;
        if (std::abs(dx) < 1e-9 && std::abs(dy) < 1e-9) continue;
        double a = std::atan2(std::abs(dy), std::abs(dx)) * 57.29577951308232;
        Kind k = Kind::Flat;
        double best = a;
        if (o.wave != Wave::Mini && std::abs(a - 45) < best) k = Kind::S45, best = std::abs(a - 45);
        if (o.wave != Wave::Big && std::abs(a - A63) < best) k = Kind::S63;
        double sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;

        if (k == Kind::Flat) {
            double len = std::abs(dx);
            if (len < 1) continue;
            if (out.size() >= o.maxPieces) return;
            Piece pc;
            pc.kind = k;
            pc.ceil = ceil;
            pc.sx = len / 30;
            double cy = p.y + (ceil ? 15 : -15), x0 = std::min(p.x, p.x + sx * len), x1 = std::max(p.x, p.x + sx * len);
            pc.c = {(x0 + x1) / 2, cy};
            pc.poly[0] = {x0, cy - 15};
            pc.poly[1] = {x1, cy - 15};
            pc.poly[2] = {x1, cy + 15};
            pc.poly[3] = {x0, cy + 15};
            pc.n = 4;
            out.push_back(pc);
            p.x += sx * len;
            continue;
        }

        double w = 30, h = k == Kind::S45 ? 30 : 60;
        int n = (int)std::floor((dx * sx * w + dy * sy * h) / (w * w + h * h) + 0.5);
        for (int j = 0; j < n; j++) {
            if (out.size() >= o.maxPieces) return;
            Piece pc;
            pc.kind = k;
            pc.ceil = ceil;
            pc.c = {p.x + sx * w / 2, p.y + sy * h / 2};
            if (k == Kind::S63 && o.rot63) {
                pc.base = 2;
                orient(pc, 60, 30, 90, sx * sy > 0, o.baseRight);
            } else {
                if (k == Kind::S63) pc.sy = 2;
                orient(pc, 30, h, 0, sx * sy > 0, o.baseRight);
            }
            out.push_back(pc);
            p.x += sx * w;
            p.y += sy * h;
        }
    }
}

std::vector<Piece> build(std::vector<V> const& raw, Opts const& o) {
    std::vector<Piece> out;
    auto pts = simplify(raw, o.eps);
    if (pts.size() < 2) return out;
    if (o.snap > 0) {
        double ox = std::round(pts[0].x / o.snap) * o.snap - pts[0].x;
        double oy = std::round(pts[0].y / o.snap) * o.snap - pts[0].y;
        for (auto& v : pts) v.x += ox, v.y += oy;
    }
    if (o.corridor) {
        chain(pts, o, false, 0, out);
        chain(pts, o, true, o.gap, out);
    } else {
        chain(pts, o, o.ceiling, 0, out);
    }
    return out;
}

// ---- recorded routes ----------------------------------------------------------

std::vector<Section> sections(std::vector<Sample> const& s, double maxGap) {
    std::vector<Section> out;
    for (int p = 0; p < 2; p++) {
        std::vector<Sample const*> run;
        auto flush = [&] {
            if (run.size() >= 8) {
                std::vector<V> pts;
                double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18, hh = 0;
                for (auto r : run) {
                    pts.push_back({r->x, r->y});
                    x0 = std::min(x0, (double)r->x), x1 = std::max(x1, (double)r->x);
                    y0 = std::min(y0, (double)r->y), y1 = std::max(y1, (double)r->y);
                    hh += r->hh;
                }
                auto idx = simplifyIdx(pts, .75);
                Section sec;
                sec.p = p;
                sec.mini = run[0]->mini;
                sec.hh = hh / run.size();
                sec.mid = {(x0 + x1) / 2, (y0 + y1) / 2};
                for (auto i : idx) sec.path.push_back(pts[i]);
                // tightest moment of each straight piece of the path
                for (std::size_t j = 0; j + 1 < idx.size(); j++) {
                    double lo = 1e18, hi = 1e18;
                    for (std::size_t k = idx[j]; k <= idx[j + 1]; k++) {
                        lo = std::min(lo, run[k]->hh + std::min((double)run[k]->down, maxGap));
                        hi = std::min(hi, run[k]->hh + std::min((double)run[k]->up, maxGap));
                    }
                    sec.lo.push_back(lo);
                    sec.hi.push_back(hi);
                }
                if (x1 - x0 >= 30 && !sec.lo.empty()) out.push_back(std::move(sec));
            }
            run.clear();
        };
        for (auto& x : s) {
            if (x.p != p) continue;
            if (x.brk || (!run.empty() && x.mini != run[0]->mini)) flush();
            run.push_back(&x);
        }
        flush();
    }
    std::sort(out.begin(), out.end(), [](Section const& a, Section const& b) { return a.path[0].x < b.path[0].x; });
    return out;
}

// Exact slope pieces along one straight wall segment.
static void wallPieces(V a, V b, bool ceil, double tight, bool haveTwo, bool baseRight, std::vector<Piece>& out) {
    double dx = b.x - a.x, dy = b.y - a.y;
    if (std::abs(dx) < .5) return;
    double ang = std::atan2(std::abs(dy), std::abs(dx)) * 57.29577951308232;
    Piece pc;
    pc.ceil = ceil;
    pc.tight = tight;
    if (ang < 2) {
        double x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), cy = (a.y + b.y) / 2 + (ceil ? 15 : -15);
        pc.kind = Kind::Flat;
        pc.sx = (x1 - x0) / 30;
        pc.c = {(x0 + x1) / 2, cy};
        pc.poly[0] = {x0, cy - 15};
        pc.poly[1] = {x1, cy - 15};
        pc.poly[2] = {x1, cy + 15};
        pc.poly[3] = {x0, cy + 15};
        pc.n = 4;
        out.push_back(pc);
        return;
    }
    bool rising = dx * dy > 0;
    double w, h;
    if (std::abs(ang - 45) < 2) pc.kind = Kind::S45, w = 30, h = 30;
    else if (std::abs(ang - 63.435) < 2) pc.kind = Kind::S63, w = 30, h = 60;
    else if (std::abs(ang - 26.565) < 2) pc.kind = Kind::S26, w = 60, h = 30;
    else {
        pc.kind = Kind::Free;
        pc.c = {(a.x + b.x) / 2, (a.y + b.y) / 2};
        pc.sx = std::abs(dx) / 30;
        pc.sy = std::abs(dy) / 30;
        orient(pc, std::abs(dx), std::abs(dy), 0, rising, baseRight);
        out.push_back(pc);
        return;
    }
    bool two = haveTwo && pc.kind != Kind::S45;
    int rot = pc.kind == Kind::S63 && two ? 90 : 0;
    double lw = rot ? h : w, lh = rot ? w : h; // box before rotation
    double len = std::hypot(w, h), total = std::hypot(dx, dy);
    int n = (int)std::floor(total / len + 1e-6);
    double rem = total - n * len;
    for (int j = 0; j <= n; j++) {
        double s = j < n ? 1 : rem / len; // last piece scaled down uniformly
        if (s < .02) break;
        double along = j < n ? (j + .5) * len : n * len + rem / 2;
        Piece q = pc;
        q.base = two ? 2 : 1;
        q.c = {a.x + dx / total * along, a.y + dy / total * along};
        q.sx = s * lw / (two ? 60 : 30);
        q.sy = s * lh / 30;
        orient(q, lw * s, lh * s, rot, rising, baseRight);
        out.push_back(q);
    }
}

// Floor and ceiling walls: parallel to each path piece at its tightest distance,
// joined where neighbouring offset lines intersect.
void walls(Section const& sec, double margin, bool haveTwo, bool baseRight, std::vector<Piece>& out) {
    std::size_t n = sec.lo.size();
    if (!n) return;
    auto const& P = sec.path;
    for (int side = 0; side < 2; side++) {
        bool ceil = side == 1;
        double sg = ceil ? 1 : -1;
        auto off = [&](std::size_t i) { return std::max(sec.hh, (ceil ? sec.hi[i] : sec.lo[i]) + margin); };
        std::vector<V> w{{P[0].x, P[0].y + sg * off(0)}};
        for (std::size_t i = 1; i < n; i++) {
            V p0{P[i - 1].x, P[i - 1].y + sg * off(i - 1)}, d0{P[i].x - P[i - 1].x, P[i].y - P[i - 1].y};
            V p1{P[i].x, P[i].y + sg * off(i)}, d1{P[i + 1].x - P[i].x, P[i + 1].y - P[i].y};
            V c{P[i].x, P[i].y + sg * (off(i - 1) + off(i)) / 2};
            double cr = d0.x * d1.y - d0.y * d1.x;
            if (std::abs(cr) > 1e-9) {
                double t = ((p1.x - p0.x) * d1.y - (p1.y - p0.y) * d1.x) / cr;
                V q{p0.x + d0.x * t, p0.y + d0.y * t};
                if (std::hypot(q.x - P[i].x, q.y - P[i].y) < 4 * std::max(off(i - 1), off(i))) c = q;
            }
            w.push_back(c);
        }
        w.push_back({P[n].x, P[n].y + sg * off(n - 1)});
        for (std::size_t i = 0; i < n; i++)
            wallPieces(w[i], w[i + 1], ceil, std::clamp((off(i) - sec.hh) / 30, 0.0, 1.0), haveTwo, baseRight, out);
    }
}

void slopeTri(double cx, double cy, double w, double h, bool fx, bool fy, int rot, bool baseRight, V out[3]) {
    double lw = rot % 180 ? h : w, lh = rot % 180 ? w : h;
    V t[3] = {{-lw / 2, -lh / 2}, {lw / 2, -lh / 2}, {lw / 2, lh / 2}};
    for (int i = 0; i < 3; i++) {
        V v = xform(t[i], fx, fy, rot, baseRight);
        out[i] = {cx + v.x, cy + v.y};
    }
}

// Vertical extent of a convex polygon on the line x.
static bool spanAt(V const* p, int n, double x, double& lo, double& hi) {
    lo = 1e18, hi = -1e18;
    for (int i = 0; i < n; i++) {
        V a = p[i], b = p[(i + 1) % n];
        if (x < std::min(a.x, b.x) || x > std::max(a.x, b.x)) continue;
        if (std::abs(b.x - a.x) < 1e-9) {
            lo = std::min({lo, a.y, b.y}), hi = std::max({hi, a.y, b.y});
            continue;
        }
        double y = a.y + (x - a.x) * (b.y - a.y) / (b.x - a.x);
        lo = std::min(lo, y), hi = std::max(hi, y);
    }
    return lo <= hi;
}

// Free space between the box [x0,x1]x[y0,y1] and a convex polygon, straight up / down.
// For a convex polygon the extremes over the box's width sit at its edges or at a vertex.
void clearance(V const* p, int n, double x0, double x1, double y0, double y1, double& up, double& down) {
    double xs[6];
    int m = 0;
    xs[m++] = x0;
    xs[m++] = x1;
    for (int i = 0; i < n; i++)
        if (p[i].x > x0 && p[i].x < x1) xs[m++] = p[i].x;
    for (int k = 0; k < m; k++) {
        double lo, hi;
        if (!spanAt(p, n, xs[k], lo, hi)) continue;
        if (lo >= y1) up = std::min(up, lo - y1);
        else if (hi <= y0) down = std::min(down, y0 - hi);
        else if (lo + hi > y0 + y1) up = 0;
        else down = 0;
    }
}

}
