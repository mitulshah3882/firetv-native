# Find-module shim for SuperTuxKart on Vega: the library is built in-tree from pinned source
# (engine/thirdparty/VegaDeps.cmake), so answer STK's find_package() with that target.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set(OGGVORBIS_FOUND TRUE)
set(OggVorbis_FOUND TRUE)
set(OGGVORBIS_INCLUDE_DIRS ${STK_VEGA_OGG_INCLUDES})
set(OGGVORBIS_LIBRARIES vorbisfile vorbis ogg)
