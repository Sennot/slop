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
    if (p.size() < 3) return p;
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
    std::vector<V> out;
    for (std::size_t i = 0; i < p.size(); i++)
        if (keep[i]) out.push_back(p[i]);
    return out;
}

// Picks flips for a slope whose unrotated box is w x h so that after rotation the
// hypotenuse rises/falls as asked and the solid half lies below (floor) or above (ceiling).
static void orient(Piece& pc, double w, double h, int rot, bool rising, bool baseRight) {
    for (int f = 0; f < 4; f++) {
        V t[3] = {{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}}; // t[1] = right angle
        for (auto& v : t) {
            if (!baseRight) v.x = -v.x;
            if (f & 1) v.x = -v.x;
            if (f & 2) v.y = -v.y;
            if (rot == 90) v = {v.y, -v.x};
        }
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

}
