# AGENTS.md — fixing a slop level without any prior context

You are asked something like "поправь <level>" / "fix <level>". This file is enough to do it.
Deeper background (architecture, history, lessons): `CLAUDE.md` in this folder.

## What slop is

A Geometry Dash 2.2081 Geode mod. While a level is played (normally by a bot), it records every
physics step of the wave: the hitbox and every obstacle in its vertical column at that moment.
From that it builds a **slope version** of each wave part: a corridor of slopes/blocks that gives
the wave exactly the same room and the same click timing windows as the original level.

"Fixing" a level = making the slope version match the original as closely as possible. You do
it **offline**: compute pieces with `tools/slop_tool.exe`, the mod picks them up with `Load`.
No mod rebuild, no game needed on your side.

## Files

Routes dir: `%LOCALAPPDATA%\GeometryDash\geode\mods\sennot.slop\routes\`

| file | meaning |
|---|---|
| `<Level_Name>.bin` | the recording (level name, non-alphanumerics -> `_`) |
| `<Level_Name>.pieces.txt` | offline pieces you write; mod uses them after `Load` |
| `<Level_Name>.prev.bin` | previous recording |
| `<Level_Name>.applied` | Apply was used on that level in the editor |
| `<Level_Name>.slope.bin` | a run on the already converted level (analysis only, never the source) |

A copy named `Name 2` / `Name copy` uses `Name.bin` + `Name.pieces.txt` when it has none of its own.
Pick the newest non-`slope`/`prev` `.bin` matching the level the user names (ask only if two
different levels match).

## Recipe

```powershell
$r = "$env:LOCALAPPDATA\GeometryDash\geode\mods\sennot.slop\routes"
cmd /c D:\claude\slop\tools\build.bat              # only if tools\slop_tool.exe is missing / core changed
D:\claude\slop\tools\slop_tool.exe summary "$r\<Level_Name>.bin"
```

`summary` already searches, per part, the hitbox width / fit tolerance / wall scale that make
the slope version match best. Columns that matter:

| column | good | if not |
|---|---|---|
| `anom` | 0 | the bot path goes through something -> see "Through objects" |
| `fHaz`, `fSol` | 1.00 | <1 = hitbox shrunk by calibration; find what the bot touched |
| `match` | >= 99.5% | parts of < 30 ticks (mode switch bits) at 90-95% are fine |
| `diff` | 0 | clicks whose tick window differs from the original |

When it looks good:

```powershell
D:\claude\slop\tools\slop_tool.exe pieces "$r\<Level_Name>.bin"
```

Tell the user (Russian, short): parts and match %, then: open the level (a **fresh copy** if
`<Level_Name>.applied` exists) in the editor -> `Load` -> pick a slope in the build tab -> `All`.

## When something is off

Look at one part:

```powershell
$S = "<your scratch dir>"
D:\claude\slop\tools\slop_tool.exe section "$r\<Level_Name>.bin" <part> "$S\p.csv"   # prints problems
& "$env:USERPROFILE\miniconda3\python.exe" D:\claude\slop\tools\plot.py "$S\p.csv" <x0> <x1>   # PNG, open it with Read
```

`section` prints `differs` lines (where the slope corridor differs, original vs slope, units) and
`through` lines (objects the bot path overlaps: type 0 hazard, 1 solid, 2 slope, object id).
The CSV also has `C` rows: click x, y, original window -lo/+hi, slope window -lo/+hi.
Plot: black bot path, cyan/magenta corridor (dashed = on the slope version), yellow/orange
pieces, vertical lines = obstacles (red hazard, gray solid, blue slope, green ground).

Known causes and where they were fixed (all in `src/core.cpp` `solve()` unless noted):

| symptom | cause | fix that exists |
|---|---|---|
| `through` on saw ids (678-680, 1705-1710, 1734-1736) | circle hitbox | recorder uses `getObjectRadius` 16-gon (needs a new recording if the .bin is old) |
| `through` on slopes (type 2), corridor bulges | wave slides on slopes | obstacle is clamped at the path, slopes excluded from calibration |
| steady ~1-2 units off on steep walls, mini parts low | hitbox not square | `Params::hw`, searched by slop_tool |
| corridor ~20 units off after a merge | side corridor leaking into reachability | channel follows only the bot's tube |
| click window off by 1 tick | edge precision | sub-tick tolerance in `windows()`, search over tol / wall scale |
| dual: walls cross the middle, diamonds/fragments | each player's corridor walled alone | `separate()` drops pieces reaching into the other player's corridor |
| many tiny parts at the same x | practice retries in the recording | `slop_tool clean in.bin out.bin` |
| a broken stretch | bad recording there | `slop_tool cut in.bin out.bin x0 x1` (that part loses its walls) |

To change the algorithm: edit `src/core.cpp`, `cmd /c tools\build.bat`, rerun `summary`, check
other levels' `.bin` did not get worse (run `summary` on 2-3 of them), write `pieces`, commit
(`git add -A; git commit -F msgfile; git -c credential.helper= -c "credential.helper=!gh auth git-credential" push`).
Changes to `src/main.cpp` (recording, editor UI) need a mod build on GitHub Actions — read `CLAUDE.md` first.

## Do not

- do not rebuild/reinstall the mod for a per-level fix;
- do not delete user files in the routes dir (rename to `.bak` if needed);
- do not write long reports: the user wants the result and one line on what to press.
