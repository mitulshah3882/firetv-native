# Find-module shim for SuperTuxKart on Vega: the library is built in-tree from pinned source
# (engine/thirdparty/VegaDeps.cmake), so answer STK's find_package() with that target.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set(JPEG_FOUND TRUE)
set(JPEG_INCLUDE_DIRS ${JPEG_INCLUDE_DIR})
set(JPEG_LIBRARIES jpeg-static)
