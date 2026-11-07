/* doomgeneric's i_sound.c includes <SDL_mixer.h> when FEATURE_SOUND is set but uses nothing from it.
 * The Vega build mixes sound itself (i_sound_vega.c), so this empty shim satisfies the include. */
#pragma once
