// Offline route tool. Uses the same core as the mod (src/core.cpp), no Geode.
//
//   slop_tool summary <route.bin> [tol window wallScale]
//   slop_tool section <route.bin> <part> <out.csv>     data for tools/plot.py + problem list
//   slop_tool pieces  <route.bin> [out] [tol window wallScale]
//                                                      write <route>.pieces.txt for the mod (Load)
//   slop_tool clean   <route.bin> <out.bin>            keep only the last pass (practice retries)
//   slop_tool cut     <route.bin> <out.bin> <x0> <x1>  drop everything recorded in [x0, x1]
//
// Parts are numbered from 1 like in the editor bar.
#include "../src/core.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

using namespace slop;

template <class T>
static bool getVec(std::ifstream& f, std::vector<T>& v) {
    uint32_t n = 0;
    if (!f.read((char*)&n, 4)) return false;
    v.resize(n);
    return !n || (bool)f.read((char*)v.data(), n * sizeof(T));
}

template <class T>
static void putVec(std::ofstream& f, std::vector<T> const& v) {
    uint32_t n = (uint32_t)v.size();
    f.write((char const*)&n, 4);
    if (n) f.write((char const*)v.data(), n * sizeof(T));
}

static Route load(char const* path) {
    Route r;
    std::ifstream f(path, std::ios::binary);
    char m[4] = {};
    if (!f.read(m, 4) || std::memcmp(m, "SLP2", 4) || !getVec(f, r.ticks) || !getVec(f, r.obs) || !getVec(f, r.inputs) || !getVec(f, r.cands)) {
        std::fprintf(stderr, "cannot read %s\n", path);
        std::exit(1);
    }
    return r;
}

static void save(char const* path, Route const& r) {
    std::ofstream f(path, std::ios::binary);
    f.write("SLP2", 4);
    putVec(f, r.ticks), putVec(f, r.obs), putVec(f, r.inputs), putVec(f, r.cands);
}

static Params params(int argc, char** argv, int at) {
    Params p;
    if (argc > at) p.tol = std::atof(argv[at]);
    if (argc > at + 1) p.window = std::atof(argv[at + 1]);
    if (argc > at + 2) p.wallScale = std::atof(argv[at + 2]);
    return p;
}

static void stats(Section const& s, int& tight, int& diff) {
    tight = diff = 0;
    for (size_t c = 0; c < s.clicks.size(); c++) {
        tight += s.winLo[c] + s.winHi[c] <= 1;
        diff += s.winLo[c] != s.genLo[c] || s.winHi[c] != s.genHi[c];
    }
}

