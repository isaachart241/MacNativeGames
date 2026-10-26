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
| 4 | Picture, audio, input, saves; frame-rate patches | in progress: picture and input to the first save |
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

### 2026-10-25: the first picture

One 75-second run with `TOTK_GPU_ALLOW_SUBMIT=1` (GPU memory budget at its default, 3,276 MB on this 8 GB Mac):
no crash, about 30 fps (screenshots at frames 269, 567, 1018), and the game's first-launch screen, "Which language
will you play in?", drawn correctly at 1920x1080: the game's own title font, the highlighted entry and its cursor,
and the Japanese, Korean and Chinese names in the system shared fonts the font fix made loadable. The game waits
there for a button. Screenshots stay in `SCARLET_build/shots` (ignored by git: they are pictures of the game).

**Layered vertex shaders (fixed).** The 12 refused pipelines: Scarlet's vertex shaders write the layer themselves
(`gl_Layer [[render_target_array_index]]`), which needs the pipeline's input primitive topology class, and they also
write a point size, which Metal only allows with the point class. `shader_load` now records `writes_layer` from the
MSL (the source is not kept in memory unless `TOTK_KEEP_MSL=1`), `pso_desc` sets the triangle class for such shaders,
and `vs_fn_for` gives them a variant with the point size kept as a local (`nopsz`), the same treatment TOTK's
layered passes get. Checked with submission off: 0 refused pipelines.

Still seen: a scaling texture-to-texture copy (1920x1080 to 480x270, not implemented), per-viewport scissors, one
pre-swizzled texture outside a memory pool (CPU fallback).

### 2026-10-25: input; past the language screen

A press (`key 38`, J = A) had no effect at first: the game only asked about the handheld pad (0x20), never player 1.
Scarlet reads a pad's style only after that pad's style-set update event fires; on the console it fires when a
controller connects, the ones connected at start included, and the stub from earlier never signalled it.
`bind_style_event` now signals it once for the pad in use. Then, in order:

- `nn::hid::GetNpadJoyAssignment`: dual (the keyboard / Pro Controller style pad is one unit).
- `nn::os::CreateThread` aborted in the SDK: it moves a thread stack into the stack region with svcMapMemory, and the
  host only accepted heap or alias memory as the source. Scarlet keeps one stack in main's .bss (main+0x46fd000);
  `stack_move` now also accepts a module's data / .bss (`runtime_module_data` in loader.c).
- Vibration: Scarlet imports the public `nn::hid::InitializeVibrationDevice` / `SendVibrationValue` (TotK the
  `detail::` ones); both now reach `hid_feedback.c` the same way (rumble off by default: no vibration devices).
- **Transform feedback (not emulated)**: Bind / Begin / End (and Pause / Resume / DrawTransformFeedback) record nothing,
  reported once each. The captured buffers stay empty and DrawTransformFeedback draws are dropped; whatever the game
  uses it for will be missing until it is emulated (Metal has no stream-out: a vertex function writing to a buffer, or
  compute, is the way).

Short run with GPU submission on (50 s, no crash, frames 331..1060 at about 30 fps): A on the language screen gives
**"Choose a look for yourself."** with the eight character portraits drawn and shaded, the button prompts' icons (from
`beluga_font`) and the animated "Use the Left Stick". The screen waits for a stick direction.

Audio: the renderer path's level monitor reads 0 throughout; Wwise mixes to AudioOut, which has no level monitor, so
whether sound is heard is not checked yet.

Tools for the next step: `TOTK_FS_LOG=1` (every RomFS open), `TOTK_INPUT_FILE` with `hletrace 2` (first call of each
host function, in order), and lldb `--batch` with the inspection commands as `-k` (they run when the process stops on
the crash).
lldb helpers used here (kept outside the repository, easy to rewrite): a script that stops in the loader's
`relocate`, reads main's base from `modules[0].base`, and then sets breakpoints or watchpoints at main offsets. The
base changes every run because the loader maps main with mmap.

### 2026-10-25: transform feedback, the colour LUT, enrollment complete

**Transform feedback (emulated).** Four vertex shaders capture varyings (points, 3 buffers, rasterizer discard). NVN
has no API for the layout; it is in the GLSLC control section of each vertex program: buffer count at +0x6ec, and per
buffer b a varying code at +0xdc + 0x80·b (0x20 + N is `locnN`), its component count at +0x4dc + 0x80·b, the stride
(u16) at +0x6dc + 2·b and an enable byte at +0x6e8 + b. Read for all four shaders; `tf_layout` rejects anything else.
Bind / Begin / End / Pause / Resume now record `OP_TF_BIND` / `OP_TF_STATE`, and a capture draw (non-indexed point,
line or triangle list) uses a vertex function variant (`vs_fn_tf`) that writes each captured varying to
`buffer + slot * stride` before returning, slot = (vertex - first) + (instance - base instance) * count, with offsets
advancing per draw as the hardware's do. With rasterizer discard the variant returns void and the pipeline has no
fragment function, as Metal requires. A vertex-to-vertex/fragment barrier follows each capture draw.
`DrawTransformFeedback` is still dropped (reported once); not seen called yet.

