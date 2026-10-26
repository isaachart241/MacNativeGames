# Thor port (Android, AYN Thor: Snapdragon 8 Gen 2, Android 13), stage T1: the runtime as a command-line program run
# over adb, with the null graphics backend (nothing is drawn) and no sound device. Run from games/scarlet:
#   python3 scripts/prepare_runtime.py --work SCARLET_build --target android   # tables for the android rewrite
#   make -f android/Makefile                # SCARLET_build/build-android/scarlet-native
#   make -f android/Makefile push           # to /data/local/tmp/scarlet (the game data are pushed once, see THOR_SCOPE.md)
#   make -f android/Makefile boot           # run it there; the log comes back to SCARLET_build/logs/thor-boot.log
# The rewrite for this target is DM_TARGET=android (third_party/d2r/patch_nso.py): guest slots behind tpidr_el0.

WORK ?= SCARLET_build
OUT := $(WORK)/build-android
NDK ?= /opt/homebrew/share/android-ndk
API ?= 33
CC := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android$(API)-clang
ADB ?= adb
DEVICE_DIR ?= /data/local/tmp/scarlet
NATIVE_OPT ?= -O2

# Everything the Mac build compiles except the Objective-C (Metal, AppKit, GameController) and the Metal-only pieces;
# frontend.m / gamepad*.m are replaced by their _android.c counterparts and nvn_mtl.m by nvn_null.c.
SRC := $(wildcard runtime/*.c runtime/*.S) \
  $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c) \
  android/compat/darwin_compat.c android/compat/elf_sections.c android/compat/sha256.c
OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(SRC))
HEADERS := $(wildcard runtime/*.h third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h android/compat/*.h android/compat/*/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h
CFLAGS := -std=gnu11 $(NATIVE_OPT) -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
  -include android/compat/android_prelude.h -Iandroid/compat -I$(OUT) -Iruntime \
  -Ithird_party/d2r_gfx/gfx/nvn/include -Ithird_party/d2r_gfx/gfx/nvn -Ithird_party/d2r_gfx/gfx/tex -Ithird_party/d2r_gfx/third_party

.PHONY: all push boot clean
all: $(OUT)/scarlet-native

$(GENERATED):
	@echo "missing $@: run python3 scripts/prepare_runtime.py --work $(WORK) --target android" && false

$(OUT)/obj/%.c.o: %.c $(HEADERS) $(GENERATED) android/Makefile
	@mkdir -p "$(dir $@)"
	"$(CC)" $(CFLAGS) -c "$<" -o "$@"

# Assembly: the same include paths, without the C prelude
$(OUT)/obj/%.S.o: %.S $(HEADERS) android/Makefile
	@mkdir -p "$(dir $@)"
	"$(CC)" -Iruntime -Ithird_party/d2r_gfx/gfx/nvn -I$(OUT) -c "$<" -o "$@"

$(OUT)/scarlet-native: $(OBJECTS)
	"$(CC)" $(OBJECTS) -lm -ldl -llog -o "$@"

push: $(OUT)/scarlet-native
	"$(ADB)" shell mkdir -p $(DEVICE_DIR)/logs
	"$(ADB)" push "$<" $(DEVICE_DIR)/scarlet-native
	"$(ADB)" shell chmod 755 $(DEVICE_DIR)/scarlet-native

# THOR_ENV: extra environment for the run (for example SCARLET_TEXT=Name TOTK_INPUT_FILE=input.txt).
THOR_ENV ?=
THOR_LOG ?= $(WORK)/logs/thor-boot.log
boot: push
	@mkdir -p "$(dir $(THOR_LOG))"
	"$(ADB)" shell "cd $(DEVICE_DIR) && TOTK_HEADLESS=1 $(THOR_ENV) ./scarlet-native rewritten/v400-android boot" > "$(THOR_LOG)" 2>&1; echo "exit $$?; log: $(THOR_LOG)"

clean:
	rm -rf "$(OUT)/obj" "$(OUT)/scarlet-native"
