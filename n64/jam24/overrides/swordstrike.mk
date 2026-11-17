# Sword Strike's asset overrides (included by ../Makefile after code/swordstrike/swordstrike.mk).
#
# Its two WAVs are not fetched: the Wilhelm scream's provenance is murky and `challengers.wav` (the
# fight music) has no documented source. swordstrike-sfx.py synthesizes our own sounds under the
# same names, so the game's code loads them unchanged.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
SS_SFX_DIR := $(BUILD_DIR)/swordstrike-sfx
$(SS_SFX_DIR)/.done: $(JAM24_DIR)/overrides/swordstrike-sfx.py
	@echo "    [SYNTH] $(SS_SFX_DIR)"
	python3 -I $< $(SS_SFX_DIR) && touch $@
$(SS_SFX_DIR)/wilhelm_scream.wav $(SS_SFX_DIR)/challengers.wav: $(SS_SFX_DIR)/.done ;
$(FILESYSTEM_DIR)/swordstrike/wilhelm_scream.wav64 $(FILESYSTEM_DIR)/swordstrike/challengers.wav64: $(FILESYSTEM_DIR)/swordstrike/%.wav64: $(SS_SFX_DIR)/%.wav
	@mkdir -p $(dir $@)
	@echo "    [SFX] $@"
	$(N64_AUDIOCONV) $(AUDIOCONV_FLAGS) -o $(dir $@) "$<"
