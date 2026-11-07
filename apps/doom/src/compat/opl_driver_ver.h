/* Chocolate Doom's i_oplmusic.c expects this enum from its i_sound.h; doomgeneric's i_sound.h predates
 * it. Force-included only into i_oplmusic.c. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
typedef enum
{
    opl_doom1_1_666, /* Doom 1 v1.666 */
    opl_doom2_1_666, /* Doom 2 v1.666, Hexen, Heretic */
    opl_doom_1_9     /* Doom v1.9, Strife */
} opl_driver_ver_t;
void I_SetOPLDriverVer(opl_driver_ver_t ver);
