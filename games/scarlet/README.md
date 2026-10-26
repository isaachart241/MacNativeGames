# Pokémon Scarlet / Violet (work in progress)

An experiment: run Pokémon Scarlet or Violet natively the way `games/by3` runs Bayonetta 3. This folder started as an
unchanged copy of `games/by3` (commit `caec8d3`), so the next commit's diff is the whole fork. Like by3, it keeps its
own copies of everything (`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/`), so changes here
never affect another game. Bayonetta 3 was chosen as the base because it already runs a third-party engine: CMIF
domains, offline network services, Wwise's AudioOut and indirect draws are all there.

**Nothing runs yet.** Stage 0 is done (2026-10-25): the code extracts, rewrites and passes the structural check. How the work goes: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own dump and the `prod.keys` from your own Switch. Nothing from the game is in this
repository; `SCARLET_build/` (the work folder) and `mods/` are ignored by git.

## Inputs

- Base `0100A3D008C5C000` **1.0.0**, a full game card image (XCI). Unlike Bayonetta's, the base is not sparse.
- Update `0100A3D008C5C800` **4.0.0** (NSP): a BKTR patch over the base, with the full ExeFS.
- `prod.keys` and firmware NCAs from your own Switch (the system fonts come from the firmware).

## What is still Bayonetta's

- `scripts/merge_romfs.py` was written for Bayonetta's sparse base. Scarlet is a normal base plus a BKTR update, the
  same shape as Tears of the Kingdom with an update, so stage 1 follows `build_app.py`'s merge instead.
- `assets/expected-v4.0.0.json` doesn't exist yet; `prepare_runtime.py` and `prepare_patches.py` need it (stage 1).
- `runtime/game_patches.c` reads Bayonetta-style patch tables (`SCARLET_PATCHES` picks them); Scarlet has none yet.
- `scripts/make_res_patch.py` holds Bayonetta's resolution sites; it does not apply to Scarlet.

## Stage 0 (done): commands

From the repository root, after `source ../env.sh`, with the files in `~/Games/Scarlet/`:

```sh
cd games/scarlet
python scripts/extract_shared_fonts.py --keys ~/Games/Scarlet/prod.keys --firmware ~/Games/Scarlet/Firmware.22.5.0 \
  --output SCARLET_build/extracted/firmware/shared-fonts
cd scripts
python extract_code.py ~/Games/Scarlet/"Pokemon Scarlet v4.0.0.nsp" --keys ~/Games/Scarlet/prod.keys \
  --output ../SCARLET_build/extracted/v400/exefs
python survey.py ../SCARLET_build/extracted/v400/exefs --json ../SCARLET_build/survey.json
cd ..
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 python third_party/d2r/patch_nso.py SCARLET_build/extracted/v400/exefs/$m SCARLET_build/rewritten/v400 $m
  python third_party/d2r/structck.py SCARLET_build/rewritten/v400 $m SCARLET_build/extracted/v400/exefs/$m
done
```

Use the build's variables: without `DM_RULESET=native DM_PRIVATE_GATES=0` the rewrite has to be redone.
What each step found is in [CHANGELOG.md](CHANGELOG.md).

## Stage 1: next

1. Run `equiv.py` on the three rewrites (each rewritten site against the original in an emulator), as by3 did.
2. Merge the RomFS: base 1.0.0 + the 4.0.0 BKTR patch, following `build_app.py`'s update path; verify every IVFC block.
3. Write `assets/expected-v4.0.0.json` pinning the ExeFS, rewrite and RomFS hashes.
