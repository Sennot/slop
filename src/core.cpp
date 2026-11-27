#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace slop {

constexpr double kDeg = 57.29577951308232;

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

static void rect(Piece& pc, double x0, double y0, double x1, double y1) {
    pc.kind = Kind::Flat;
    pc.c = {(x0 + x1) / 2, (y0 + y1) / 2};
    pc.sx = (x1 - x0) / 30;
    pc.sy = (y1 - y0) / 30;
    pc.poly[0] = {x0, y0};
    pc.poly[1] = {x1, y0};
    pc.poly[2] = {x1, y1};
    pc.poly[3] = {x0, y1};
    pc.n = 4;
}

// ---- drawing ------------------------------------------------------------------------

// Walks the simplified stroke with a pen that always aims at the next vertex from where
// the previous pieces really ended, so rounding never accumulates.
static void chain(std::vector<V> const& pts, Opts const& o, bool ceil, double lift, std::vector<Piece>& out) {
    constexpr double A63 = 63.43494882292201;
    V p{pts[0].x, pts[0].y + lift};
    for (std::size_t i = 1; i < pts.size(); i++) {
        double dx = pts[i].x - p.x, dy = pts[i].y + lift - p.y;
        if (std::abs(dx) < 1e-9 && std::abs(dy) < 1e-9) continue;
        double a = std::atan2(std::abs(dy), std::abs(dx)) * kDeg;
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
            pc.ceil = ceil;
            double x0 = std::min(p.x, p.x + sx * len), x1 = std::max(p.x, p.x + sx * len);
            rect(pc, x0, ceil ? p.y : p.y - 30, x1, ceil ? p.y + 30 : p.y);
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

// ---- geometry -----------------------------------------------------------------------

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

// Vertical extent of a convex polygon over the strip [x0, x1]: for a convex polygon the
// extremes sit on the strip edges or at a vertex inside it.
bool spanOver(V const* p, int n, double x0, double x1, double& lo, double& hi) {
    double xs[2 + kMaxPoly];
    int m = 0;
    xs[m++] = x0;
    xs[m++] = x1;
    for (int i = 0; i < n; i++)
        if (p[i].x > x0 && p[i].x < x1) xs[m++] = p[i].x;
    lo = 1e18, hi = -1e18;
    for (int k = 0; k < m; k++) {
        double a, b;
        if (spanAt(p, n, xs[k], a, b)) lo = std::min(lo, a), hi = std::max(hi, b);
    }
    return lo <= hi;
}

// ---- interval sets ------------------------------------------------------------------

using Ivs = std::vector<Interval>;

static void norm(Ivs& v) {
    std::sort(v.begin(), v.end(), [](Interval const& a, Interval const& b) { return a.lo < b.lo; });
    Ivs o;
    for (auto const& i : v) {
        if (!o.empty() && i.lo <= o.back().hi) o.back().hi = std::max(o.back().hi, i.hi);
        else o.push_back(i);
    }
    v = std::move(o);
}

static Ivs complement(Ivs const& blocked, double a, double b) {
    Ivs o;
    double cur = a;
    for (auto const& i : blocked) {
        if (i.hi <= cur) continue;
        if (i.lo >= b) break;
        if (i.lo > cur) o.push_back({cur, i.lo});
        cur = std::max(cur, i.hi);
    }
    if (cur < b) o.push_back({cur, b});
    return o;
}

static Ivs expand(Ivs const& v, double d) {
    Ivs o;
    for (auto const& i : v) o.push_back({i.lo - d, i.hi + d});
    norm(o);
    return o;
}

static Ivs meet(Ivs const& a, Ivs const& b) {
    Ivs o;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        double lo = std::max(a[i].lo, b[j].lo), hi = std::min(a[i].hi, b[j].hi);
        if (lo <= hi) o.push_back({lo, hi});
        if (a[i].hi < b[j].hi) i++;
        else j++;
    }
    return o;
}

// interval holding y, else the nearest one
static int find(Ivs const& v, double y) {
    int best = -1;
    double bd = 1e18;
    for (int i = 0; i < (int)v.size(); i++) {
        double d = y < v[i].lo ? v[i].lo - y : y > v[i].hi ? y - v[i].hi : 0;
        if (d < bd) bd = d, best = i;
    }
    return best;
}

static bool inside(Ivs const& v, double y) {
    for (auto const& i : v)
        if (y >= i.lo - 1e-6 && y <= i.hi + 1e-6) return true;
    return false;
}

// ---- route sections -----------------------------------------------------------------

std::vector<Section> split(Route const& r) {
    std::vector<Section> out;
    for (int p = 0; p < 2; p++) {
        Section cur;
        Tick const* last = nullptr;
        auto close = [&] {
            if (cur.ti.size() >= 8) {
                auto const& a = r.ticks[cur.ti.front()];
                auto const& b = r.ticks[cur.ti.back()];
                cur.x0 = std::min(a.x, b.x), cur.x1 = std::max(a.x, b.x);
                double y0 = 1e18, y1 = -1e18;
                for (auto i : cur.ti) y0 = std::min(y0, (double)r.ticks[i].y), y1 = std::max(y1, (double)r.ticks[i].y);
                cur.mid = {(cur.x0 + cur.x1) / 2, (y0 + y1) / 2};
                if (cur.x1 - cur.x0 >= 15) out.push_back(std::move(cur));
            }
            cur = Section{};
        };
        for (std::size_t i = 0; i < r.ticks.size(); i++) {
            auto const& t = r.ticks[i];
            if (t.p != p) continue;
            bool brk = !last || t.brk || t.mini != last->mini;
            if (!brk) {
                double dx = t.x - last->x, dy = t.y - last->y;
                if (dx <= 0 || dx > 30 || std::abs(dy) > (t.mini ? 2 : 1) * dx + 2) brk = true; // teleport / respawn
            }
            if (brk) close();
            if (cur.ti.empty()) cur.p = p, cur.mini = t.mini;
            cur.ti.push_back(i);
            last = &t;
        }
        close();
    }
    std::sort(out.begin(), out.end(), [](Section const& a, Section const& b) { return a.x0 < b.x0; });
    return out;
}

// Exact slope pieces along one straight wall segment.
static void wallPieces(V a, V b, bool ceil, double tight, bool haveTwo, bool baseRight, std::vector<Piece>& out) {
    double dx = b.x - a.x, dy = b.y - a.y;
    if (std::abs(dx) < .05) return;
    double ang = std::atan2(std::abs(dy), std::abs(dx)) * kDeg;
    Piece pc;
    pc.ceil = ceil;
    pc.tight = tight;
    if (ang < 1.5) {
        double x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y = (a.y + b.y) / 2;
        rect(pc, x0, ceil ? y : y - 30, x1, ceil ? y + 30 : y);
        out.push_back(pc);
        return;
    }
    bool rising = dx * dy > 0;
    double w, h;
    if (std::abs(ang - 45) < 1.5) pc.kind = Kind::S45, w = 30, h = 30;
    else if (std::abs(ang - 63.435) < 1.5) pc.kind = Kind::S63, w = 30, h = 60;
    else if (std::abs(ang - 26.565) < 1.5) pc.kind = Kind::S26, w = 60, h = 30;
    else {
        pc.kind = Kind::Free;
        pc.c = {(a.x + b.x) / 2, (a.y + b.y) / 2};
        pc.sx = std::abs(dx) / 30;
        pc.sy = std::abs(dy) / 30;
        orient(pc, std::abs(dx), std::abs(dy), 0, rising, baseRight);
        out.push_back(pc);
        return;
    }
    // stretch the exact angle a little so the pieces end exactly on b
    bool two = haveTwo && pc.kind != Kind::S45;
    int rot = pc.kind == Kind::S63 && two ? 90 : 0;
    double lw = rot ? h : w, lh = rot ? w : h; // box before rotation
    double len = std::hypot(w, h), total = std::hypot(dx, dy);
    int n = (int)std::floor(total / len + 1e-6);
    double rem = total - n * len;
    double fx = std::abs(dx) / (total / len * w), fy = std::abs(dy) / (total / len * h); // ~1
    for (int j = 0; j <= n; j++) {
        double s = j < n ? 1 : rem / len; // last piece scaled down
        if (s < .01) break;
        double along = j < n ? (j + .5) * len : n * len + rem / 2;
        Piece q = pc;
        q.base = two ? 2 : 1;
        q.c = {a.x + dx / total * along, a.y + dy / total * along};
        double bw = lw * s * (rot ? fy : fx), bh = lh * s * (rot ? fx : fy);
        q.sx = bw / (two ? 60 : 30);
        q.sy = bh / 30;
        orient(q, bw, bh, rot, rising, baseRight);
        out.push_back(q);
    }
}

// Vertical face between two boundary heights, as a block on the solid side.
static void riser(std::vector<V> const& v, std::size_t j, bool ceil, double tight, std::vector<Piece>& out) {
    V A = v[j], B = v[j + 1];
    double xs = (A.x + B.x) / 2, y0 = std::min(A.y, B.y), y1 = std::max(A.y, B.y);
    if (y1 - y0 < .25) return;
    bool up = B.y > A.y;
    bool right = ceil ? !up : up; // solid side of the step
    double w = 1;
    if (right && j + 2 < v.size()) {
        V C = v[j + 2];
        if (ceil ? C.y <= B.y + 1 : C.y >= B.y - 1) w = std::clamp(C.x - B.x, 1.0, 30.0);
    } else if (!right && j >= 1) {
        V Z = v[j - 1];
        if (ceil ? Z.y <= A.y + 1 : Z.y >= A.y - 1) w = std::clamp(A.x - Z.x, 1.0, 30.0);
    }
    Piece pc;
    pc.ceil = ceil;
    pc.tight = tight;
    rect(pc, right ? xs : xs - w, y0, right ? xs + w : xs, y1);
    out.push_back(pc);
}

// pts = how far the wave center can go (channel edge). The wall that produces exactly that
// edge is the edge pushed out by the hitbox: ev + |slope| * eh vertically, eh sideways for steps.
static void fitWall(std::vector<V> const& pts, std::vector<char> const& open, std::vector<double> const& wid, bool ceil,
                    double ev, double eh, Params const& prm, std::vector<Piece>& out) {
    std::size_t n = pts.size(), a = 0;
    double sg = ceil ? 1 : -1;
    while (a < n) {
        if (open[a]) {
            a++;
            continue;
        }
        std::size_t b = a;
        while (b < n && !open[b]) b++;
        std::vector<V> run(pts.begin() + a, pts.begin() + b);
        auto idx = simplifyIdx(run, prm.tol);
        std::vector<V> v;
        for (auto i : idx) v.push_back(run[i]);
        std::size_t m = v.size();
        if (m < 2) {
            a = b;
            continue;
        }
        // offset lines (point + direction)
        std::vector<V> lp(m - 1), ld(m - 1);
        std::vector<char> vert(m - 1);
        for (std::size_t j = 0; j + 1 < m; j++) {
            double dx = v[j + 1].x - v[j].x, dy = v[j + 1].y - v[j].y;
            vert[j] = std::atan2(std::abs(dy), std::abs(dx)) * kDeg > 80;
            if (vert[j]) {
                bool right = ceil ? dy < 0 : dy > 0; // solid side of the step
                lp[j] = {(v[j].x + v[j + 1].x) / 2 + (right ? eh : -eh), v[j].y};
                ld[j] = {0, 1};
            } else {
                double off = ev + std::abs(dy / dx) * eh + prm.margin;
                lp[j] = {v[j].x, v[j].y + sg * off};
                ld[j] = {dx, dy};
            }
        }
        auto at = [&](std::size_t j, double x) { // point of line j at x (or its own x if vertical)
            if (vert[j]) return V{lp[j].x, lp[j].y};
            return V{x, lp[j].y + (x - lp[j].x) * ld[j].y / ld[j].x};
        };
        std::vector<V> W{at(0, v[0].x)};
        for (std::size_t j = 1; j + 1 < m; j++) {
            V p0 = lp[j - 1], d0 = ld[j - 1], p1 = lp[j], d1 = ld[j];
            double cr = d0.x * d1.y - d0.y * d1.x;
            V c = at(j, v[j].x);
            if (std::abs(cr) > 1e-9) {
                double t = ((p1.x - p0.x) * d1.y - (p1.y - p0.y) * d1.x) / cr;
                V q{p0.x + d0.x * t, p0.y + d0.y * t};
                if (std::hypot(q.x - v[j].x, q.y - v[j].y) < 4 * (ev + 3 * eh + std::abs(prm.margin)) + 30) c = q;
            }
            W.push_back(c);
        }
        W.push_back(at(m - 2, v[m - 1].x));
        for (std::size_t j = 0; j + 1 < W.size(); j++) {
            double mw = 1e18;
            for (std::size_t q = idx[j]; q <= idx[j + 1]; q++) mw = std::min(mw, wid[a + q]);
            double tight = std::clamp(mw / 60, 0.0, 1.0);
            if (vert[j]) riser(W, j, ceil, tight, out);
            else wallPieces(W[j], W[j + 1], ceil, tight, prm.haveTwo, prm.baseRight, out);
        }
        a = b;
    }
}

// Forward: where the wave can get from the entry. Backward: from where it can still reach the
// exit. Both only through free space, moving +-k per unit of x. x depends on time only, so this
// is exact for anything the triggers do, as long as they do not depend on the path itself.
static std::vector<Interval> channel(Route const& r, Section const& s, std::vector<Ivs> const& F) {
    std::size_t n = s.ti.size();
    double k = s.mini ? 2 : 1;
    auto T = [&](std::size_t i) -> Tick const& { return r.ticks[s.ti[i]]; };
    auto pick = [&](Ivs const& v, double y) {
        Ivs o;
        int j = find(v, y);
        if (j >= 0) o.push_back(v[j]);
        return o;
    };
    // Only the tube the bot flies in: a parallel corridor that merges later must not widen it,
    // the slope version will not have that corridor.
    std::vector<Ivs> R(n), C(n);
    R[0] = pick(F[0], T(0).y);
    for (std::size_t i = 1; i < n; i++) {
        R[i] = pick(meet(expand(R[i - 1], k * (T(i).x - T(i - 1).x)), F[i]), T(i).y);
        if (R[i].empty()) R[i] = pick(F[i], T(i).y);
    }
    C[n - 1] = pick(F[n - 1], T(n - 1).y);
    for (std::size_t i = n - 1; i > 0; i--) {
        C[i - 1] = pick(meet(expand(C[i], k * (T(i).x - T(i - 1).x)), F[i - 1]), T(i - 1).y);
        if (C[i - 1].empty()) C[i - 1] = pick(F[i - 1], T(i - 1).y);
    }
    std::vector<Interval> ch(n);
    for (std::size_t i = 0; i < n; i++) {
        auto g = meet(R[i], C[i]);
        int j = find(g, T(i).y);
        ch[i] = j >= 0 ? g[j] : Interval{T(i).y, T(i).y};
    }
    return ch;
}

// How many ticks a click can come early / late and the run still be savable.
static void windows(Route const& r, Section const& s, std::vector<Ivs> const& F, std::vector<Interval> const& ch,
                    std::vector<int> const& dir, std::vector<int>& lo, std::vector<int>& hi) {
    double k = s.mini ? 2 : 1;
    auto T = [&](std::size_t i) -> Tick const& { return r.ticks[s.ti[i]]; };
    auto in = [](Interval const& c, double y) { return y >= c.lo - 1e-6 && y <= c.hi + 1e-6; };
    int n = (int)s.ti.size();
    lo.clear(), hi.clear();
    for (int i : s.clicks) {
        int d0 = dir[i - 1], d1 = dir[i], h = 0, l = 0;
        for (int sft = 1; sft <= 10 && i + sft < n; sft++) {
            double y = T(i).y;
            bool ok = true;
            for (int j = i; j < i + sft && ok; j++) y += d0 * k * (T(j + 1).x - T(j).x), ok = inside(F[j + 1], y);
            if (!ok || !in(ch[i + sft], y)) break;
            h = sft;
        }
        for (int sft = 1; sft <= 10 && i - sft >= 0; sft++) {
            double y = T(i - sft).y;
            bool ok = true;
            for (int j = i - sft; j < i && ok; j++) y += d1 * k * (T(j + 1).x - T(j).x), ok = inside(F[j + 1], y);
            if (!ok || !in(ch[i], y)) break;
            l = sft;
        }
        lo.push_back(l), hi.push_back(h);
    }
}

void solve(Route const& r, Section& s, Params const& prm) {
    std::size_t n = s.ti.size();
    auto T = [&](std::size_t i) -> Tick const& { return r.ticks[s.ti[i]]; };

    // Hitbox calibration: the bot survived, so the real hitbox never overlapped anything.
    // One badly modelled object must not shrink the hitbox everywhere: use the closest approach
    // per object and only follow a consistent trend (several objects), never single outliers.
    std::unordered_map<std::int32_t, std::pair<double, int>> closest; // cand -> ratio, type
    s.cands.clear();
    for (std::size_t i = 0; i < n; i++) {
        auto const& t = T(i);
        for (std::uint32_t q = t.first; q < t.first + t.count; q++) {
            auto const& o = r.obs[q];
            if (o.cand < 0) continue;
            s.cands.push_back(o.cand);
            if (o.type == 2) continue; // the wave may slide on slopes: touching them says nothing about the hitbox
            double d = t.y < o.lo ? o.lo - t.y : t.y > o.hi ? t.y - o.hi : 0;
            double f = d / std::max(.01f, t.hh);
            auto it = closest.find(o.cand);
            if (it == closest.end()) closest[o.cand] = {f, o.type == 0 ? 0 : 1};
            else it->second.first = std::min(it->second.first, f);
        }
    }
    std::sort(s.cands.begin(), s.cands.end());
    s.cands.erase(std::unique(s.cands.begin(), s.cands.end()), s.cands.end());
    auto calib = [&](int type) {
        std::vector<double> v;
        for (auto const& [c, rt] : closest)
            if (rt.second == type && rt.first < 1) v.push_back(rt.first);
        if (v.size() < 4) return 1.0;
        std::sort(v.begin(), v.end());
        return std::clamp(v[v.size() / 4] - .005, .5, 1.0);
    };
    s.fHaz = calib(0);
    s.fSolid = calib(1);

    // free space per tick; whatever the bot provably flew through does not block at that tick
    std::vector<Ivs> F(n);
    s.anomalies = 0;
    for (std::size_t i = 0; i < n; i++) {
        auto const& t = T(i);
        double eh = t.hh * s.fHaz, es = t.hh * s.fSolid;
        Ivs bl;
        bool bad = false;
        for (std::uint32_t q = t.first; q < t.first + t.count; q++) {
            auto const& o = r.obs[q];
            double e = o.type == 0 ? eh : es;
            if (t.y > o.lo - e + 1e-3 && t.y < o.hi + e - 1e-3) {
                // the bot was there (sliding on a slope, or a badly modelled object): the obstacle
                // starts right at the path instead of vanishing, so the wall stays on it
                bad |= o.type != 2;
                if (o.lo + o.hi > 2 * t.y) bl.push_back({t.y + 1e-3, o.hi + e});
                else bl.push_back({o.lo - e, t.y - 1e-3});
                continue;
            }
            bl.push_back({o.lo - e, o.hi + e});
        }
        s.anomalies += bad;
        norm(bl);
        F[i] = complement(bl, t.y - prm.window, t.y + prm.window);
    }
    s.chan = channel(r, s, F);

    // walls
    std::vector<V> fl(n), ce(n);
    std::vector<char> fo(n), co(n);
    std::vector<double> wid(n);
    double hh = 0;
    for (std::size_t i = 0; i < n; i++) {
        auto const& t = T(i);
        auto c = s.chan[i];
        fo[i] = c.lo <= t.y - prm.window + 1;
        co[i] = c.hi >= t.y + prm.window - 1;
        fl[i] = {t.x, c.lo};
        ce[i] = {t.x, c.hi};
        wid[i] = c.hi - c.lo;
        hh += t.hh;
    }
    hh /= n;
    s.pieces.clear();
    double hw = prm.hw > 0 ? prm.hw : hh;
    fitWall(fl, fo, wid, false, hh * s.fSolid * prm.wallScale, hw, prm, s.pieces);
    fitWall(ce, co, wid, true, hh * s.fSolid * prm.wallScale, hw, prm, s.pieces);

    // the same analysis on the generated geometry
    struct PB {
        double x0, x1;
        Piece const* p;
    };
    std::vector<PB> pb;
    for (auto const& p : s.pieces) {
        double a = 1e18, b = -1e18;
        for (int i = 0; i < p.n; i++) a = std::min(a, p.poly[i].x), b = std::max(b, p.poly[i].x);
        pb.push_back({a, b, &p});
    }
    std::sort(pb.begin(), pb.end(), [](PB const& a, PB const& b) { return a.x0 < b.x0; });
    std::vector<Ivs> G(n);
    std::size_t start = 0;
    for (std::size_t i = 0; i < n; i++) {
        auto const& t = T(i);
        double hx = prm.hw > 0 ? prm.hw : t.hh;
        double hx0 = t.x - hx, hx1 = t.x + hx, es = t.hh * s.fSolid;
        Ivs bl;
        while (start < pb.size() && pb[start].x1 < hx0 - 200) start++;
        for (std::size_t q = start; q < pb.size() && pb[q].x0 <= hx1; q++) {
            if (pb[q].x1 < hx0) continue;
            double lo, hi;
            if (spanOver(pb[q].p->poly, pb[q].p->n, hx0, hx1, lo, hi)) bl.push_back({lo - es, hi + es});
        }
        for (std::uint32_t q = t.first; q < t.first + t.count; q++)
            if (r.obs[q].type == 3) bl.push_back({r.obs[q].lo - es, r.obs[q].hi + es});
        norm(bl);
        G[i] = complement(bl, t.y - prm.window, t.y + prm.window);
    }
    auto ch2 = channel(r, s, G);
    s.chan2 = ch2;
    std::size_t ok = 0;
    s.bad.clear();
    for (std::size_t i = 0; i < n; i++) {
        double tol = 1.5 + std::abs(prm.margin); // under one tick of wave travel
        bool good = (fo[i] || std::abs(ch2[i].lo - s.chan[i].lo) <= tol) && (co[i] || std::abs(ch2[i].hi - s.chan[i].hi) <= tol);
        ok += good;
        if (!good && (s.bad.empty() || T(i).x - s.bad.back().x > 6)) s.bad.push_back({T(i).x, T(i).y});
    }
    s.score = n ? 100.0 * ok / n : 0;

    // click windows, original vs slope version
    std::vector<int> dir(n, 0);
    double k = s.mini ? 2 : 1;
    for (std::size_t i = 0; i + 1 < n; i++) {
        double dy = T(i + 1).y - T(i).y, d = k * (T(i + 1).x - T(i).x);
        dir[i] = dy > d * .5 ? 1 : dy < -d * .5 ? -1 : 0;
    }
    s.clicks.clear();
    for (std::size_t i = 1; i + 1 < n; i++)
        if (dir[i] && dir[i - 1] && dir[i] != dir[i - 1]) s.clicks.push_back((int)i);
    windows(r, s, F, s.chan, dir, s.winLo, s.winHi);
    windows(r, s, G, ch2, dir, s.genLo, s.genHi);
}

}