**The colour grading LUT.** The tone-map pass samples a 32x32x32 3D texture that one draw fills: one triangle,
32 instances, the vertex shader writing layer = instance + 1 (the tone-map shader samples one slice up to match).
Opened as a plain pass, Metal ignored the layer and every instance landed on slice 0. `lay_self` now opens a layered
pass over every layer of the targets when the draw's vertex shader writes the layer itself (no depth, no pending
clear), and a draw like that inside a plain pass reopens it. Note for reading pass dumps: the `d` line printed 1 as
the instance count of every non-indexed draw; it prints the real count now.

**My regression, found the long way.** Adding the capture variant in `pso_desc` put the line that sets
`d.fragmentFunction` at the end of a comment, so from then on every pipeline had no fragment function. Every screen
came out magenta, which is the swapchain memory's initial content showing through. It looked like a missing pass
after the name prompt (the screenshots before that were not retaken), so the search went through the LUT above
(a real bug) before `DM_NVN_HILITE` showed that a draw with a replaced fragment function did land. Fixed by putting
the assignment back on its own line. Lesson kept: retake an early screenshot after any change to `pso_desc`.

Also fixed on the way:
- The warm-up thread built a cached transform feedback pipeline from a shader loaded without its control section and
  aborted; capture keys are skipped there and built on first use.
- `pool_range_check` refused a 16 KiB memory pool whose two host pages were each write-protected by a different
  neighbouring pool's dirty tracking (pools are 8 KiB aligned, host pages 16 KiB). Every page is now checked on its own.

Checked, GPU submission on, 60 s, no crash, about 30 fps: language, look, the name prompt (software keyboard,
`kbtype Thor`), then **"Are you ready to start the game with these settings?"** with the 3D character model, the
Naranja Academy enrollment application with language and name filled in, and **"Enrollment complete!"** with the
autosave notice. The photo on the application is black (the scaled texture-to-texture copy is the first suspect).

Audio: the AudioOut level line reads rms 0 with 0 nonzero callback frames over every 5 s window, so nothing audible
is produced yet.

### 2026-10-26: the opening plays; the player's room and the first save

With the fixes below, A presses alone carry the game from "Enrollment complete!" through the whole opening: Naranja
Academy, the classrooms and library, Director Clavell's introduction ("Director Clavell, Head of Naranja Academy"),
the courtyard and front gate, the "Pokémon Scarlet" title over Paldea, a fly-over of Mesagoza and the lighthouse, then a
loading screen, and gameplay: the player's room, "Now saving..." and the player at the desk with a "Get Up" prompt
(left stick). The first save is written: `user-data/save/<id>/main`, 4.4 MB.

- **Controller colours.** At about 250 s, as gameplay starts, the game aborted in the SDK (result 0x58cca). The call
  stack named `SetControlPadRestrictionType`, but that frame was a return address at the start of the next symbol: the
  aborting function is `nn::hid::GetNpadControllerColor(left, right, id)`, whose last instruction is the call to abort.
  Both colour queries are now answered by the host (neon blue / neon red Joy-Cons, a dark Pro Controller), as the SDK's
  own code reads hid shared memory this host never maps. Lesson kept: when a frame sits at +0x0 of a function, look at
  the function before it.
