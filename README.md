# slop

Geode mod for Geometry Dash 2.2081: draw wave slopes in the editor.

| Input | Result |
|---|---|
| Ctrl + drag | floor (solid below the line) |
| Ctrl + Shift + drag | ceiling (solid above the line) |
| Ctrl + Alt + drag | corridor: floor + ceiling, `Corridor gap` apart |
| release Ctrl before the mouse | cancel |

- Angles snap to 45° (big wave, 1x1 slope), 63.4° (mini wave, the 2x1 slope of the same set stood upright) and 0° (stretched block).
- Slope type: the slope picked in the build tab, or a selected placed slope (its colors, layers and groups are copied).
- Pieces are placed off-grid exactly along the stroke; the whole stroke is one undo step.

Built by GitHub Actions (Windows).
