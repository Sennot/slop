# slop — notes for the next session

Geode mod (GD 2.2081, Windows only) that turns the wave parts of a level into exact slope
corridors, plus Ctrl+drag slope drawing. User talks Russian, is low on tokens: answer short,
do not re-derive what is written here, do not start unasked changes.

## Build / install

- Mod builds **only on GitHub Actions** (public repo `Sennot/slop`, `.github/workflows/build.yml`:
  windows-2022, `sdk: given` = mod.json `geode 5.10.1`, bindings pinned to `7f6c2a7…` +
  `-DSKIP_BUILDING_CODEGEN=OFF`). Do not set up a local Geode build.
- Push: `git -c credential.helper= -c "credential.helper=!gh auth git-credential" push`
  (the default credential manager hangs silently). Commit messages with quotes: `git commit -F file`
  (PowerShell 5.1 mangles `"` in native args). Write mod.json with the Write tool (no BOM).
- Install: download the run artifact (`gh run download <id> -R Sennot/slop`) and copy the `.geode`
  to `D:\SteamLibrary\steamapps\common\Geometry Dash\geode\mods\`.
- Bindings check before using a GD member: `gh api "repos/geode-sdk/bindings/contents/bindings/2.2081/GeometryDash.bro?ref=7f6c2a75742856de88dad354e576dcff8a28e881" -H "Accept: application/vnd.github.raw" > gd.bro`, then grep it.

## Where things are

- Routes (recordings): `%LOCALAPPDATA%\GeometryDash\geode\mods\sennot.slop\routes\<level name>.bin`
  (non-alnum -> `_`). A copy `Name 2` / `Name copy` falls back to `Name.bin` if it has none.
  `<name>.pieces.txt` next to the route = offline pieces (see below). `<name>.applied` = Apply was
  used on that level; later runs on it are saved to `<name>.slope.bin` instead (never over the route).
- Game log: `D:\SteamLibrary\steamapps\common\Geometry Dash\geode\logs\` — lines `[slop]`:
  `route saved: N ticks…`, `apply part i: matched/removed/placed`, `template id (one/two)`.
- `src/core.*` — all geometry, Geode-free. `src/main.cpp` — hooks, recorder, editor bar.

## How it works (short)

- Recording (`GJBaseGameLayer::checkCollisions` hook, every physics step, only while the wave is
  on, PlayLayer only unless setting `record-editor`): wave hitbox (`getObjectRect`, big hh=5,
  mini hh=3) + for every Solid/Hazard/Slope overlapping the hitbox's x strip its vertical span
  right now (slopes = triangles from flips/rotation, rotated = `m_orientedBox` corners, saws =
  16-gon from `getObjectRadius`), ground/ceiling from the ground layers. Clicks via
  `handleButton` hook, stored by player x (x does not depend on inputs). Respawn = x jumps back
  > 5 -> truncate.
- `split()` cuts ticks into parts (per player, mini/big, teleports). `solve()`:
  per-object closest approach -> hitbox calibration (only if >=4 objects agree);
  free space per tick (obstacles the bot provably went through are ignored at that tick);
  forward/backward reachability **inside the bot's own tube** -> channel; walls = channel edge
  pushed out by the hitbox (`ev + |slope|*eh` vertically, `eh` sideways for steps) -> RDP fit ->
  pieces (45° 1x1, 63.4° 2x1 rot 90, 26.6° 2x1, flat = 30-thick stretched block, steps = riser
  blocks, other angles = scaled 1x1); then the same analysis on the generated pieces gives
  `match %` (ticks within 1.5 units) and per-click tick windows (original vs slope version).
- Editor bar: `< > - + Apply All Test Hide Load Del`. Apply deletes recorded objects of the part
  (matched by id + start position) and deco around it, places pieces with the build-tab /
  selected slope family, two undo steps. Test replays the clicks in a playtest
  (`processCommands` hook), fails on `playerDestroyed` or a math hazard overlap.

## Debug loop (no game, no rebuild)

```
tools\build.bat                                   # MSVC -> tools\slop_tool.exe
tools\slop_tool.exe summary <route.bin>            # per part: match %, calibration, anomalies
tools\slop_tool.exe section <route.bin> <part> p.csv   # prints where it differs / goes through
python tools\plot.py p.csv [x0 x1]                 # PNG, look at it with Read
tools\slop_tool.exe pieces <route.bin>             # writes <route>.pieces.txt -> user presses Load
tools\slop_tool.exe clean <in.bin> <out.bin>       # drop practice retries
tools\slop_tool.exe cut <in.bin> <out.bin> x0 x1   # drop a broken stretch
```
Python: `%USERPROFILE%\miniconda3\python.exe` (matplotlib installed).
Algorithm fixes: change `src/core.cpp`, rebuild the tool, `pieces` -> user presses `Load` then
`Apply`. Commit the core change too so the next mod build has it.

## Lessons (cost real time)

- Saws are circles; modelling them as squares made the bot "fly through" them and wrecked
  calibration and the channel (Ringy Paracosm, v0.3.0 "ничего не совпало").
- Reachability must stay inside the bot's tube; a side corridor that merges later widened the
  channel by ~20 units where the slope version has no such corridor.
- A sloped wall touches the square hitbox with a corner: offset is `hh*(1+|slope|)`, not `hh`.
- Playing an applied copy used to overwrite its route — now `.applied` -> `.slope.bin`.
- Not verified in game: wave vs slope collision (slide or die), ground bounds from ground
  layers, replay precision with CBF. `Unrotated slope rises right` setting flips the slope model.
