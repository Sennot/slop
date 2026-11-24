#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Geode-free geometry: strokes and recorded wave routes -> slope pieces.
namespace slop {

struct V {
    double x = 0, y = 0;
};

enum class Kind { S45, S63, S26, Free, Flat };
enum class Wave { Auto, Big, Mini };

struct Piece {
    Kind kind = Kind::S45;
    int base = 1;       // 1 = 1x1 slope object, 2 = 2x1 slope object
    V c;                // object center, level units
    bool fx = false, fy = false;
    int rot = 0;        // degrees, clockwise
    double sx = 1, sy = 1;
    bool ceil = false;
    double tight = 1;   // preview color: 0 = no room, 1 = roomy
    V poly[4];          // outline, level units
    int n = 0;
};

// ---- drawing --------------------------------------------------------------------

struct Opts {
    double eps = 10;       // simplify tolerance, level units
    Wave wave = Wave::Auto;
    bool ceiling = false;
    bool corridor = false;
    double gap = 30;       // corridor: vertical distance floor -> ceiling
    double snap = 0;       // start point grid, 0 = off
    bool baseRight = true; // unrotated slope rises to the right
    bool rot63 = true;     // 63° piece = 2x1 slope rotated 90°, else 1x1 with scaleY 2
    std::size_t maxPieces = 600;
};

std::vector<std::size_t> simplifyIdx(std::vector<V> const& pts, double eps);
std::vector<V> simplify(std::vector<V> const& pts, double eps);
std::vector<Piece> build(std::vector<V> const& pts, Opts const& o);

// ---- recorded routes ------------------------------------------------------------

// Something that can kill or block the wave, as its vertical extent over the hitbox width
// at one physics step. type: 0 hazard, 1 solid, 2 slope, 3 ground/ceiling.
struct Obstacle {
    float lo, hi;
    std::uint8_t type;
    std::int32_t cand;
};

struct Tick {
    float x, y, hh; // hitbox center, half size
    std::uint8_t p, mini, brk;
    std::uint32_t first, count; // obstacles
};

struct Input {
    float x;        // player x when the button changed (x does not depend on inputs)
    std::uint8_t down, button, p2;
};

struct Cand {
    std::int32_t id;
    float x, y;     // start position
};

struct Route {
    std::vector<Tick> ticks;
    std::vector<Obstacle> obs;
    std::vector<Input> inputs;
    std::vector<Cand> cands;
};

struct Interval {
    double lo, hi;
};

struct Params {
    double window = 300;   // free space considered above/below the path
    double tol = .5;       // boundary fit tolerance
    double wallScale = 1;  // wall distance = hitbox half size * this
    double margin = 0;     // manual gap tweak per section
    bool haveTwo = true, baseRight = true;
};

struct Section {
    int p = 0;
    bool mini = false;
    std::vector<std::size_t> ti;     // ticks of this section
    double x0 = 0, x1 = 0;
    V mid;
    // results
    double fHaz = 1, fSolid = 1;     // hitbox calibration from the bot path
    int anomalies = 0;               // ticks where the path went through something
    std::vector<Interval> chan;      // per tick: where the wave center can be and still finish
    std::vector<Interval> chan2;     // the same on the slope version
    std::vector<Piece> pieces;
    double score = 0;                // % of ticks where the slope version gives the same room
    std::vector<V> bad;              // mismatching spots
    std::vector<int> clicks;         // tick index of each click
    std::vector<int> winLo, winHi;   // ticks a click can be early / late, original
    std::vector<int> genLo, genHi;   // same on the slope version
    std::vector<std::int32_t> cands; // objects that touched the wave's column
};

std::vector<Section> split(Route const& r);
void solve(Route const& r, Section& s, Params const& prm);

constexpr int kMaxPoly = 16; // convex polygon vertices (circles are 16-gons)
bool spanOver(V const* poly, int n, double x0, double x1, double& lo, double& hi);
void slopeTri(double cx, double cy, double w, double h, bool fx, bool fy, int rot, bool baseRight, V out[3]);

}
