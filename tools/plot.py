# Plots a part dumped by `slop_tool section <route> <part> <out.csv>`.
#   python tools/plot.py part.csv [x0 x1] [out.png]
# Vertical lines = obstacles in the wave's column per tick (red hazard, gray solid, blue slope,
# green ground/ceiling). Black = bot path, cyan/magenta = corridor (original), dashed = corridor
# on the generated slope version, yellow/orange = floor/ceiling pieces, dots = clicks
# (red: 0-1 tick window, orange: 2-3, white-ish: more; ring = window differs on the slope version).
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon

src = sys.argv[1]
x0 = float(sys.argv[2]) if len(sys.argv) > 3 else -1e18
x1 = float(sys.argv[3]) if len(sys.argv) > 3 else 1e18
out = sys.argv[4] if len(sys.argv) > 4 else src.rsplit(".", 1)[0] + ".png"
T, O, P, C = [], [], [], []
for line in open(src):
    f = line.strip().split(",")
    if f[0] == "T": T.append(list(map(float, f[1:])))
    elif f[0] == "O": O.append(list(map(float, f[1:])))
    elif f[0] == "P": P.append((int(f[1]), list(map(float, f[2:]))))
    elif f[0] == "C": C.append(list(map(float, f[1:])))
T = [t for t in T if x0 <= t[0] <= x1]
ys = [t[1] for t in T]
ylo, yhi = min(ys) - 120, max(ys) + 120
fig, ax = plt.subplots(figsize=(16, 7), dpi=90)
col = {0: "red", 1: "gray", 2: "blue", 3: "green"}
for x, lo, hi, ty, _ in O:
    if x0 <= x <= x1 and hi > ylo and lo < yhi:
        ax.plot([x, x], [max(lo, ylo), min(hi, yhi)], color=col[int(ty)], lw=0.6, alpha=0.45)
xs = [t[0] for t in T]
ax.plot(xs, [t[1] for t in T], "k-", lw=1)
ax.plot(xs, [t[3] for t in T], "c-", lw=1)
ax.plot(xs, [t[4] for t in T], "m-", lw=1)
ax.plot(xs, [t[5] for t in T], "c--", lw=0.8)
ax.plot(xs, [t[6] for t in T], "m--", lw=0.8)
for ceil, pts in P:
    xy = list(zip(pts[0::2], pts[1::2]))
    if any(x0 - 50 <= p[0] <= x1 + 50 for p in xy):
        ax.add_patch(Polygon(xy, closed=True, fc=("orange" if ceil else "yellow"), ec="k", lw=0.4, alpha=0.6))
for x, y, lo, hi, glo, ghi in C:
    if x0 <= x <= x1:
        w = lo + hi
        ax.plot(x, y, "o", ms=5, color="red" if w <= 1 else "orange" if w <= 3 else "lightgray", mec="k", mew=0.4)
        if (lo, hi) != (glo, ghi):
            ax.plot(x, y, "o", ms=9, mfc="none", mec="magenta", mew=1)
ax.set_xlim(max(x0, xs[0]), min(x1, xs[-1]))
ax.set_ylim(ylo, yhi)
ax.set_aspect("equal")
plt.tight_layout()
plt.savefig(out)
print(out)
