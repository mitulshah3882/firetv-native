# N64brew Game Jam 2024 minigames: credits and licences

Source: [github.com/n64brew/N64brew-GameJam2024](https://github.com/n64brew/N64brew-GameJam2024)
at `0dea720`, fetched as a sparse checkout by `tools/fetch-deps.sh n64jam2024` (only the harness,
the harness assets listed below, and the minigames we build). Nothing from it is committed here.

The jam repository is MIT licensed (Copyright (c) 2024 N64brew). Our files in `n64/jam24/`
(the static-link Makefile, `jam_boot.c`, the Fire TV remote patches in `patches/`, the asset
overrides in `overrides/`, and the button icons in `assets-remote/`, drawn by
`assets-remote/make-icons.py`) are our own, MIT.

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

## Hydra Harmonics (`code/hydraharmonics`, `assets/hydraharmonics`)
- By Catch-64: N64 Squid (lead programmer), Gary Jones III (art director, composer), Brozilla
  (audio conversion), StatycTyr (additional art). Code, sprites and the XM music
  (`hydraharmonics.xm`) are the team's own, in the MIT repo (credits: `code/hydraharmonics/readme.md`).
- Sound effects (`aah`, `eww`, `gulp`, `ow`, `hit-0..3`, `slide-whistle-up/down`, `drumroll`):
  in the MIT repo; their original sources are not documented upstream (like the harness's).
- **Font replaced.** The game ships "Superclarendon", a commercial font we can't
  redistribute: `tools/fetch-deps.sh` keeps it out of the checkout, and our build uses
  "Coustard" by Vernon Adams (SIL OFL 1.1, pinned in `tools/fetch-deps.sh`), at the same size and
  outline (`overrides/hydraharmonics.mk`).
- Our remote patch (`patches/hydraharmonics.patch`) loads Coustard, says OK instead of A, and
  draws the controls panel with our own remote icons (`assets-remote/hydraharmonics`): Up, Down
  and OK, in place of the joypad's D-pad, C, L, R, A and Z icons.

## Harness assets the minigames load
`assets/squarewave.ttf` (the jam's UI font, also used by our results screen),
`assets/core/StartButton.png`, `AButton.png`, `Start.wav`, `Countdown.wav`, `Stop.wav`,
`Winner.wav`. Part of the MIT jam template; their original sources are not documented, so confirm
them before shipping too.

## Sword Strike (`code/swordstrike`, `assets/swordstrike`)
- By Super Boognish (`assets/minigames/swordstrike.txt`), in the MIT repo: code, the fighter sprites,
  the two backdrop models (`background_cube_*.glb`) and the sand texture.
- 3D through tiny3d (MIT, Max Bebök), pinned at `6f5ac0b` (`tools/fetch-deps.sh tiny3d`).
- **Sounds replaced.** The game's two WAVs aren't fetched: `wilhelm_scream.wav` (the knock-out; the
  Wilhelm scream's provenance is murky) and `challengers.wav` (the fight music; no documented source).
  `overrides/swordstrike-sfx.py` synthesizes our own under the same names: a sliding square-wave yelp
  and an 8-bar chiptune loop in A minor (MIT).
- Packaged as `apps/n64-swordstrike` (2026-11-18), its 3D backdrop drawn by our host model of
  tiny3d's microcode. Remote: OK jumps, Play/Pause attacks, Rewind/Fast-forward slide (the runtime's
  `N64_REMOTE_MAP`); a remote patch to make one-thumb play comfortable is still to do.
