# Find-module shim for SuperTuxKart on Vega: the library is built in-tree from pinned source
# (engine/thirdparty/VegaDeps.cmake), so answer STK's find_package() with that target.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set(OPENAL_FOUND TRUE)
set(OpenAL_FOUND TRUE)
set(OPENAL_INCLUDE_DIR ${STK_VEGA_OPENAL_INCLUDES})
set(OPENAL_LIBRARY OpenAL)
