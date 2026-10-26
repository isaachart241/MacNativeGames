#!/system/bin/sh
# Thor port: the device half of android/tools/install_data.sh, run by the app's own user (run-as) so the files it
# copies belong to the app.   install_data_app.sh <stage> <app files folder> <1: copy the Mac save over the app's>
set -e
STAGE=$1; APP=$2; SAVE=$3
cd "$APP"
mkdir -p extracted/v400/romfs extracted/firmware rewritten user-data
# the RomFS image (10 GB): copied again only when its size or index differs
if [ "$(stat -c %s "$STAGE/extracted/v400/romfs/romfs.bin")" = "$(stat -c %s extracted/v400/romfs/romfs.bin 2>/dev/null)" ] &&
   cmp -s "$STAGE/extracted/v400/romfs/index.json" extracted/v400/romfs/index.json; then
    echo "RomFS unchanged"
else
    for f in romfs.bin runtime-index.bin index.json; do cp "$STAGE/extracted/v400/romfs/$f" extracted/v400/romfs/; done
    echo "RomFS copied"
fi
rm -rf extracted/firmware/shared-fonts rewritten/v400-android
cp -r "$STAGE/extracted/firmware/shared-fonts" extracted/firmware/
cp -r "$STAGE/rewritten/v400-android" rewritten/
cp "$STAGE/user-data/account.uid" user-data/
if [ ! -d user-data/save ]; then
    cp -r "$STAGE/user-data/save" user-data/; echo "the Mac's save copied"
elif [ "$SAVE" = 1 ]; then
    mv user-data/save "user-data/save-before-$(date +%Y%m%d-%H%M%S)"; cp -r "$STAGE/user-data/save" user-data/
    echo "the Mac's save copied (the app's kept as user-data/save-before-...)"
else
    echo "the app's save kept (--save copies the Mac's over it)"
fi
[ -f env.txt ] || echo "# VAR=value lines, set before the game starts (runtime/activity_android.c)" > env.txt
du -sh extracted rewritten user-data
