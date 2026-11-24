# slop

Geode mod for Geometry Dash 2.2081: turn the wave parts of a level into exact slope corridors.

## How it works

The wave's x position depends only on time, never on clicks. So at a given x every possible
path meets the level in the same state - moved, rotated and toggled objects included.
That turns a moving level into a static map of where the wave survives, and that map is
what the slope version has to reproduce.

1. **Record.** Play the level with a bot (or noclip / practice). Every physics step the mod stores
   the wave hitbox, its clicks and every obstacle in its column as it is at that moment.
2. **Solve.** Forward pass: everywhere the wave can get. Backward pass: everywhere it can still
   finish from. Their overlap around the bot's path is the corridor. Hitbox size is calibrated
   from the bot path (it survived, so the real hitbox never touched anything).
3. **Fit.** Corridor edges become slopes (45°, 63.4°, 26.6°), blocks (flat, steps) or scaled
   slopes for other angles, pushed out by the hitbox so the corridor stays exactly the same.
4. **Check.** The same analysis runs on the generated pieces: `match %` per part, magenta dots
   where it differs, and per click how many ticks early/late still work (red = 0-1, orange = 2-3),
   compared with the original.
5. **Test.** `Test` replays the bot's clicks in a playtest and reports where (if anywhere) the
   wave dies. Turn noclip and bot playback off for it.

## Editor bar

`<` `>` part, `-` `+` widen/narrow the part, `Apply` replaces the part (objects that ever reached
the wave's column there + deco are removed, the slope version is placed; two Ctrl+Z),
`All`, `Test`, `Hide`, `Del` (delete the recorded route).

Status: `~N path-dependent` = speed/teleport portals, collision blocks or touch-triggered
triggers in the part (timing there may depend on the path), `!N through objects` = the
recorded path went through something (noclip or wrong slope orientation setting).

Use Apply on a copy of the level. Slope set = slope picked in the build tab or a selected
placed slope.

## Drawing

| Input | Result |
|---|---|
| Ctrl + drag | floor |
| Ctrl + Shift + drag | ceiling |
| Ctrl + Alt + drag | corridor (`Corridor gap`) |
| release Ctrl first | cancel |

Built by GitHub Actions (Windows).
