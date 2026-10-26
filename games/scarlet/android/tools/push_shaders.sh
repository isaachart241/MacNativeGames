#!/bin/sh
# Thor port: copy shader variants made ahead of play (android/tools/prefill_shaders.py) into the app's shader cache.
# The app is stopped first (it appends to the same index during play). The files go through /data/local/tmp and are
# unpacked by the app's own user (run-as), since files the shell user creates in the app's folder are not readable by
# the app; index lines for variants the cache already lists are not added again.
#   android/tools/push_shaders.sh SCARLET_build/thor-shader-prefill
set -e
SRC=${1:?usage: push_shaders.sh <prefill folder>}
PKG=org.macnativegames.scarlet
CACHE=/storage/emulated/0/Android/data/$PKG/files/user-data/shader-cache-vk
TAR=/data/local/tmp/scarlet-shaders.tar
LOCAL=$(mktemp -d)
trap 'rm -rf "$LOCAL"' EXIT
(cd "$SRC" && COPYFILE_DISABLE=1 tar -cf "$LOCAL/shaders.tar" --no-xattrs index-add.tsv $(ls | grep -E '_v\.(spv|json)$'))
adb shell am force-stop $PKG
adb push "$LOCAL/shaders.tar" $TAR > /dev/null
adb shell "chmod 644 $TAR && run-as $PKG sh -c 'mkdir -p $CACHE && cd $CACHE && tar -xf $TAR && touch index.tsv && cut -f2 index.tsv > .have && grep -vFf .have index-add.tsv >> index.tsv; rm -f .have index-add.tsv; wc -l < index.tsv' && rm $TAR"
