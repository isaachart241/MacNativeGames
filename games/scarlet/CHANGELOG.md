# Pokémon Scarlet / Violet port: work log

A record of how this port is being made, kept the way `games/by3/CHANGELOG.md` is: each entry says what was found, what
was done and why, what went wrong, and how it was checked. Dates are absolute; "verified" means a check with a recorded
result. Game data, keys and saves are never committed.

## Status

| Stage | Goal | State |
|---|---|---|
| 0 | Feasibility: does the game fit the by3 architecture? | done 2026-10-25 |
| 1 | Code and data out of the dump; code rewritten for Apple silicon | done 2026-10-25 |
| 2 | The runtime fork boots to `nnMain` | done 2026-10-25 |
| 3 | Every unknown service / NVN call answered until the first `nvnQueuePresentTexture` | done 2026-10-25 (submission off) |
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

### 2026-10-25: stage 1, verified code and data

- **equiv.py** on all three rewrites, every rewritten site run against the original in an emulator: sdk 13,993 sites,
  subsdk0 10,541, main 25,330; 0 divergent; every site at least 2 effective trials; 300 of 300 mutation controls
  caught per module (the 118 + 190 host-gate sites are structck's). A second rewrite gives the pinned bytes again.
- **RomFS**: `scripts/extract_update_romfs.py` (Scarlet's copy of TotK's; only the Program NCA picker takes the title)
  merges base 1.0.0 + the 4.0.0 BKTR patch with the pinned, patched hactool from `build_app.build_hactool`. 99 s,
  9,925,204,188 bytes, all six IVFC levels GOOD, 30 files: `arc/data.trpfs` (7.47 GiB, Trinity) and `data.trpfd`,
  Wwise `.bnk` / `.pck` under `audio/wwise/bin/Switch`, Bink 2 movies (`.bk2`) under `demo`, small `event` and
  `system` files. Bink is linked into main (its strings are there), so the game decodes its own movies.
- `scripts/pin_version.py` (new) writes `assets/expected-v4.0.0.json` from the verified work folder. Hand-checked
  entries (below) are carried over, never regenerated. `.gitignore` covers the formats seen.

### 2026-10-25: stage 2, the game reaches `nnMain`

- `make prepare` stopped at one relative relocation in main pointing past the image: main+0x46cb7d8 holds
  main+0x47eb8e8, 0x188e8 past the end (0x47d3000). The neighbouring relocations are TLS bounds, main+0xbc..0x118 loads
  them and passes them to `__nnmusl_init_dso`, and nothing dereferences this one: it is the end of main's thread-local
  `.tbss`, which takes addresses but no image space. Rather than drop the check, `prepare_runtime.py` and the loader
  now accept exactly the relocations pinned in `relative_beyond_image` (with the reason in the pin file); anything
  else still stops.
- 337,026 relocations, the 30-file RomFS index and the six shared fonts. The runtime builds with Xcode 27.1.
- Headless boot: SDK init, `nninitStartup` (a 3.1 GiB heap). It called `nn::fs::SetAllocator`, which no host
  answered (the SDK version reaches `fsp-srv` to check the access log): answered as a no-op, since the host answers fs
  by name. Also `IsSaveDataExisting(Uid)` (one-argument form) mapped to the existing check.
- Then all module constructors (main 2,133, subsdk0 45, sdk 112) and **`nnMain`**.

### 2026-10-25: stage 3 (in progress)

Answered so far, in order:

- `nn::aoc::GetAddOnContentListChangedEvent`: the SDK's version reads the aoc service object `host_aoc_init` never
  made (null dereference). Now a system event that is never signalled (no DLC here).
- The game needs a window (VI). Windowed runs need the display awake: with the Mac's display asleep
  `CGGetActiveDisplayList` reports none and the display link cannot start ("native display link initialization").
  `make boot` is always headless (`TOTK_HEADLESS` set, even empty), so windowed runs call the binary directly.
- With a window: Metal device (Apple M3), NVN context, a 268 MiB memory pool, the NVN window (2 images,
  1920x1080), then **present interval 2: Scarlet presents at 30 fps**.
- NVN calls answered (`third_party/d2r_gfx`, only what the game called): `nvnTexturePoolGetSize` with the other
  texture / sampler pool getters, `nvnQueueBuilderSetQueuePriority` (1000, stored in the builder's padding; one Metal
  queue), `nvnBufferBuilderGet{Size,MemoryOffset,MemoryPool}`, `nvnMemoryPoolBuilderGet{Size,Flags,Memory}`. Two
  prototypes in `nvn_protos.h` are wrong (GetMemoryPool returns the pointer, GetMemory returns `void *`); those use
  `NVN_NOPROTO`.
- Audio comes up: Wwise opens `audren:u` (REV10, 48 kHz stereo through CoreAudio) and loads `PM.bnk` (the
  `English(US)` path first, as expected).

**The missing audio root (fixed).** The crash at main+0x14fe804 was the audio system's node tree: its root
(`this+0x58`) was never created. Traced back step by step (a write watchpoint showed nothing ever wrote it):

- main+0x1509880 creates the Wwise globals (`SourceGlobal`, `DefaultListener`) and the `Root` node, and stores it at
  `+0x58`. It runs from main+0x1505338 only after two gates: the sound engine init (main+0x1507028, returned 1) and
  main+0x1508a4c, which returned 0.
- That gate's first step, main+0x15096ec, hands the Wwise low-level IO its base path through a virtual call that must
  return `AK_Success` (1): Wwise's `SetBasePath`. It checks the folder exists with `nn::fs::GetEntryType` on
  `data:/audio/wwise/bin/Switch/`.
- **Host bug**: `runtime_romfs_entry_type` appended `/` to anything that is not a file, so a folder asked for with
  its trailing slash became `audio/wwise/bin/Switch//` and was reported missing. Fixed in `runtime/romfs.c` (no second
  slash). TotK and Bayonetta never asked for a folder that way.
- The audio system's init result was ignored by its caller, which is why the failure only showed later as a null
  dereference. Verified with lldb probes: base path 1, second gate 1, root init 1, init 1.

After the fix, Wwise opens its five packages (`Stream`, `LooseMedia`, `Patch00`, `SDC0`, `SDC2`) and loads all 17
banks, and the game reads `event/trevt/bin/event.trevt`.

Then, answered by name:

- `nn::hid::BindNpadStyleSetUpdateEvent`: an event created and never signalled (the Mac's controller keeps its
  style).
- `nn::pl::RequestSharedFontLoad`: the fonts load on first use.

**Fonts (fixed).** The null dereference at main+0x2151bc0 was the game's font manager: a glyph lookup for `?` in font
slot 2 found nothing. Slot 2 is `beluga_font` (an icon font, no ASCII) plus a font with an empty name: every system
shared font failed `nn::fontll::ScalableFontEngine::LoadFont` with 0x193, while the game's own fonts
(`RodinNTLGPro-DB`, `UDKakugoC80Pro-DB`, `beluga_font`) loaded. The game passes `nn::pl::GetSharedFontAddress` straight
to the low-level font engine, which takes plain TrueType; the host handed it the XORed BFTTF payload (the data after the
header starts 0x49631806: 0x00010000 XORed with the key 0x49621806). `runtime/shared_font.c` now decodes the payload in
place after the integrity check (key = header word ^ 0x7f9a0218) and checks for the TrueType magic. Games that use
NintendoWare's font library decode BFTTF themselves, which is why TotK and Bayonetta never showed it.

Answered by name after that, each the first time the game asked:

- `nn::bcat` (news and distributions) as a console that never connected: the delivery cache mounts and is empty, a sync
  is refused as unavailable, nothing opens. Constructors and destructors stay the SDK's.
- `nn::crypto::GenerateCryptographicallyRandomBytes`: `arc4random_buf` (csrng on the console).
- `nn::bluetooth` LE (the Pokemon GO Plus + accessory) as no device in range: `InitializeBle` succeeds (the game ignores
  its result), the five BLE events are created and never signalled.
- NVN multisample coverage modulation / coverage-to-color / sample-location grid, stored in the fields `ms_st` reserved;
  a non-default value is reported once (not drawn).

**The first draws.** The game then records command buffers and submits them. Its vertex shaders read NVN's draw
parameters from the driver constant buffer, c0 + 0x640 (Ryujinx's NvnBaseVertexByteOffset; 0x644 base instance,
0x648 draw index), which the replay did not fill and the prebuilt translator lists as unfillable. The replay now writes
them per draw for the vertex stage (`g_draw_params`, `driver_bank_fill`, included in the optional driver-bank cache
key), and `nvn_refl.m` accepts exactly those three offsets for vertex shaders. A SPIRV-Cross pixel shader declares the
function constant `spvLinearTextureAlignmentOverride` (65535), which Metal will not build unspecialized:
`shader_fn` specializes with no values, keeping SPIRV-Cross's default.

### 2026-10-25: milestone, the main loop runs

With GPU submission still off (the host's default), the game ran until the 2-minute limit stopped it: frame 3,500 at
about 30 fps (present interval 2, the main thread on a 33.33 ms real-time period), about 73 render passes and 53
distinct NVN functions a frame, audio output running without underruns (silent so far). Nothing reaches the screen:
the work is encoded and dropped.

Seen in that run, not fatal yet:

- 12 render pipelines refused by Metal: the vertex shader writes `render_target_array_index` (layered rendering, e.g.
  shadow cascades or cube maps), which needs the pipeline's input primitive topology set. Those draws are skipped.
- A texture-to-texture copy that scales (1920x1080 to 480x270) is not implemented.
- One draw sets different scissors per viewport; one pre-swizzled texture is not in a memory pool (CPU fallback).

Next: GPU submission (`TOTK_GPU_ALLOW_SUBMIT=1`) for a first picture, then those four.

Tools for the next step: `TOTK_FS_LOG=1` (every RomFS open), `TOTK_INPUT_FILE` with `hletrace 2` (first call of each
host function, in order), and lldb `--batch` with the inspection commands as `-k` (they run when the process stops on
the crash).
lldb helpers used here (kept outside the repository, easy to rewrite): a script that stops in the loader's
`relocate`, reads main's base from `modules[0].base`, and then sets breakpoints or watchpoints at main offsets. The
base changes every run because the loader maps main with mmap.