// Rebuilds a route from the ticks to keep (obstacle ranges re-indexed).
static Route keep(Route const& r, std::vector<size_t> const& ticks, std::vector<Input> inputs) {
    Route o;
    o.cands = r.cands;
    o.inputs = std::move(inputs);
    for (auto i : ticks) {
        Tick t = r.ticks[i];
        uint32_t first = (uint32_t)o.obs.size();
        for (uint32_t q = t.first; q < t.first + t.count; q++) o.obs.push_back(r.obs[q]);
        t.first = first;
        o.ticks.push_back(t);
    }
    return o;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: slop_tool summary|section|pieces|clean|cut <route.bin> ...");
        return 1;
    }
    std::string cmd = argv[1];
    Route r = load(argv[2]);

    if (cmd == "clean" || cmd == "cut") {
        if (argc < 4) return 1;
        double x0 = cmd == "cut" && argc > 5 ? std::atof(argv[4]) : 0, x1 = cmd == "cut" && argc > 5 ? std::atof(argv[5]) : 0;
        std::vector<size_t> kept;
        std::vector<Input> in;
        if (cmd == "clean") {
            // a jump back by more than one step is a respawn: what came after the new x is dropped
            std::vector<size_t> brkAt;
            for (size_t i = 0; i < r.ticks.size(); i++) {
                auto const& t = r.ticks[i];
                bool back = false;
                for (size_t j = kept.size(); j-- > 0;)
                    if (r.ticks[kept[j]].p == t.p) {
                        back = t.x < r.ticks[kept[j]].x - 5;
                        break;
                    }
                if (back) {
                    while (!kept.empty() && r.ticks[kept.back()].x > t.x - 1) kept.pop_back();
                    brkAt.push_back(kept.size());
                }
                kept.push_back(i);
            }
            for (auto const& e : r.inputs) {
                if (!in.empty() && e.x < in.back().x - 5)
                    while (!in.empty() && in.back().x > e.x - 1) in.pop_back();
                in.push_back(e);
            }
            Route o = keep(r, kept, in);
            for (auto k : brkAt) // the first tick of every new attempt starts a new part
                if (k < o.ticks.size()) o.ticks[k].brk = 1;
            save(argv[3], o);
            std::printf("ticks %zu -> %zu, inputs %zu -> %zu\n", r.ticks.size(), o.ticks.size(), r.inputs.size(), o.inputs.size());
        } else {
            for (size_t i = 0; i < r.ticks.size(); i++)
                if (r.ticks[i].x < x0 || r.ticks[i].x > x1) kept.push_back(i);
            for (auto const& e : r.inputs)
                if (e.x < x0 || e.x > x1) in.push_back(e);
            Route o = keep(r, kept, in);
            for (size_t i = 1; i < o.ticks.size(); i++)
                if (o.ticks[i - 1].x < x0 && o.ticks[i].x > x1) o.ticks[i].brk = 1;
            save(argv[3], o);
            std::printf("ticks %zu -> %zu, inputs %zu -> %zu\n", r.ticks.size(), o.ticks.size(), r.inputs.size(), o.inputs.size());
        }
        return 0;
    }

    auto secs = split(r);
    bool isSection = cmd == "section";
    Params prm = params(argc, argv, isSection ? 5 : cmd == "pieces" ? 4 : 3);
    // The recorded strip used the real hitbox width, which is not always the height (mini wave):
    // per part, take the half width that makes the slope version match best.
    std::vector<double> bestHw(secs.size(), 0);
    for (size_t i = 0; i < secs.size(); i++) {
        auto& s = secs[i];
        double hh = r.ticks[s.ti[s.ti.size() / 2]].hh; // the first tick can still have the old size
        struct Try {
            double f, score;
            int diff;
        };
        std::vector<Try> tried;
        double best = -1;
        for (double f = .5; f <= 1.31; f += .05) {
            Params p = prm;
            p.hw = hh * f;
            solve(r, s, p);
            int tight, diff;
            stats(s, tight, diff);
            tried.push_back({f, s.score, diff});
            best = std::max(best, s.score);
        }
        // among the best (within 0.15%): fewest clicks whose tick window changed, then closest to square
        double pick = -1;
        int pd = 1 << 30;
        for (auto const& t : tried)
            if (t.score >= best - .15 && (t.diff < pd || (t.diff == pd && std::abs(t.f - 1) < std::abs(pick - 1)))) pick = t.f, pd = t.diff;
        bestHw[i] = hh * pick;
        Params p = prm;
        p.hw = bestHw[i];
        solve(r, s, p);
    }

    if (cmd == "summary") {
        std::printf("ticks %zu obstacles %zu inputs %zu objects %zu\n", r.ticks.size(), r.obs.size(), r.inputs.size(), r.cands.size());
        std::puts("part p mini  ticks      x0      x1   hh   hw  fHaz fSol anom  match clicks tight diff pieces");
        for (size_t i = 0; i < secs.size(); i++) {
            auto const& s = secs[i];
            int tight, diff;
            stats(s, tight, diff);
            double hh = 0;
            for (auto t : s.ti) hh += r.ticks[t].hh;
            std::printf("%4zu %d %4d %6zu %7.0f %7.0f %4.1f %4.2f %5.2f %4.2f %4d %5.1f%% %6zu %5d %4d %6zu\n", i + 1, s.p, s.mini, s.ti.size(), s.x0,
                        s.x1, hh / s.ti.size(), bestHw[i], s.fHaz, s.fSolid, s.anomalies, s.score, s.clicks.size(), tight, diff, s.pieces.size());
        }
        return 0;
    }

    if (isSection) {
        if (argc < 5) return 1;
        size_t k = (size_t)std::atoi(argv[3]) - 1;
        if (k >= secs.size()) return 1;
        auto const& s = secs[k];
        FILE* o = std::fopen(argv[4], "w");
        for (size_t j = 0; j < s.ti.size(); j++) {
            auto const& t = r.ticks[s.ti[j]];
            std::fprintf(o, "T,%f,%f,%f,%f,%f,%f,%f\n", t.x, t.y, t.hh, s.chan[j].lo, s.chan[j].hi, s.chan2[j].lo, s.chan2[j].hi);
            for (uint32_t q = t.first; q < t.first + t.count; q++) {
                auto const& ob = r.obs[q];
                std::fprintf(o, "O,%f,%f,%f,%d,%d\n", t.x, ob.lo, ob.hi, ob.type, ob.cand >= 0 ? r.cands[ob.cand].id : -1);
            }
        }
        for (auto const& p : s.pieces) {
            std::fprintf(o, "P,%d", p.ceil);
            for (int q = 0; q < p.n; q++) std::fprintf(o, ",%f,%f", p.poly[q].x, p.poly[q].y);
            std::fprintf(o, "\n");
        }
        for (size_t c = 0; c < s.clicks.size(); c++) {
            auto const& t = r.ticks[s.ti[s.clicks[c]]];
            std::fprintf(o, "C,%f,%f,%d,%d,%d,%d\n", t.x, t.y, s.winLo[c], s.winHi[c], s.genLo[c], s.genHi[c]);
        }
        std::fclose(o);
        // problems: where the slope version differs, and objects the bot path went through
        int shown = 0;
        for (size_t j = 0; j < s.ti.size() && shown < 20; j++) {
            auto const& t = r.ticks[s.ti[j]];
            if (std::abs(s.chan[j].lo - s.chan2[j].lo) <= 1.5 && std::abs(s.chan[j].hi - s.chan2[j].hi) <= 1.5) continue;
            std::printf("differs  x %.1f y %.1f  original %.1f..%.1f  slope %.1f..%.1f\n", t.x, t.y, s.chan[j].lo, s.chan[j].hi, s.chan2[j].lo,
                        s.chan2[j].hi);
            shown++;
            while (j + 1 < s.ti.size() && r.ticks[s.ti[j + 1]].x - t.x < 15) j++;
        }
        shown = 0;
        for (size_t j = 0; j < s.ti.size() && shown < 20; j++) {
            auto const& t = r.ticks[s.ti[j]];
            for (uint32_t q = t.first; q < t.first + t.count; q++) {
                auto const& ob = r.obs[q];
                double e = t.hh * (ob.type == 0 ? s.fHaz : s.fSolid);
                if (t.y <= ob.lo - e || t.y >= ob.hi + e) continue;
                std::printf("through  x %.1f y %.1f  type %d span %.1f..%.1f  object id %d at (%.0f,%.0f)\n", t.x, t.y, ob.type, ob.lo, ob.hi,
                            ob.cand >= 0 ? r.cands[ob.cand].id : -1, ob.cand >= 0 ? r.cands[ob.cand].x : 0, ob.cand >= 0 ? r.cands[ob.cand].y : 0);
                shown++;
            }
        }
        return 0;
    }

    if (cmd == "pieces") {
        std::string out = argc > 3 ? argv[3] : "";
        if (out.empty() || out == "-") {
            out = argv[2];
            out = out.substr(0, out.size() - 4) + ".pieces.txt";
        }
        FILE* o = std::fopen(out.c_str(), "w");
        for (auto const& s : secs) {
            std::fprintf(o, "section %d %.3f %.3f %.1f%%\n", s.p, s.x0, s.x1, s.score);
            for (auto const& p : s.pieces) {
                std::fprintf(o, "piece %d %d %.4f %.4f %d %d %d %.6f %.6f %d %.3f %d", (int)p.kind, p.base, p.c.x, p.c.y, p.fx, p.fy, p.rot, p.sx, p.sy,
                             p.ceil, p.tight, p.n);
                for (int q = 0; q < p.n; q++) std::fprintf(o, " %.4f %.4f", p.poly[q].x, p.poly[q].y);
                std::fprintf(o, "\n");
            }
        }
        std::fclose(o);
        std::printf("wrote %s (%zu parts)\n", out.c_str(), secs.size());
        return 0;
    }
    return 1;
}
