# Pokémon Scarlet / Violet (work in progress)

An experiment: run Pokémon Scarlet or Violet natively the way `games/by3` runs Bayonetta 3. This folder started as an
unchanged copy of `games/by3` (commit `caec8d3`), so the next commit's diff is the whole fork. Like by3, it keeps its
own copies of everything (`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/`), so changes here
never affect another game. Bayonetta 3 was chosen as the base because it already runs a third-party engine: CMIF
domains, offline network services, Wwise's AudioOut and indirect draws are all there.

**Nothing runs yet.** How the work goes: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own dump and the `prod.keys` from your own Switch. Nothing from the game is in this
repository; `SCARLET_build/` (the work folder) and `mods/` are ignored by git.

## What is still Bayonetta's

These are placeholders until stage 1 pins Scarlet's version. Each is a compile-time or hash check, so nothing can
silently use Bayonetta values with Scarlet data:

- `runtime/game_version.h`: `TOTK_GAME_VERSION "0.0.0"`, `TOTK_GAME_DIR "v120"`.
- `v120` in `Makefile`, `scripts/prepare_runtime.py`, `scripts/prepare_patches.py`, `scripts/make_res_patch.py`,
  `scripts/make_mod_manifest.py`.
- `assets/expected-v1.2.0.json` was removed; `prepare_runtime.py` and `prepare_patches.py` need a Scarlet
  `assets/expected-v<version>.json` (stage 1) before they run.
- `runtime/game_patches.c` reads Bayonetta's patch tables (`SCARLET_PATCHES` picks them); Scarlet has none yet.
- `scripts/merge_romfs.py` was written for Bayonetta's sparse base NCA. If Scarlet's base is not sparse, the update
  RomFS may be enough, or the script needs a non-sparse path.

## Stage 0: the first session with the dump

From the repository root, after `source ../env.sh` (Homebrew, the Python 3.14 environment, `LLVM_BIN`):

1. Put the dump, update and keys somewhere outside the repository, e.g. `~/Games/Scarlet/`.
2. Extract the code from the update:

   ```sh
   cd games/scarlet/scripts
   python extract_code.py ~/Games/Scarlet/update.nsp --keys ~/Games/Scarlet/prod.keys --output ../SCARLET_build/extracted/exefs
   ```

   Add `--title 01008F6008C5E000` for Violet. If it refuses (a layout Bayonetta did not have), that is the first fix.
3. Survey it:

   ```sh
   python survey.py ../SCARLET_build/extracted/exefs --json ../SCARLET_build/survey.json
   ```

   It lists build IDs and the SDK version, middleware markers, NVN names this tree does not implement, and services in
   `main.npdm` the host never mentions. Copy the summary (counts and names, no game data) into CHANGELOG.md.
4. Try the rewrite, which is generic. Expect a new site kind or two, as Bayonetta needed `site_spset`:

   ```sh
   for m in sdk subsdk0 main; do
     DM_RULESET=native DM_PRIVATE_GATES=0 python ../third_party/d2r/patch_nso.py ../SCARLET_build/extracted/exefs/$m ../SCARLET_build/rewritten/v0 $m
   done
   ```

   Use the build's variables: without `DM_RULESET=native DM_PRIVATE_GATES=0` the rewrite has to be redone.
5. Look up main's build ID in community `.pchtxt` files (60 fps, resolution) for that version.

Then stage 1: rename `v120` to the real version folder, write `assets/expected-v<version>.json`, and merge the RomFS.