- **The first save.** Right after that the game crashed in `nn::oe::EnterExitRequestHandlingSection` (a null pointer in
  the SDK's applet state). Scarlet brackets each save with Enter / LeaveExitRequestHandlingSection; the host has no HOME
  menu exit request to hold back, so both are now host functions that only count the nesting depth.
- **Scaling texture copies.** `copy_t2t` reported 1920x1080 to 480x270 copies as not implemented (one colour target,
  RG11B10Float, and one Depth32Float_Stencil8). Both are now drawn as one triangle over the destination rectangle:
  colour sampled nearest or linear as the copy flags say, depth (and stencil, through an X32_Stencil8 view) read texel
  by texel and written from the fragment. Colour, non-integer and depth formats only; mirroring and format changes are
  still reported. The black photo on the enrollment form is not fixed by this.
- **RomFS reads.** One run stopped with "RomFS read failure": `pread` into guest memory returns EFAULT when the replay
  write-protects the page again between `dm_guest_write_prepare` and the read (the kernel cannot take the dirty
  tracker's fault). RomFS and save reads now prepare again and retry on EFAULT / EINTR, and report errno if they still fail.
- **Memory pool buffers.** One run stopped because Metal refused the zero-copy buffer for a pool in pages the game had
  just mapped. The pool path now waits for the GPU, lets deferred releases go and tries again, and logs the VM regions
  of the range if the second try fails too.

Audio: Scarlet's Wwise output goes through AudioOut (48 kHz stereo, to Core Audio), not the renderer, which only ever
sees one update with no voices; that is why the renderer's level line read 0. AudioOut now has its own level line (and `SCARLET_AUDIO_VOICES=1` prints the renderer's voices every 5 s):
silent for the first 10 s, then rms 0.013–0.022, peak 0.13–0.16, nearly every sample nonzero once the title music
starts. Sound is produced; listening is still to do.

Checked: 340 s with GPU submission on, A presses only, no crash; frames 1..9961 at about 30 fps.

Unresolved question kept here: the vertex shaders add c0+0x640 (base vertex) to Metal's `vertex_id`, which already
includes the base / first vertex, so a draw with a nonzero first vertex would be offset twice if Maxwell's vertex ID
excludes it. Every draw of the 37 shaders that read c0+0x640 in two pass dumps has base 0 and first 0, so it is not
tested either way yet.

Seen, not fixed yet: the room is overexposed too; outdoor scenes in the opening are washed out (sky white, characters pale) and the sky has large
dark / purple rectangles; some characters are drawn grey and white in some shots and fully coloured in others; the
enrollment photo is black.

### 2026-10-26: the sky, the exposure and the colours

Three problems turned out to be one. Outdoors the opening was nearly white, indoors overbright, characters grey and
white in some shots, and the sky split into four flat quadrants (dark green, purple, blue, navy) with the split exactly
at the screen centre. The pass dump and render-target dump of one cloud frame showed the quadrants already in the
480x270 sky buffer the sky pass draws. That pass's vertex shader writes only `.xy` of its varying; its fragment shader
divides by `.w` of the same varying. On the GPU an unwritten varying component reads as (0, 0, 0, 1); the translated MSL
starts from `main0_out out = {}`, so `.w` was 0, and x/0, y/0 became infinities whose signs depended on the quadrant. The
infinite sky also fed the exposure and the lighting, hence the white outdoors and the pale characters (the G-buffer
albedo had the right colours all along: black hair, red and white gear).

`vs_out_defaults` now starts every float4 user output of a vertex shader with w = 1, both for the shader as loaded and
for the variants built from its source (`sh_src`); a write by the shader still replaces it. Checked in the same frames:
blue sky with clouds over Naranja Academy, the battle court in its real colours, Nemona with black hair, the player's
room in natural light (school uniform, red tie). The enrollment photo has not been rechecked.

Also:
- A same-size copy between the sRGB and linear forms of RGBA8 / BGRA8 (1919x1080, seen in the room) copies the bits
  through a view in the destination's format instead of being dropped.
- `lay_self` (the vertex shader picks its layer) now keeps a pending clear: the view covers every layer and both NVN and
  Metal clear all layers of a layered target. With that a second such pass appears, a 64x64 cube map drawn one face per
  instance (six layers), which before went to face 0 only. Each new layered target is logged once.
- A save backup is kept in `SCARLET_build/user-data/save-backup/` while a test replays the opening without a save.

### 2026-10-26: Thor port, T0 (device probe) and the Android rewrite

Plan: `~/Games/ThorNative/THOR_SCOPE.md` (outside the repository). The Thor is connected over USB and driven with adb.

**T0, measured on the device** (`android/probe/thor_probe.c`, report in `android/probe/thor-probe-2026-10-26.txt`):
AYN Thor 12 GB (11.0 GiB usable), Snapdragon 8 Gen 2 (`kalama`), Android 13 / API 33, kernel 5.15, 4 KiB pages, 39-bit
user address space, 164 GB free. The GPU driver is Qualcomm's Adreno 740 driver 676.53 (Vulkan 1.3.128); the second
Vulkan library on the system, `vulkan.pastel.so`, is SwiftShader (software). Host checks: `tpidrro_el0` reads 0 (as
expected on Linux), Bionic's `TLS_SLOT_APP` is free, anonymous memory made executable runs, write faults can be caught
and retried, `pread` into a write-protected page fails with EFAULT (the case the RomFS retry handles), a 64 GiB
reservation below 2^39 fits, memfd works. A `dlopen`ed library may not use initial-exec TLS (tested: the linker refuses
it), which matters for the app stage. Vulkan: BC1–BC7 sampled natively, transform feedback (4 buffers), custom border
colours, depth clip control, stencil export, vertex-stage stores, buffer device address, dynamic rendering and
geometry shaders are all there; host memory import, shader layer output and 64-bit shader types are not.

**The rewriter's Android target.** Rewritten code finds its thread slots as "thread pointer + an offset from the key
page"; on Apple the thread pointer is `tpidrro_el0`. `DM_TARGET=android` makes `patch_nso.py` read `tpidr_el0`
(Bionic's thread pointer) instead, one instruction per block with every block's length unchanged; the host will keep
the slots in its executable's static TLS, at a fixed offset from `tpidr_el0` in every thread. The rewrite records
`# THREAD_REG 1` in `fixups.tsv`, and `structck.py` / `equiv.py` read it (the patch area must then hold `tpidr_el0`
reads followed by the key-page sequence and no `tpidrro_el0`; the equivalence emulator also requires the real
`tpidr_el0` to be unchanged after each block). Scarlet has no parameter gates, the only two-word in-place pattern.
Checked: the Apple rewrite of all three modules is byte-identical to the pinned one; the Android rewrite
(`SCARLET_build/rewritten/v400-android`) passes the structural check and the equivalence check for sdk (13,993 sites),
subsdk0 (10,541) and main (25,330), with 0 divergent sites.

### 2026-10-26: Thor port, T1: the game runs on the device (no picture yet)

**The link.** The Android build (`android/Makefile`, `make -f android/Makefile`, then `push` / `boot`) compiles every
runtime and graphics source except the Objective-C ones, with three stand-ins: `nvn_null.c` (the TotK tree's null
graphics backend: every `mtl_*` entry with the CPU-visible effects of a submission done at once — event writes, counter
reports, buffer copies, uniform updates — and fences signalled when committed; Scarlet's diagnostics hooks added),
`runtime/frontend_android.c` (no window: boot progress on stderr, a placeholder VI window, a 60 Hz vsync thread, text
input answered from `SCARLET_TEXT`, default "Thor") and `runtime/gamepad_android.c` (no controller yet; the input
script of `runtime/input.c` works). The Mac Makefile leaves those three out. Assembly takes its own rule (no C prelude).

**First run: an SDK abort in `NvOsDrvOpen`.** The game reached `nnMain`, made its threads, and then opened the real
NVN driver: `nvnBootstrapLoader` was not replaced. `HLE_FORCE` (`host/hle.h`) still put its table entries in the Mach-O
section name `__DATA,totkgfx`, which on ELF is a different section from the `totkgfx` that `gfx_bridge.c` reads. Fixed
the same way as the other registration sections.

**Second run: the main loop.** 4,200 frames at 33.3 ms (the game's own 30 fps, present interval 2), the title screen
waiting for input. Module bases are now printed at load (`mapped main at …`).

**Third run, with the Mac save copied over (`user-data/account.uid` and the save folder): Continue aborted** with a
guest `Break`. `runtime/services.c` now logs `nn::diag::detail::AbortImpl` (all three forms main imports: condition,
function, file, line, message), which named it: a Lua error, `bad argument #1 to 'band' (number has no integer
representation)`. Ruled out one at a time, each by a run on the Mac or the Thor: the GPU output (the Mac without GPU
submission continues), audio (the Mac with `SCARLET_AUDIO=0 TOTK_AUDIO_OFF=1` continues), the silent Opus stand-in
(the Mac built with `-DSCARLET_OPUS_SILENT`, now a switch in `opus_codec.c`, continues), the guest address layout (the
Thor with the low region moved still aborts), timing (the Thor pinned to its big cores still aborts), FPCR (both start
threads at 0; the game sets flush-to-zero itself), the rewrite (the Apple and Android rewrites differ only in the 50,172
thread-register reads, and neither leaves a `tpidr_el0` access). Decisive: the Thor's exact file set (null backend,
headless front end) built natively for the Mac continues, so the difference was the platform.

Under `lldb-server` on the Thor (the NDK's), a breakpoint on Lua's "no integer representation" path
(`luaL_checkinteger`, main+0x24390) and a small Lua 5.4 stack walker (bytecode and constants decoded) showed the game's
own `int()` helper failing on `timeT / 180` with `timeT = 549,755,802,832 = 0x7f_ffff_d4d0`: a host stack address (ASLR
off under the debugger, the main thread's native stack right under 2^39). The game reads an uninitialised local there.
The cause was ours: the game's main thread ran guest code on its native stack, below what the host had left there
during startup, and with no host-stack switch, so host functions it called wrote their frames among the game's. On the
Mac the leftovers are `0x16f…` addresses, small enough that `int()` takes its `math.floor` branch and nothing shows.

Fix (both platforms): the main thread now gets a fresh, zero-filled 8 MiB guest stack just below the stack region
(`runtime_main_stack_top`, reported as a stack by `svcQueryMemory`); `boot()` switches to it before `nn::init::Start`
and gives the thread a host stack on its native one, as every created thread has; the `nnMain` observer is no longer
moved to the host stack and zeroes the guest stack below itself before entering the game, so it starts from the clean
stack Horizon gives it.

Checked: on the Thor, Continue loads the save and the game runs on (frames 1,200–4,200: 59–63 NVN functions and about
3,700 calls a frame, the Mac's numbers for the same point; 1.35 GB footprint without textures in GPU memory). On the
Mac, the regression run (two A presses, screenshot at 60 s, GPU on) reaches the house as before.

Not checked: anything visible on the Thor (no graphics backend yet), the new-game path with an empty save on the Thor
(it sat idle after the first A on the title before this fix; not retried), sound (no AAudio output, Opus silent until
libopus is built for Android), controllers.

### 2026-10-26: Thor port, T2: the first pictures through Vulkan

**The backend.** The TotK tree's Vulkan NVN backend (`nvn_vk*.c`, written for Windows behind the same `mtl_*` seam and
never run with a game) is now in Scarlet's tree, built by `make -f android/Makefile GFX=vk` (the default stays the null
backend). Its two external libraries are replaced by local stand-ins in `android/compat/vk`: `volk.h` (Android's
libvulkan exports every core function up to 1.3; the two extension functions it calls are loaded by hand) and
`vk_mem_alloc.h` / `vk_shims.c` (the part of VulkanMemoryAllocator it uses, over one dedicated `vkAllocateMemory` per
resource). The Android front end reports no window, so it renders offscreen; `TOTK_VK_SHOT=<dir>` writes every n-th
present as a PNG, and `TOTK_VK_DUMP_RT=<present>,<dir>` every render target of one frame. On the Thor: Adreno 740,
BC and ASTC sampled natively, no FP64, no host-memory import, so pools are mirrored with dirty uploads.

**Shaders.** The backend asks a `totk_xlate --service --target spirv` for each variant it lacks. Scarlet's translator has
no SPIR-V target, and no .NET is installed to build the TotK tree's newer one, but its MSL chain produces the same
SPIR-V first (the same translation job) with everything in descriptor set 0. `android/tools/shader_bridge.py` runs it on
the Mac, moves a pixel shader's resources to set 1 (the shared support buffer stays set 0 binding 0) and writes the
SPIR-V chain's reflection from the SPIR-V itself (a Python port of `SpirvReflect.cs` and `HandleSpirv`). The device
reaches it through `adb reverse` (`TOTK_XLATE_TCP=<port>`, a TCP client added to `nvn_vk_rt.c`): the request carries
the guest code inline, the reply carries the `.spv` and `.json`, and the variants land in `user-data/shader-cache-vk`
on the device. 146 variants translated in the first sessions, none failed.

**First pictures.** The title screen renders exactly (offscreen, 1920x1080). After Continue the house rendered with its
geometry, textures and layout right but every surface covered in fine stripes along its own texture direction. The
render-target dump of one frame located it: the G-buffer (pass 8) is clean, the stripes start in the full-screen
lighting pass (pass 16), whose shader reads slot 28 and takes bit 6 of the value's raw bits as a material flag. That
slot is a stencil view (`set_stencil`, which Scarlet's NVN layer has and the TotK backend did not know), so the backend
bound the depth plane, and bit 6 of a depth float is noise that follows the depth gradient. Views with `set_stencil`
now select the stencil aspect; on the Adreno a float sampler returns the stencil bits the shader expects, and the house
renders like the Mac's, sunlight on the stairs included.

Not done: the player character (only its shadow is drawn; it is skinned through transform feedback, `OP_TF_*`, which the
backend does not implement yet), a window (needs the APK stage's surface), storage buffers whose 16-byte-aligned start is
not 64-byte aligned (the Adreno's minimum; the MSL-chain shaders assume 16, so those bindings read 0..48 bytes early;
none in the lighting pass), `TOTK_VK_NO_BC=1` (crashes in the driver), performance (not measured).

### 2026-10-26: Thor port, T2: the player character, and three fixes for the Adreno

All on the Thor, offscreen, checked with screenshots in the house after Continue.

- **Per-stage program binds.** Scarlet binds vertex-only programs and unbinds the fragment stage (`BindProgram(NULL,
  FRAGMENT)`) for its transform feedback captures; the TotK Vulkan backend took every bind as the whole program, so those
  draws ran with no program and were dropped. The replay now keeps a program per graphics stage and combines them as the
  Metal backend does (`prog_bind`, `prog_combined`).
- **Transform feedback**, as on the Mac: a capture draw uses a variant of the vertex shader that stores the captured
  components itself (`shader_bridge.py tf_patch`: storage buffers 100+b and an info block at 98 in set 0, read
  `{first, count, base_instance, strides, offsets}`), drawn with rasterisation discarded; offsets advance per draw
  (`OP_TF_BIND` / `OP_TF_STATE`). The layout comes from the control section (`vk_tf_layout`, the Metal backend's
  `tf_layout`), and a capture variant has its own shader key. Guest capture buffers are only 4-byte aligned, so each is
  bound at its 64-byte-aligned start and the shader adds the rest.
- **Unwritten varyings** read (0, 0, 0, 1) (`vs_out_defaults`, the Mac's fix ported to SPIR-V): the character was washed out.
- **Storage buffers on the Adreno.** The translator's storage buffer accesses index from the buffer's exact address
  (Metal's 16-byte rule); a Vulkan descriptor must start on `minStorageBufferOffsetAlignment` (64), so whenever a buffer
  was not 64-byte aligned every access landed up to 48 bytes early. The character's head exploded in some frames and not
  in others (the skinning data moves every frame). Every storage buffer access chain now adds its binding's offset from
  the aligned start, read from one more storage buffer (`sbuf_offsets`, filled per draw by `VK_RB_SBOFF`).
- **Driver crashes** (a NULL dereference in `vulkan.adreno.so`, in `vkEndCommandBuffer`, intermittently during the load
  after Continue): when the game released a texture from another thread while the render thread was recording, the
  backend destroyed the image at once if the GPU was idle, under an open command buffer that used it. Destruction now
  waits for the submission being recorded. Submissions also use one command pool per recording thread. The crash
  handler names the library and offset of each frame (`dladdr`), which located it.

Checked: four consecutive screenshots (presents 3000 to 3450) show the character animating correctly in the house;
the run held 30 fps (frame time median 33.3 ms, NVN replay about 8 ms a frame); no crash in the last two runs (one with
170 variants translated during it).

### 2026-10-26: Thor port: thread slots through TLS_SLOT_APP; sound through AAudio

**Thread slots for an app.** The Android rewrite found its thread slots at a fixed offset from `tpidr_el0`, the
executable's static TLS. A library an app loads cannot have that (the T0 probe: Bionic refuses initial-exec TLS in a
`dlopen`ed library), so the APK stage could not have used it. As THOR_SCOPE.md decision 1 planned, the slots are now in a
64-byte block per guest thread whose address Bionic's `TLS_SLOT_APP` holds (slot 2, offset 16 on arm64 in Android 13's
`tls_defines.h`, "available for use by apps in Android Q and later"): every thread-pointer read the rewriter emits is
followed by `ldr xN, [xN, #16]` (`patch_nso.py`, `# THREAD_REG 2`), and the gates' `LOAD_TP` does the same. `structck.py`
and `equiv.py` know the template (the emulator's thread pointer points at a block whose slot 2 holds the slot page; the
block and the slot must be unchanged after every patch block). Checked: the Apple rewrites of all three modules are
byte-identical to the pinned ones; the Android rewrite passes the structural check and the equivalence check for sdk
(13,993 sites), subsdk0 (10,541) and main (25,330) with 0 divergent sites; `expected-v4.0.0.json` pins the new Android
hashes. On the Thor the game boots, continues the save and renders the house and the character as before; the Mac
regression run reaches the house as before. The previous rewrite stays as `rewritten/v400-android-static`.

**Sound.** Scarlet's Wwise mixes on its own and plays through AudioOut (`audout:u`), which on Android was a silent sink.
`audio_out.c` now opens an AAudio stream per AudioOut: appended buffers go into a ring the stream's callback plays from,
and a buffer is given back (and the buffer event signalled) once its last frame has played, so the game is paced by
the real device as on the Mac. The audio renderer's mixer (`audio_mixer.c`) has an AAudio output too (Core Audio and
AAudio now share its ring pull); Scarlet sends it silence. `SCARLET_AUDIO_MUTE=1` keeps the streams running but silent
(tests at night). Checked on the Thor, muted: AudioOut level in the house rms 0.011 to 0.018 (the Mac's was 0.024),
481,280 samples per 5 s (48 kHz stereo) with almost all of them nonzero, no stalls, 30 fps held. Music the game decodes
with Opus stays silent until libopus is built for Android (`opus_silent.h`).

### 2026-10-26: Thor port, T3: an APK, on the Thor's screen, with its controls

**The app.** `make -f android/Makefile APK=1 GFX=vk install` builds the runtime as `libscarlet.so` and packs it into
`scarlet.apk` (4 MB) with no Android SDK, Gradle or Java: Android's own `NativeActivity` loads the library
(`android:hasCode="false"`), and `android/tools/build_apk.py` compiles `android/app/AndroidManifest.xml` to Android's
binary XML (attribute IDs checked against the Thor's `framework-res.apk`) and signs the APK with an APK Signature
Scheme v2 block made with openssl (a debug key in `SCARLET_build/apk-key`). The APK holds no game code or data: the app
reads `extracted/`, `rewritten/v400-android/` and `user-data/` from its files folder
(`/sdcard/Android/data/org.macnativegames.scarlet/files`), where `env.txt` sets test switches and `logs/run.log` is
the game's log (`runtime/activity_android.c`). The game data were copied there on the device as the app's own user
(`run-as`), since files that `adb shell` creates there belong to the shell user and the app cannot read them.

**The window.** The Vulkan backend presents to the activity's window (`front_vk_surface`: `vkCreateAndroidSurfaceKHR`).
The Thor's top panel is portrait (1080x1920); the swapchain is made in the game's landscape with the identity transform
and the compositor rotates it, and Android's `VK_SUBOPTIMAL_KHR` for that mismatch no longer remakes the swapchain at
every present. When the window goes (Home, recent apps), the backend destroys its swapchain and surface before the
window is released (`nvn_vk_window_lost`), renders offscreen meanwhile, and makes them again for the next window.
Focus loss is passed to the game as on the Mac (nn::oe message 15) and silences the sound until focus returns.

**The controls.** The Thor's pad ("Odin Controller", `Vendor_2020_Product_0111.kl`) sends Android gamepad keys and
joystick motion: sticks on X / Y and Z / RZ, triggers on BRAKE / GAS, the d-pad as HAT_X / HAT_Y and d-pad keys.
`runtime/gamepad_android.c` turns them into the Mac's physical buttons (face buttons by position: the right one is the
Switch's A; `TOTK_FACE_BUTTONS=label` maps them by label) and applies the player's layout and its modes
(`TOTK_CONTROLS`). The back key is taken so it does not close the game; home, volume and recent apps stay with Android.

**Fixes the app needed.** In an app `/proc/self/exe` is Android's `app_process64`, so the section lookup behind the
graphics layer's HLE table (`android/compat/elf_sections.c`) found nothing and the game stopped at `nvdrv`; it now reads
the file `dladdr` names (the library, or the program). The Mac's one-game-at-a-time slot is skipped in the app, as on
iOS. `exit()` in an app runs the framework's destructors under its running threads (FORTIFY aborts), so the app saves
the Vulkan pipeline cache and calls `_exit`.

**Checked on the Thor:** the title screen fills the screen (screencap 1920x1080, the right way up); a gamepad button
injected with `input gamepad keyevent BUTTON_B` (the right face button) passes the title and Continue into the house;
the left stick, injected into the controller's own input device (`sendevent` on `/dev/input/event9`), walks the
character down the stairs. Frame time p50 33.33 ms, p95 35.0 ms; NVN replay 11.2 ms; 2.4 GB PSS. Home and back: the
window and swapchain are remade, the game kept running, focus out and in was reported and the sound was silent in
between. Not checked: the face buttons pressed by hand (whether the Thor sends BUTTON_A for its bottom button), and
sound through the speaker (the tests ran muted at night). The Mac runtime still builds.

**Shaders without the Mac.** `android/tools/prefill_shaders.py` translates for the Thor, ahead of play, every shader
variant in the Mac port's own shader cache that the device's cache lacks: the Mac keeps each variant's guest code, its
constant section and the bind state it depends on, and shader keys are the same on both ports. It runs them through the
bridge's translation with that bind state as the answers; `android/tools/push_shaders.sh` unpacks them into the app's
cache as the app's user and appends the index. 195 shaders (198 variants) were added, each with exactly the Mac's
dependencies; the device cache went from 202 variants of 172 shaders to 400 of 369. With the bridge turned off
(`TOTK_XLATE_TCP` unset), title, Continue, the house and a walk round the ground floor drew with no shader missing
(only pipelines being built at first use: 112 draws over two frames). Not made ahead: transform feedback variants
(their layout is known only when the game draws) and anything neither port has met yet.

### 2026-10-26: Thor port: Opus through Android's own decoder

Scarlet decodes its music and voices with the hardware Opus decoder (65 decoders made at start, at 8 to 48 kHz), which
on Android was a silent stand-in waiting for libopus. Android has an Opus decoder of its own, reachable through the NDK's
`AMediaCodec` (`c2.android.opus.decoder`, the platform's libopus in the media codec process), so
`android/compat/opus_mediacodec.c` puts it behind the libopus calls `runtime/opus_codec.c` makes: one codec per game
decoder, an `OpusHead` with no pre-skip so every sample comes back, each packet queued and its output waited for, reset
by a flush. The platform decoder always outputs 48 kHz; for a lower rate every (48000 / rate)-th sample is kept, as
libopus's own CELT decoder lowers its rate (averaging them first cost 30 dB of agreement). The final-range integrity
check is skipped (the codec does not report it). A probe on the Thor (the platform's Opus encoder, a 440 Hz tone) decoded
the tone at its own level through 70 decoders at once and after a flush.

**Checked against libopus.** With `SCARLET_OPUS_DUMP=logs/opus.dump` the app records its first 4000 decoded packets;
`android/tools/opus_compare.py` decodes the same packets with the Mac's libopus from each decoder's creation and through
its resets. Title to house: 4000 packets from 47 decoders, signal-to-difference 58.6 dB overall, 55 to 61 dB on each of
the long streams (the usual gap between two libopus builds), about 31 dB on the 4 to 6 packets that start 24 kHz streams
(different resampling paths), the same sample count for every packet. In the game: 1.6 ms per packet on average (worst
22 ms over 5000), the game's mix in the house at rms 0.018 instead of 0.011 without Opus, frame time p50 33.2 ms.
Not checked: listening (the tests ran muted at night).

### 2026-10-26: Thor port: two memory leaks, text entry, pausing in the background

**A 30-minute soak in the house grew the app's native heap by 6.5 MB a minute.** Android's malloc debug (the app's
`wrap.` property with `LIBC_DEBUG_MALLOC_OPTIONS=backtrace_enable_on_signal`, backtraces switched on by a signal once
in the house, two `am dumpheap -n` dumps compared by backtrace) found two causes:

- **Command recording buffers** (`hle_nvn.c`, shared with the Mac build). Each NVN command buffer kept a host buffer
  for its records (64 KiB or more) until `nvnCommandBufferFinalize`. Scarlet initializes about one command buffer a
  frame and finalizes almost none (1,714 Initialize against 42 Finalize over 1,500 frames on the Thor; 1,678 against 18
  on the Mac), which real NVN allows since it allocates nothing per command buffer. The recording is copied out at
  EndRecording, so the buffer now goes back to a pool of up to 64 there and is lent again at the next recording. The
  Mac build reaches the house with it (RSS 244 to 257 MB over four minutes).
- **Platform Opus codecs** (`android/compat/opus_mediacodec.c`). Scarlet initializes decoders again for new sounds and
  leaves many unfinalized (512 game decoders after 25 minutes). With libopus that costs nothing; one platform codec
  per decoder filled the codec process, grew this process's heap and slowed decoding to 7, then 15 ms a packet. A
  decoder now takes a codec at its first packet from a pool of flushed codecs (per channel count) and gives it back
  when destroyed or after a second without packets. Over 25 minutes: 4 codecs made in all, 1.6 ms a packet, worst
  7.5 ms.

After both: native heap 1,129 then 1,128 MB over ten minutes, total 2.46 GB and flat, frame time p50 33.0 ms.

**Text entry.** The keyboard applet (`swkbd_host.c`) cancelled every prompt on Android: it called the front end only on
macOS. It now asks `runtime_front_text_input` there too, which in the app shows an Android dialog (the game's prompt, a
text field with its initial text and length limit, OK) made over JNI on the UI thread; the app has no Java code, so a
timer on the UI thread's looper notices the dialog closing and reads the field. In landscape the keyboard takes the
screen with its own field; Done puts it away and shows OK (pinned below the dialog instead, it hid the dialog). Checked
with the keyboard self-test (`kbtest` in the input file): text typed with `input text`, Done, a tap on OK; the game's
side got `result 0, text "Arven"`. The command-line build answers from `SCARLET_TEXT` as before.

**Away from the app the game pauses.** Without a window the vsync ticks stop and the backend's presents wait
(`nvn_vk_present_hook`); the vsync ticks alone did not stop Scarlet. Home, 30 s away: no frames, the process at 11 %
CPU instead of 96 to 169 %; back: the window and swapchain are remade and the frames go on.

**Test switches in the app.** The input file, input script, auto-load and thread dumps started from constructors that
read the environment when the library loads, before the app reads `env.txt`, so in the app they never started.
`RUNTIME_ENV_START` (`runtime.h`) keeps them constructors in the programs and makes them functions the app calls after
`env.txt`.

**Setup.** `android/tools/install_data.sh` copies the game data, fonts, the Android rewrite and (once) the Mac's save
into the app's folder as the app's user; `android/README.md` describes building, installing, the data folder, the
controls and the two ways to get shaders.
