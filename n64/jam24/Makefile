# Builds N64brew Game Jam 2024 minigames as ONE static libdragon ELF (no DSOs), for N64Recomp.
#
# The jam's own Makefile builds every minigame into a DSO that the harness dlopen()s at run time.
# A static recompiler needs every function present in one ELF, so instead:
#   - each minigame is linked into one relocatable object (ld -r); only its entry points stay
#     global (everything else is made local, so several minigames can share one ELF), and those are
#     renamed <game>_minigame_init/loop/fixedloop/cleanup/def;
#   - its data and bss are gathered into one section each (minigame.ld), bracketed by
#     <game>_data_start/end and <game>_bss_start/end, so every round can start from fresh globals
#     (a DSO is loaded fresh each time; a statically linked minigame keeps its old state);
#   - the harness's core.c and minigame.c are linked unmodified; its dlopen/dlsym calls are
#     redirected (--wrap) to a table in jam_boot.c, which also replaces main.c and the tiny3d menus.
#
# Run from the jam checkout (tools/n64-jam24.sh does this):
#   make -C third_party/_src/N64brew-GameJam2024-<rev> -f <repo>/n64/jam24/Makefile \
#        TARGET=jam_landgrab MINIGAMES=landgrab
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
JAM24_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TARGET ?= jam_landgrab
MINIGAMES ?= landgrab
ROMTITLE ?= "N64BREW GAMEJAM 2024"

BUILD_DIR = build-$(TARGET)
ASSETS_DIR = assets
FILESYSTEM_DIR = filesystem
# Each target gets its own DFS: only its minigames' assets plus the shared harness files.
N64_MKDFS_ROOT = $(BUILD_DIR)/fs

include $(N64_INST)/include/n64.mk

# tiny3d (third_party/_src/tiny3d-*, built by tools/n64-jam24.sh) for minigames that draw 3D:
# USE_T3D=1 and T3D_DIR come from tools/n64-jam24.sh. Its library is linked only into those
# programs, so the 2D games' ELFs (and their recompiled code) don't change.
ifeq ($(USE_T3D),1)
include $(T3D_DIR)/t3d.mk
# Models as in the jam's Makefile, minus its mkasset compression (our runtime decodes LZ4 only).
$(FILESYSTEM_DIR)/%.t3dm: $(ASSETS_DIR)/%.glb
	@mkdir -p $(dir $@)
	@echo "    [T3D-MODEL] $@"
	$(T3D_GLTF_TO_3D) $(T3DM_FLAGS) "$<" $@
endif

# As in the jam's Makefile (DEBUG=1, FINAL=0): keep debug info, no extra asset compression.
N64_CFLAGS += -g -DDEBUG=1

# Harness files the minigames (and jam_boot.c's results screen) load.
ASSETS_LIST = $(FILESYSTEM_DIR)/squarewave.font64 \
              $(FILESYSTEM_DIR)/core/StartButton.sprite $(FILESYSTEM_DIR)/core/AButton.sprite \
              $(FILESYSTEM_DIR)/core/Start.wav64 $(FILESYSTEM_DIR)/core/Countdown.wav64 \
              $(FILESYSTEM_DIR)/core/Stop.wav64 $(FILESYSTEM_DIR)/core/Winner.wav64 \
              $(addprefix $(FILESYSTEM_DIR)/minigames/,$(addsuffix .desc,$(MINIGAMES)))
$(FILESYSTEM_DIR)/squarewave.font64: MKFONT_FLAGS += --outline 1 --range all

# ---- minigames: one relocatable object each, entry points renamed -----------------------------
# Compiled as for the jam's DSOs: -G 0 (no $gp-relative small data). This also matters for the
# symbol localization below: on MIPS, a GPREL16 relocation against a symbol that only becomes
# local after `ld -r` is resolved against the relocatable's own gp, 0x7ff0 off.
MG_SYMS = minigame_init minigame_loop minigame_fixedloop minigame_cleanup minigame_def
MG_DATA_SYMS = __jam_data_start __jam_data_end __jam_bss_start __jam_bss_end

