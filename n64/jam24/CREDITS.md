# N64brew Game Jam 2024 minigames: credits and licences

Source: [github.com/n64brew/N64brew-GameJam2024](https://github.com/n64brew/N64brew-GameJam2024)
at `0dea720`, fetched as a sparse checkout by `tools/fetch-deps.sh n64jam2024` (only the harness,
the harness assets listed below, and the minigames we build). Nothing from it is committed here.

The jam repository is MIT licensed (Copyright (c) 2024 N64brew). Our files in `n64/jam24/`
(the static-link Makefile, `jam_boot.c`, the Fire TV remote patch `patches/landgrab.patch` and
its button icons in `assets-remote/`, drawn by `assets-remote/make-icons.py`) are our own, MIT.

## Land Grab (`code/landgrab`, `assets/landgrab`)
- Code: Meeq (Christopher Bonhage), MIT. Game concept: "Blokus" by Bernard Tavitian.
- Fonts: "Anita Semi-Square" by Gustavo Paz L. (CC BY 3.0); "Banana Brick" by artmaker (SIL OFL 1.1).
- Music: "15 Years Before" by Ultrasyd and "4 Weeks Of Hysteria" by Phekkis, both **CC BY-NC 4.0**
  (fine for this free project; they must be credited wherever the game ships, and it can't be sold).
- Sound effects: `buzz.wav` by TannerSound (CC BY 3.0), `click.wav` by kwahmah_02 (CC0),
  `pop.wav` by deathbyfairydust (CC BY 4.0), all from freesound.org.
- Sprites (logo, background, cursors, button icons): no separate credit; part of the MIT repo.
- Our remote patch (`patches/landgrab.patch`, applied at build time to a copy of the code) makes
  the hints, the pause screen and the control legend name the Fire TV remote's buttons, and draws
  the legend with our own remote-button icons instead of the joypad ones (which stay unused).

## Mallard (`code/mallard`, `assets/mallard`)
- Code: Josh Kautz, MIT.
- **Not cleared for shipping yet.** The repo documents no source or licence for:
  - the two XM tracks (`mallard_intro_music.xm`, `mallard_game_music.xm`): confirm with the
    author or replace them before publishing a build;
  - the fonts "HaloDek" (reported free for commercial use; confirm) and "Celtic Garamond the 2nd"
    (only loaded: the intro paragraphs that use it are disabled in the game's code);
  - the duck, snowman and park art (presumably the author's own).

## Harness assets the two minigames load
`assets/squarewave.ttf` (the jam's UI font, also used by our results screen),
`assets/core/StartButton.png`, `AButton.png`, `Start.wav`, `Countdown.wav`, `Stop.wav`,
`Winner.wav`. Part of the MIT jam template; their original sources are not documented, so confirm
them before shipping too.
