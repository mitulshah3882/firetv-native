# Find-module shim for SuperTuxKart on Vega: the library is built in-tree from pinned source
# (engine/thirdparty/VegaDeps.cmake), so answer STK's find_package() with that target.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set(FREETYPE_FOUND TRUE)
set(Freetype_FOUND TRUE)
set(FREETYPE_INCLUDE_DIRS ${STK_VEGA_FREETYPE_INCLUDES})
set(FREETYPE_LIBRARIES freetype)
set(FREETYPE_LIBRARY freetype)
