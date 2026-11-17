# Your to-do list

Things only you can do: your accounts, your ears, your hardware, your call. Claude keeps this list
current while it works. A one-line answer in chat is enough for any decision. Finished items move to
**Done** at the bottom with the date.

_Last updated: 2026-11-17._

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
  - **Chroma Orbit** (third card): each world now has its own song, and the bass and drums come in
    as the star's energy fills. Play a world through to its supernova and listen to the build.
- [ ] **Plug in a 4K stick** (developer mode on) and tell Claude. It runs every game and the GPU check
  on it. Only the HD stick has been tested so far.
- [ ] **Gamepad, if you have one:** pair a Bluetooth gamepad with the stick and tell Claude. Nothing has
  been tested with a pad: Gravity Choir's 2–4 players, SuperTuxKart and SuperTux.

- [ ] **Play Prism Spire with friends.** It's the second card in the arcade: 2–8 players pass the remote
  and stack glass until someone topples it. Claude can't playtest a party game; tell it what felt
  unfair, too easy or too hard (piece shapes, the plinth's width, how often wind, tremor and slippery
  glass happen). Since your last look: OK now lowers the piece gently onto the tower, and the tricky
  shapes only arrive after the fifth piece. With the computer playing carefully, a round lasts about
  11 pieces (up to 17); people should do better. How long should a round feel?

- [ ] **Try Chroma Orbit.** The third card in the arcade: a puzzle where coloured pairs fall toward a
  star and you turn the planet. Tell Claude if it's too fast, too slow, or confusing.

## Your Amazon developer account
- [ ] **Gravity Choir: Live App Test.** This is the first real test of whether the Appstore accepts a
  native Vega package. Claude prepares the package and listing beforehand; the steps are
  in `apps/gravity-choir/store/README.md` (section "Live App Test steps").
  Prism Spire and Chroma Orbit now have listing kits too (`apps/<game>/store/`), so once Gravity
  Choir's test shows the store accepts a native package, they can follow the same steps.

## Decisions
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
- 2026-11-16: **Open source or private?** Open source in this repo.
- 2026-11-16: **The next big project:** original games, Prism Spire then Chroma Orbit.
