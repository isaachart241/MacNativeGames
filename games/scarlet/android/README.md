# Scarlet on the AYN Thor (Android)

The Mac port's runtime, built for Android: the rewritten game code runs natively on the Thor's Snapdragon 8 Gen 2, the
graphics layer replays NVN through Vulkan on the Adreno 740, and the game is an app with no Java code (Android's
`NativeActivity` loads `libscarlet.so`). The plan and the stages are in `THOR_SCOPE.md` (outside the repository); what
was done and checked is in `../CHANGELOG.md` ("Thor port").

## Build and install (from `games/scarlet`, on the Mac)

```sh
python3 scripts/prepare_runtime.py --work SCARLET_build --target android   # the Android rewrite and its tables
make -f android/Makefile APK=1 GFX=vk install                             # SCARLET_build/build-android-apk/scarlet.apk
android/tools/install_data.sh                                             # game data and the Mac's save into the app
```

Needs the Android NDK (`/opt/homebrew/share/android-ndk`), `adb`, `openssl` and Python 3; not the Android SDK, Gradle or
Java (`android/tools/build_apk.py` writes the binary manifest and the v2 signature). The debug signing key is made in
`SCARLET_build/apk-key` the first time: keep it, since Android refuses an update signed with another key.

The APK holds no game code or data. They live in the app's files folder,
`/sdcard/Android/data/org.macnativegames.scarlet/files`:

| | |
|---|---|
| `extracted/`, `rewritten/v400-android/` | the RomFS image, fonts and rewritten modules (`install_data.sh`) |
| `user-data/save/` | the save (the Mac's, copied once; `install_data.sh --save` copies it again, keeping the app's) |
| `user-data/shader-cache-vk/` | translated shaders and the Vulkan pipeline cache |
| `env.txt` | `VAR=value` lines set before the game starts, for example `SCARLET_AUDIO_MUTE=1` |
| `logs/run.log` | the game's log (`logs/run-prev.log`: the run before) |

Files there must belong to the app: copy them in with `adb shell run-as org.macnativegames.scarlet ...` (as the
scripts do), not with plain `adb push`.

## Playing

The Thor's pad is the Switch Pro Controller, buttons by position: the right face button is A, the bottom one B. Set
`TOTK_FACE_BUTTONS=label` in `env.txt` to use the printed letters instead; `TOTK_CONTROLS=<file>` takes a layout file
as on the Mac (`runtime/controller_map.h`). The pad's back button does nothing in the game; Home leaves the app: the
game pauses and goes on when you come back. Text the game asks for (names, nicknames) is
typed in a dialog with the on-screen keyboard.

## Shaders

The game's shaders are translated to SPIR-V on the Mac; the device keeps what it has met in its cache. Two ways to fill
it:

- **Ahead of play:** `android/tools/prefill_shaders.py` translates every shader the Mac port has met (its own cache)
  that the device lacks, and `android/tools/push_shaders.sh` installs them. Title, Continue and the house need nothing
  else.
- **During play:** with the Thor on USB, run `adb reverse tcp:7841 tcp:7841` and
  `python3 android/tools/shader_bridge.py --port 7841`, and add `TOTK_XLATE_TCP=7841` to `env.txt`. A shader the
  device lacks is translated on the Mac while the game runs (its draws are skipped for the second or so that takes).

Without either, a shader neither port has met is never drawn on the Thor.

## Command-line build (tests)

`make -f android/Makefile GFX=vk` builds `scarlet-native`, the same runtime as a program for `adb shell`, with no
window (`TOTK_VK_SHOT=<dir>` writes PNG screenshots of what it renders). It reads its data from
`/data/local/tmp/scarlet`.
