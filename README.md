# slop

Geode mod for Geometry Dash 2.2081: turn wave parts of a level into readable slope corridors.

## Route (semi-auto)

1. Play the level (PlayLayer or editor playtest; noclip, bot macro or practice all work).
   Every physics step the mod records the wave hitbox and the free space above and below it
   against everything that can kill it **at that moment** - move/rotate/toggle triggers included.
   Re-recording a part replaces only that x range.
2. In the editor the bar above the toolbar shows the proposal: path + floor/ceiling slopes, red = tight.
   Walls run parallel to the path at the tightest distance of each straight piece.
3. `<` `>` section, `-` `+` gap, `Place` / `All` put slopes on the current editor layer
   (one Ctrl+Z per section), `Hide` overlay, `Del` deletes the recorded route.

Pieces: 45° = 1x1 slope, 63.4° = 2x1 stood upright, 26.6° = 2x1, flat = stretched block,
anything else = scaled 1x1. Slope set = the slope picked in the build tab or a selected placed slope.

## Drawing

| Input | Result |
|---|---|
| Ctrl + drag | floor |
| Ctrl + Shift + drag | ceiling |
| Ctrl + Alt + drag | corridor (`Corridor gap`) |
| release Ctrl first | cancel |

Built by GitHub Actions (Windows).
