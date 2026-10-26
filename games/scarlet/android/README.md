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

The game's rumble drives the Thor's vibration motor. It has one motor, so it gets the stronger of the game's two;
`TOTK_RUMBLE=0` turns rumble off, and `SCARLET_RUMBLE_TEST=<1-255>` buzzes once at start to check the motor.

The bottom screen is the game's second screen while it is in focus: its menus, battle commands, dialogue and map
screen are drawn there (the top screen shows the game without them; screens that are all interface, such as the
title, show on both), with a strip of your map below them. They are laid out for this screen: the empty space between
the game's boxes is taken out and what is left is scaled up to the screen's width, so dialogue and battle text read at
the size the game gives them on a TV, or larger; screens that are all menu fill the width. Otherwise it shows the game's own map, north up and following you (drag to look
around, pinch to zoom; it follows you again a few seconds after the last touch), and the minimap leaves the top screen;
leaving the game gives the bottom screen back to whatever was there. The map is the game's own art, gathered as you
play from the minimap (hidden from the top screen but still drawn by the game), and kept in the data folder's
`map-cache/`: places appear once you have been near them. There is no need to open the game's map screen; when you
do (Y), its place names, the objective and the whole-Paldea view (zoomed out) are added too. `SCARLET_BOTTOM=0` leaves the bottom screen alone and keeps the minimap;
`SCARLET_MINIMAP=1` keeps the minimap as well, `SCARLET_BOTTOM_UI=0` keeps the interface on the top screen.

## Save backups

The save lives in the app's data folder, which Android deletes with the app (an uninstall, or an update signed with
another key). So the app copies it to `/sdcard/Documents/Scarlet saves/<date time>/` once at start and 5 s after each
save, keeping the 10 newest. `SCARLET_SAVE_BACKUP=0` turns this off; `SCARLET_SAVE_BACKUP_DIR=<path>` moves it. To
restore one, close the game and copy the backup's files, as the app, into the folder under `user-data/save/` that holds
`main`:

```sh
adb shell "run-as org.macnativegames.scarlet cp '/sdcard/Documents/Scarlet saves/<date time>/'* /sdcard/Android/data/org.macnativegames.scarlet/files/user-data/save/<folder>/"
```

## Shaders

The game's shaders are translated to SPIR-V on the Thor itself, the first time each one is drawn, and kept in the app's
cache. The translator is the Mac port's own (the Ryujinx shader translator with this project's service), compiled ahead
of time for Android with NativeAOT and packed into the APK with spirv-cross as `libtotk_xlate.so` and
`libspirv_cross.so`; its Vulkan mode (`android/xlate/VkService.cs`) gives byte for byte what the Mac bridge gave. A draw
waits for its shader (about 20 ms each; up to 5 s, `TOTK_SHADER_WAIT_MS`), so the first look at a new place may stutter
briefly; after that it comes from the cache.

- **Rebuilding the translator** needs the .NET 10 SDK: `android/xlate/build.sh android` (or `mac` for checks), then copy
  `SCARLET_build/build-android-tools/xlate-android/totk_xlate` to `prebuilt/android-arm64/` and update its `.sha256`.
  `python3 android/xlate/check_parity.py` compares the service with the bridge over the Mac's shader cache.
- **Ahead of play:** `android/tools/prefill_shaders.py` translates every shader the Mac port has met that the device
  lacks, and `android/tools/push_shaders.sh` installs them (no stutter the first time there).
- **On the Mac instead:** with `TOTK_XLATE_TCP=7841` in `env.txt`, `adb reverse tcp:7841 tcp:7841` and
  `python3 android/tools/shader_bridge.py --port 7841`, the Mac translates.

## Command-line build (tests)

`make -f android/Makefile GFX=vk` builds `scarlet-native`, the same runtime as a program for `adb shell`, with no
window (`TOTK_VK_SHOT=<dir>` writes PNG screenshots of what it renders). It reads its data from
`/data/local/tmp/scarlet`.
