# Hydra Harmonics' asset overrides (included by ../Makefile after code/hydraharmonics/hydraharmonics.mk).
#
# The game ships Superclarendon, a commercial font we can't redistribute (tools/fetch-deps.sh leaves
# it out of the checkout). Its text uses Coustard instead, an OFL slab serif in the same Clarendon
# style, at the same size and outline; patches/hydraharmonics.patch loads it.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
HH_FONT_TTF := $(JAM24_DIR)/../../third_party/_src/fonts/Coustard-Regular.ttf
ASSETS_LIST := $(filter-out $(FILESYSTEM_DIR)/hydraharmonics/Superclarendon-Regular-01.font64,$(ASSETS_LIST))
ASSETS_LIST += $(FILESYSTEM_DIR)/hydraharmonics/Coustard-Regular.font64
$(FILESYSTEM_DIR)/hydraharmonics/Coustard-Regular.font64: $(HH_FONT_TTF)
	@mkdir -p $(dir $@)
	@echo "    [FONT] $@"
	$(N64_MKFONT) --outline 1 --size 12 -o $(dir $@) "$<"