# Our patches for the Fire TV remote (patches/<game>.patch: on-screen button names, the control
# legend, remote-friendly inputs). The jam checkout stays as fetched: the minigame's sources are
# copied to $(BUILD_DIR)/<game>-src and patched there (two levels down, like code/<game>, so the
# "../../core.h" includes still resolve), and the copy is compiled instead. A patch that no longer
# applies stops the build. Its new sprites come from assets-remote/<game>/*.png (our own art).
define PATCHED_template
$$(BUILD_DIR)/$(1)-src/.patched: $$(JAM24_DIR)/patches/$(1).patch $$(wildcard code/$(1)/*.[ch] code/$(1)/*/*.[ch])
	@echo "    [PATCH] $$<"
	@rm -rf $$(@D) && mkdir -p $$(@D) && cp -R code/$(1)/. $$(@D)/
	@patch -s -f -F 0 -p3 -d $$(@D) -i $$< </dev/null || \
		{ echo "error: $$< no longer applies to code/$(1); update it for this jam revision" >&2; exit 1; }
	@touch $$@
SRC_$(1) := $$(SRC_$(1):code/$(1)/%=$$(BUILD_DIR)/$(1)-src/%)
$$(SRC_$(1)): $$(BUILD_DIR)/$(1)-src/.patched ;
ASSETS_LIST += $$(patsubst $$(JAM24_DIR)/assets-remote/%.png,$$(FILESYSTEM_DIR)/%.sprite,$$(wildcard $$(JAM24_DIR)/assets-remote/$(1)/*.png))
endef

define MINIGAME_template
SRC_$(1) := $$(wildcard code/$(1)/*.c) $$(wildcard code/$(1)/*/*.c)
ifneq ($$(wildcard $$(JAM24_DIR)/patches/$(1).patch),)
$$(eval $$(call PATCHED_template,$(1)))
endif
$$(SRC_$(1):%.c=$$(BUILD_DIR)/%.o): CFLAGS += -G 0 -DN64_DSO
$$(BUILD_DIR)/mg_$(1).o: $$(SRC_$(1):%.c=$$(BUILD_DIR)/%.o) $$(JAM24_DIR)/minigame.ld
	@echo "    [MINIGAME] $$@"
	$$(N64_LD) -r -T $$(JAM24_DIR)/minigame.ld -o $$@.r $$(filter %.o,$$^)
	$$(N64_OBJCOPY) $$(foreach s,$$(MG_SYMS) $$(MG_DATA_SYMS),--keep-global-symbol=$$(s)) $$@.r $$@.l
	$$(N64_OBJCOPY) $$(foreach s,$$(MG_SYMS),--redefine-sym $$(s)=$(1)_$$(s)) \
		$$(foreach s,data_start data_end bss_start bss_end,--redefine-sym __jam_$$(s)=$(1)_$$(s)) \
		--rename-section .data.jam_minigame=.data.jam_$(1) --rename-section .bss.jam_minigame=.bss.jam_$(1) $$@.l $$@
	@rm -f $$@.r $$@.l
-include code/$(1)/$(1).mk
# Our changes to a minigame's asset list (overrides/<game>.mk), e.g. a free font in place of a
# commercial one.
-include $$(JAM24_DIR)/overrides/$(1).mk
endef
$(foreach m,$(MINIGAMES),$(eval $(call MINIGAME_template,$(m))))

# ---- asset conversion (the jam Makefile's rules) ----------------------------------------------
$(FILESYSTEM_DIR)/%.sprite: $(ASSETS_DIR)/%.png
	@mkdir -p $(dir $@)
	@echo "    [SPRITE] $@"
	$(N64_MKSPRITE) $(MKSPRITE_FLAGS) -o $(dir $@) "$<"
REMOTE_SPRITES := $(patsubst $(JAM24_DIR)/assets-remote/%.png,$(FILESYSTEM_DIR)/%.sprite,$(wildcard $(JAM24_DIR)/assets-remote/*/*.png))
$(REMOTE_SPRITES): $(FILESYSTEM_DIR)/%.sprite: $(JAM24_DIR)/assets-remote/%.png
	@mkdir -p $(dir $@)
	@echo "    [SPRITE] $@"
	$(N64_MKSPRITE) $(MKSPRITE_FLAGS) -o $(dir $@) "$<"
$(FILESYSTEM_DIR)/%.font64: $(ASSETS_DIR)/%.ttf
	@mkdir -p $(dir $@)
	@echo "    [FONT] $@"
	$(N64_MKFONT) $(MKFONT_FLAGS) -o $(dir $@) "$<"
$(FILESYSTEM_DIR)/%.wav64: $(ASSETS_DIR)/%.wav
	@mkdir -p $(dir $@)
	@echo "    [SFX] $@"
	$(N64_AUDIOCONV) $(AUDIOCONV_FLAGS) -o $(dir $@) "$<"
$(FILESYSTEM_DIR)/%.wav64: $(ASSETS_DIR)/%.mp3
	@mkdir -p $(dir $@)
	@echo "    [SFX] $@"
	$(N64_AUDIOCONV) $(AUDIOCONV_FLAGS) -o $(dir $@) "$<"
$(FILESYSTEM_DIR)/%.xm64: $(ASSETS_DIR)/%.xm
	@mkdir -p $(dir $@)
	@echo "    [XM] $@"
	$(N64_AUDIOCONV) $(AUDIOCONV_FLAGS) -o $(dir $@) "$<"
$(FILESYSTEM_DIR)/minigames/%.desc: $(ASSETS_DIR)/minigames/%.txt
	@mkdir -p $(dir $@)
	cp "$<" $@

$(BUILD_DIR)/$(TARGET).dfs: $(ASSETS_LIST)
	@echo "    [DFS] $@"
	@rm -rf "./$(BUILD_DIR)/fs"
	@for f in $(ASSETS_LIST:$(FILESYSTEM_DIR)/%=%); do mkdir -p "$(N64_MKDFS_ROOT)/$$(dirname $$f)"; cp "$(FILESYSTEM_DIR)/$$f" "$(N64_MKDFS_ROOT)/$$f"; done
	$(N64_MKDFS) $@ "$(N64_MKDFS_ROOT)" >/dev/null

# ---- the program --------------------------------------------------------------------------------
JAM_LIST := $(foreach m,$(MINIGAMES),X($(m)))
$(BUILD_DIR)/jam_boot.o: $(JAM24_DIR)/jam_boot.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -I. '-DJAM_MINIGAMES=$(JAM_LIST)' -o $@ $<

$(BUILD_DIR)/$(TARGET).elf: LDFLAGS += --wrap=dlopen --wrap=dlsym --wrap=dlclose
$(BUILD_DIR)/$(TARGET).elf: $(BUILD_DIR)/core.o $(BUILD_DIR)/minigame.o $(BUILD_DIR)/jam_boot.o \
                            $(foreach m,$(MINIGAMES),$(BUILD_DIR)/mg_$(m).o)

all: $(TARGET).z64
$(TARGET).z64: N64_ROM_TITLE=$(ROMTITLE)
$(TARGET).z64: $(BUILD_DIR)/$(TARGET).dfs

clean:
	rm -rf "./$(BUILD_DIR)" "./$(TARGET).z64"

-include $(wildcard $(BUILD_DIR)/*.d $(BUILD_DIR)/*/*.d $(BUILD_DIR)/*/*/*.d $(BUILD_DIR)/*/*/*/*.d)
.PHONY: all clean
.DEFAULT_GOAL := all
