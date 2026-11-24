#pragma once
#include <cstddef>
#include <vector>

// Geode-free geometry: stroke -> slope pieces.
namespace slop {

struct V {
    double x = 0, y = 0;
};

enum class Kind { S45, S63, Flat };
enum class Wave { Auto, Big, Mini };

struct Piece {
    Kind kind = Kind::S45;
    V c;            // object center, level units
    bool fx = false, fy = false;
    int rot = 0;    // degrees, clockwise
    double sx = 1, sy = 1;
    bool ceil = false;
    V poly[4];      // outline for preview
    int n = 0;
};

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

std::vector<V> simplify(std::vector<V> const& pts, double eps);
std::vector<Piece> build(std::vector<V> const& pts, Opts const& o);

}
