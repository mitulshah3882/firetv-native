# Chroma Orbit: design

An original falling-block puzzle for the Fire TV remote. Coloured arcs fall inward, ring by ring,
toward a star at the centre of a planet. You turn the whole planet to choose where they land.
Concept and market notes: `docs/research/original-game-concepts.md` (C2). Built after Prism Spire,
on the same shell (SDL3, GLES 3.0 on the TV, desktop GL on the Mac), following decision 63's GPU
rules from the start.

## Board
- A polar grid: **8 rings** (ring 0 hugs the star) × **16 sectors**.
- A piece is a **pair**: two cells side by side in the same ring, each with one of **4 colours** (5
  from world 3). It appears beyond the outer ring at the top of the screen and falls inward one ring
  per tick.
- When a cell of the piece has a cell or the star directly inside it, the piece lands. Any cell of the
  pair left unsupported keeps falling on its own, so pairs split over edges, as in classic
  pair-droppers.

## Controls (remote first)
| Remote | Action |
|---|---|
| Left / Right | turn the planet one sector (the falling piece stays at the top) |
| Down (hold) | fall faster |
| OK | swap the pair's two colours |
| Up | hold the pair and take the held one (once per piece) |
| Back / Menu | pause |

## Clearing
- **Colour clusters:** 4 or more cells of one colour, connected along rings and spokes, dissolve.
- **Full rings:** a ring whose 16 sectors are all filled dissolves whatever its colours, and pays a
  bonus.
- After a clear, everything outside falls inward; new clusters and rings that form then clear too: a
  **chain**. Each chain step multiplies the score and plays the next chord stab higher.
- You lose when a new pair has no room beyond the outer ring.

## Stages and the star
- A stage is one world and one song. Clearing cells feeds the star's **energy** meter. When it is full
  (about 3–4 minutes of good play) the star goes **supernova**: it swells, swallows the rings, the
  screen flashes white on the downbeat, and the next world blooms in a new key with an empty board.
- Five worlds, each a palette plus one shader skin: nebula, aurora, ocean, ember, night city. Falls get
  faster each world.
- Endless after the fifth (a sixth and later loop the skins faster). Best score per mode in `/data`.

## Look
- The star: a procedural plasma (fbm) rendered at low resolution with limb darkening, bloomed.
- The cells: glossy extruded arc segments with a Fresnel rim and a reflection of the star.
- Solar-wind particles; cleared cells burst into particles that orbit and fall into the star.
- Danger (cells near the outer ring) dims the particles and brings in a detuned drone, so the board
  stays readable.

## Sound
- Each world is a song of stems synthesized live; the ring depth of the stack picks how many stems
  play. A clear is a chord stab; chains arpeggiate upward; the star's brightness is the master
  filter.

## Pressure: solar flares
Without pressure the rules are too forgiving: a one-move greedy AI that can reach any rotation never
lost in 2,000 pairs. So every few pairs the star **flares**: every cell moves one ring outward and a
new core ring appears with 12 of 16 sectors filled (it clears as a full ring once you fill the gaps).
Headless runs (`tools/rules-sim.c`, 200 games each; "reach" = sectors a player can turn before the
pair lands, falling with fall speed):

| Flare every | Reach | Colours | Strong AI | Sloppy AI (skill 0.6) |
|---|---|---|---|---|
| 6 pairs | 4 | 4 | 1,026 pairs | 579 |
| 5 | 3 | 4 | 216 | 163 |
| 6 | 4 | 5 | 714 | 393 |
| 4 | 3 | 4 | 170 | |

So the worlds ramp: flares every 9 pairs in world 1 down to 4 by world 6, falls faster each world
(less reach), and a fifth colour from world 3.

## Rules check before the art
The rules are prototyped with a plain renderer on the Mac first (scripted AI plus a few hand-played
runs) to answer: is a pair-dropper on a ring readable, and do full rings happen often enough to matter
next to colour clusters? Tune the ring and sector counts there.
