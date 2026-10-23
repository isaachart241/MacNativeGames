# Pokémon Scarlet / Violet port: work log

A record of how this port is being made, kept the way `games/by3/CHANGELOG.md` is: each entry says what was found, what
was done and why, what went wrong, and how it was checked. Dates are absolute; "verified" means a check with a recorded
result. Game data, keys and saves are never committed.

## Status

| Stage | Goal | State |
|---|---|---|
| 0 | Feasibility: does the game fit the by3 architecture? | ready to run (needs the dump) |
| 1 | Code and data out of the dump; code rewritten for Apple silicon | |
| 2 | The runtime fork boots to `nnMain` | |
| 3 | Every unknown service / NVN call answered until the first `nvnQueuePresentTexture` | |
| 4 | Picture, audio, input, saves; frame-rate patches | |
| 5 | Performance, shaders, packaging as an app | |

## Log

### 2026-10-23: the fork, and the stage 0 survey

- `caec8d3`: `games/by3` copied unchanged to `games/scarlet`.
- The next commit renames only the identity: work folder `SCARLET_build`, binary `scarlet-native`, app
  `Pokemon Scarlet.app` (`org.macnativegames.scarlet`), `SCARLET_*` environment variables, `[scarlet]` log tags,
  `runtime_scarlet_override`, and title ID `0100A3D008C5C000` (unverified; Violet `01008F6008C5E000`, `TITLE=` in the
  Makefile). `BY3_HUD_RENDER` keeps its name because the shared graphics layer reads it. Comments that say why code
  exists ("Bayonetta 3 needs …") are kept as history. `third_party/` and `prebuilt/` are byte-identical to by3's, so
  fixes merge across.
- Bayonetta's pinned hashes (`assets/expected-v1.2.0.json`) and work log were removed.
- `scripts/survey.py` (new): stage 0 in one command, reading the extracted ExeFS without running it.
  `tests/test_scarlet_survey.py` checks it on a synthetic module and npdm. The test caught a bug before any real use:
  the first pattern for NVN names consumed the NUL between two packed names and missed every second one.
  The graphics layer's implemented set is read from all three registration macros (`NVN`, `NVN_NOPROTO`, `NVN_EXTRA`):
  431 names at this commit.

Not checked yet: anything about Scarlet itself.
