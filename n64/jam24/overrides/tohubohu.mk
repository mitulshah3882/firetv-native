# Le Tohu-bohu's build overrides (included by ../Makefile after code/tohubohu/tohubohu.mk).
#
# Its own .mk adds -fms-extensions (anonymous struct members) for build/code/tohubohu/%.o, the jam's
# object directory; ours is $(BUILD_DIR), so the flag is set again here for the objects we compile.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
$(SRC_tohubohu:%.c=$(BUILD_DIR)/%.o): CFLAGS += -fms-extensions
