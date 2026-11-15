/* Empty stand-in for <EGL/eglext.h>. Vega's SDK ships no GLES/EGL headers (GL is dlopen()ed); STK's GLES
 * build includes this header in two files but uses only glad's GL declarations, and creates its
 * context through SDL, not EGL. SPDX-License-Identifier: GPL-3.0-or-later */
