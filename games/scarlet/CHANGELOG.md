# Pokémon Scarlet / Violet port: work log

A record of how this port is being made, kept the way `games/by3/CHANGELOG.md` is: each entry says what was found, what
was done and why, what went wrong, and how it was checked. Dates are absolute; "verified" means a check with a recorded
result. Game data, keys and saves are never committed.

## Status

| Stage | Goal | State |
|---|---|---|
| 0 | Feasibility: does the game fit the by3 architecture? | done 2026-10-25 |
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

### 2026-10-25: stage 0, feasibility

Inputs: base 1.0.0 (`0100A3D008C5C000`, XCI, 7.44 GiB), update 4.0.0 (`0100A3D008C5C800`, NSP, 4.88 GiB), `prod.keys`
and firmware 22.5.0 from the same Switch. Title, update ID and both versions were read from the files' own control
data (NACP display version), not from file names. The title ID guessed on 2026-10-23 was right.

Findings, each read from the dump:

- **The base is not sparse.** The XCI holds a full 6.4 GiB Program NCA; the update's Program NCA is a BKTR patch over
  it. That is Tears of the Kingdom's shape with an update, not Bayonetta's, so the RomFS merge (stage 1) follows
  `build_app.py`, not `scripts/merge_romfs.py`. The update's ExeFS is complete and needs no merge.
- **Same module layout as TotK and by3**: `main` (build `421C5411B487EB4D049DD065FEC9547773E8E598`, 71.8 MiB mapped),
  `sdk`, `subsdk0` (movie), `rtld`, `main.npdm`. `scripts/extract_code.py` took them from the update unchanged, every
  ExeFS block verified.
- **SDK 13.3.13**, the same generation as Bayonetta's 13.3.2. NVN 13.3.13.
- **Middleware** (strings in main, SDK MW tags): Wwise, Havok AI 2021.1, NintendoWare G3d / Ui2d / Vfx / Font,
  Nintendo's online SDK (NPLN 1.8.3, gRPC, OpenSSL, libcurl), Pia 6.20.1 (P2P), movie library with VP9 and H.264.
- **NVN**: main names 567 entry points (by3: 533). 429 are implemented by this tree's graphics layer, 7 are deliberate
  no-ops, 131 are not. Of those 131, 46 are debug / GL / CUDA interop / NVX names and 51 are getters; 34 could be
  called by a renderer. The ones that would need real work if called: transform feedback (Begin / End / Pause / Resume /
  Draw, buffer binds), tessellation levels, integer clears (`ClearColori/ui`, `ClearTexturei`), `Downsample` /
  `TiledDownsample`, `DrawTexture`, `ReportValue`, `ResolveDepthBuffer`, viewport swizzles, sample shading. Like
  by3, the gfx library looks up every name at start, so stage 3 will show which are really called.
- **Services**: main.npdm allows 49. The ones by3 never met: `bt` / `btm:u` (34 nn::bluetooth imports, likely the
  Pokémon GO Plus + accessory), `ldn:u` (27 nn::ldn, local wireless for Union Circle and trades), `bcat:u` (22),
  `audin:u`, `irs`, `nfc` / `nfp` (amiibo), `mii:u`, `ngct:u` (profanity filter), `nim:eca`, plus `web`, `album`
  and `ec` imports. All are expected to need offline answers, as by3's network services got.
- **SDK imports main makes and the host replaces by name**: fs 25 of 31, oe 13 of 19, hid 21 of 56, account 10 of
  25, nifm 6 of 8, time 7 of 18, swkbd 6 of 11, vi 4 of 7, pl 3 of 4. os (106), util (44), mem, lmem, crypto and
  fontll are library code that runs as it is; audio (42) goes through `audren:u` / `audout:u`, which the host answers.
  bluetooth (34), ldn (27), bcat (22) and socket (29, offline `bsd:u` exists) have nothing yet.

**Rewrite.** `patch_nso.py` with the build's variables (`DM_RULESET=native DM_PRIVATE_GATES=0`) rewrote all three
modules with no change to the rewriter: sdk 14,111 sites (76 svc, 42 counter reads), subsdk0 10,541 (all x18),
main 25,520 (19,896 x18, 5,424 thread-pointer reads, 190 svc, one `spset` site, the kind by3 added in `29034df`).
main took 79 s on the M3.

Verified: `structck.py` OK for all three modules, 0 failures each, against the independent llvm-objdump count
(main: independent count 25,529, rewrite sites 25,520, x18 references inside rewrite windows 19, as structck reports
them). Not run yet: `equiv.py`.

Also: the six system fonts were extracted from firmware 22.5.0 with these keys (`extract_shared_fonts.py`: all six
IVFC levels and BFTTF headers pass). `TOTK_GAME_DIR` and the scripts' paths now use `v400`; `TOTK_GAME_VERSION` is
4.0.0.

Tooling fixed on the way, in `scripts/survey.py`: names taken over by `HLE_FORCE` (`nvnBootstrapLoader`) and the
`g_nops` list were counted as missing; the services section said "never mentioned by the host" for services the host
answers by replacing SDK functions. It now reports per-library import coverage instead. Both are covered by
`tests/test_scarlet_survey.py`.
