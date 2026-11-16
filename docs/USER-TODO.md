# Your to-do list

Things only you can do: your accounts, your ears, your hardware, your call. Claude keeps this list
current while it works. A one-line answer in chat is enough for any decision. Finished items move to
**Done** at the bottom with the date.

_Last updated: 2026-11-16._

> **Pushing:** Claude keeps pushing finished work to GitHub under your OK of 2026-11-15 ("feel free to
> push"). Say so if you'd rather it hold commits locally.

## On the TV (about 20 minutes)
- [ ] **Listen to the games.** Nobody has heard this week's audio changes through a TV. Tell Claude
  about anything harsh, clipped, crackly, too quiet or too loud.
  - **Gravity Choir:** play until the multiplier is high (each step brings in another part of the
    band), then fire the Crescendo when its meter is full.
  - **Brew Volley** (N64 card in the arcade): the hit and whistle sounds.
  - **Land Grab** (N64 card): the music.
  - **Hydra Harmonics** (new, last card in the arcade): a four-player N64 jam game about hydras
    eating music notes. Up and Down move, OK chomps. Its music and voices haven't been heard yet.
- [ ] **Plug in a 4K stick** (developer mode on) and tell Claude. It runs every game and the GPU check
  on it. Only the HD stick has been tested so far.
- [ ] **Gamepad, if you have one:** pair a Bluetooth gamepad with the stick and tell Claude. Nothing has
  been tested with a pad: Gravity Choir's 2–4 players, SuperTuxKart and SuperTux.

## Your Amazon developer account
- [ ] **Gravity Choir: Live App Test.** This is the first real test of whether the Appstore accepts a
  native Vega package. Claude prepares the package and listing beforehand; the steps are
  in `apps/gravity-choir/store/README.md` (section "Live App Test steps").

## Decisions
- [ ] **Which original game next?** Only our own games can go to the store without legal questions.
  The research picks (`docs/research/original-game-concepts.md`):
  - **Chroma Orbit**, a synesthetic colour-matching puzzle. Best fit for a remote and the widest
    audience. **Claude's pick:** it reuses Gravity Choir's renderer and synth.
  - **Prism Spire**, a pass-the-remote party stacker for groups, no gamepads needed.
  - **Phosphor**, the smallest: you are the electron beam, and the shape you draw is the sound you
    hear.
- [ ] **Open source or private for future original games?** Gravity Choir is already public under
  MIT (it was pushed on 2026-11-15, and that can't be taken back). If you might want to sell a
  game, or keep its code to yourself, the next one should start in a private repo that uses this
  repo's MIT engine as a dependency. Claude's pick: keep Gravity Choir here, and decide this before
  the second original game starts.
- [ ] **The GPL ports and the store.** Claude recommends keeping Doom, Quake, SuperTux and SuperTuxKart
  sideload-only and submitting only our own games. Putting any of them in the store first needs
  answers to `docs/research/appstore-submission.md` section 6: the SDK code linked into GPL apps,
  two non-commercial SuperTux assets, the Doom and Quake trademarks, and the upstream projects'
  consent.
- [ ] **Doom and Quake: moving while firing.** On a remote it takes two thumbs. Options: leave it
  (Claude's pick, since these stay sideload-only), an auto-fire toggle, or fire on a second key.
- [ ] **Case study for your website:** want Claude to write it from the journal now, or wait until
  the first store submission?

## On the Mac
- [ ] **Free 1.5 GB of old debug files.** Claude isn't allowed to run a wildcard delete, so either run
  this from the repo or tell Claude to go ahead. The Mac is down to 12 GB free.
  ```bash
  rm -rf apps/*/build/debug/.build-id/*
  ```

## Done
_(nothing yet)_
