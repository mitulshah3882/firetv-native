# Find-module shim for SuperTuxKart on Vega: the library is built in-tree from pinned source
# (engine/thirdparty/VegaDeps.cmake), so answer STK's find_package() with that target.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set(CURL_FOUND TRUE)
set(CURL_INCLUDE_DIRS ${CURL_INCLUDE_DIR})
set(CURL_LIBRARIES libcurl_static)
