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
# frontend.m / gamepad*.m are replaced by their _android.c counterparts and nvn_mtl.m by a backend chosen with GFX:
#   GFX=null (default): nvn_null.c, nothing drawn (stage T1)
#   GFX=vk: the Vulkan backend nvn_vk*.c (stage T2, from the TotK tree), over android/compat/vk (volk / VMA stand-ins)
GFX ?= null
VK_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/nvn_vk*.c) third_party/d2r_gfx/gfx/tex/nvn_fmt_vk.c third_party/d2r_gfx/gfx/tex/nvn_astc_dec.c
ALL_GFX := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c)
ifeq ($(GFX),vk)
GFX_SRC := $(filter-out third_party/d2r_gfx/gfx/nvn/nvn_null.c,$(ALL_GFX)) android/compat/vk/vk_shims.c
GFX_LIBS := -lvulkan
OUT := $(WORK)/build-android-vk
else
GFX_SRC := $(filter-out $(VK_SRC),$(ALL_GFX))
GFX_LIBS :=
endif
# APK=1 (stage T3): the same runtime as libscarlet.so for the app (runtime/activity_android.c), with the Vulkan backend
# presenting to the activity's window; no Java, no Gradle (android/tools/build_apk.py packs and signs it):
#   make -f android/Makefile APK=1 GFX=vk apk       # SCARLET_build/build-android-apk/scarlet.apk
#   make -f android/Makefile APK=1 GFX=vk install   # adb install -r (the game data: runtime/activity_android.c)
APK ?= 0
ifeq ($(APK),1)
ifneq ($(GFX),vk)
$(error APK=1 needs GFX=vk)
endif
OUT := $(WORK)/build-android-apk
PIC := -fPIC -DSCARLET_APK
else
PIC :=
endif
SRC := $(wildcard runtime/*.c runtime/*.S) $(GFX_SRC) \
  android/compat/darwin_compat.c android/compat/elf_sections.c android/compat/sha256.c
OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(SRC))
HEADERS := $(wildcard runtime/*.h third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h android/compat/*.h android/compat/*/*.h)
GEN := $(WORK)/build-android
GENERATED := $(GEN)/runtime_tables.h $(GEN)/romfs_config.h $(GEN)/shared_fonts_config.h
CFLAGS := -std=gnu11 $(NATIVE_OPT) $(PIC) -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
  -include android/compat/android_prelude.h -Iandroid/compat -Iandroid/compat/vk -I$(GEN) -Iruntime \
  -Ithird_party/d2r_gfx/gfx/nvn/include -Ithird_party/d2r_gfx/gfx/nvn -Ithird_party/d2r_gfx/gfx/tex -Ithird_party/d2r_gfx/third_party

.PHONY: all push boot clean apk install
all: $(OUT)/scarlet-native

$(GENERATED):
	@echo "missing $@: run python3 scripts/prepare_runtime.py --work $(WORK) --target android" && false

$(OUT)/obj/%.c.o: %.c $(HEADERS) $(GENERATED) android/Makefile
	@mkdir -p "$(dir $@)"
	"$(CC)" $(CFLAGS) -c "$<" -o "$@"

# Assembly: the same include paths, without the C prelude
$(OUT)/obj/%.S.o: %.S $(HEADERS) android/Makefile
	@mkdir -p "$(dir $@)"
	"$(CC)" $(PIC) -Iruntime -Ithird_party/d2r_gfx/gfx/nvn -I$(GEN) -c "$<" -o "$@"

$(OUT)/scarlet-native: $(OBJECTS)
	"$(CC)" $(OBJECTS) $(GFX_LIBS) -laaudio -lm -ldl -llog -o "$@"

# The app's library: -Bsymbolic binds the runtime's own symbols inside it (the assembly reaches them PC-relative).
$(OUT)/libscarlet.so: $(OBJECTS)
	"$(CC)" -shared -Wl,-soname,libscarlet.so -Wl,-Bsymbolic -Wl,--no-undefined $(OBJECTS) $(GFX_LIBS) -landroid -laaudio -lm -ldl -llog -o "$@"

APK_KEY ?= $(WORK)/apk-key
apk: $(OUT)/libscarlet.so android/app/AndroidManifest.xml android/tools/build_apk.py
	python3 android/tools/build_apk.py --manifest android/app/AndroidManifest.xml --lib "$<" --key "$(APK_KEY)" -o "$(OUT)/scarlet.apk"
install: apk
	"$(ADB)" install -r "$(OUT)/scarlet.apk"

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
	rm -rf "$(OUT)/obj" "$(OUT)/scarlet-native" "$(OUT)/libscarlet.so" "$(OUT)/scarlet.apk"
