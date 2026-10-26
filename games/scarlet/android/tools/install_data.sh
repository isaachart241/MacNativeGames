#!/bin/sh
# Thor port, stage T3: put the game data into the app's files folder, from the Mac's work folder (run from games/scarlet):
#   extracted/v400/romfs (the RomFS image and its index), extracted/firmware/shared-fonts, rewritten/v400-android (the
#   Android rewrite: python3 scripts/prepare_runtime.py --work SCARLET_build --target android), user-data/account.uid,
#   and the Mac's save (user-data/save) when the app has none yet; --save copies it over the app's, which is kept first
#   as user-data/save-before-<date>.
# Files go to /data/local/tmp/scarlet first (adb push --sync sends only what changed: about 10 GB the first time) and
# are copied into the app's folder by the app's own user (run-as: the app cannot read files the shell user makes
# there), by android/tools/install_data_app.sh. The app must be installed (make -f android/Makefile APK=1 GFX=vk
# install); it is stopped while this runs. Its first start after a new RomFS copy checks the image again (about 40 s).
#   android/tools/install_data.sh [--save]
set -e
WORK=${WORK:-SCARLET_build}
PKG=org.macnativegames.scarlet
APP=/storage/emulated/0/Android/data/$PKG/files
STAGE=/data/local/tmp/scarlet
SAVE=0; [ "$1" = "--save" ] && SAVE=1
adb shell pm path $PKG > /dev/null || { echo "install the app first: make -f android/Makefile APK=1 GFX=vk install" >&2; exit 1; }
adb shell "[ -d $APP ]" || { adb shell am start -n $PKG/android.app.NativeActivity > /dev/null; sleep 3; }   # Android makes the folder
adb shell am force-stop $PKG
adb shell mkdir -p $STAGE/extracted/v400/romfs $STAGE/extracted/firmware $STAGE/rewritten $STAGE/user-data
adb push --sync "$WORK/extracted/v400/romfs/romfs.bin" "$WORK/extracted/v400/romfs/runtime-index.bin" "$WORK/extracted/v400/romfs/index.json" $STAGE/extracted/v400/romfs/
adb push --sync "$WORK/extracted/firmware/shared-fonts" $STAGE/extracted/firmware/
adb push --sync "$WORK/rewritten/v400-android" $STAGE/rewritten/
adb push --sync "$WORK/user-data/account.uid" "$WORK/user-data/save" $STAGE/user-data/
adb push "$(dirname "$0")/install_data_app.sh" $STAGE/install_data_app.sh > /dev/null
adb shell chmod -R a+rX $STAGE/extracted $STAGE/rewritten/v400-android $STAGE/user-data/account.uid $STAGE/user-data/save $STAGE/install_data_app.sh
adb shell chmod a+rx $STAGE $STAGE/rewritten $STAGE/user-data   # the folders on the way (only these: not the shell's other files there)
adb shell run-as $PKG sh $STAGE/install_data_app.sh $STAGE $APP $SAVE
echo "done: start Scarlet on the Thor"
