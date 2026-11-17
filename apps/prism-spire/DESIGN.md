# Prism Spire: design

An original pass-the-remote party game for Fire TV: players take turns stacking glass prisms into a
tower on a stone plinth at sunset. Whoever makes it fall loses the round. One remote, 2–8 players,
no phones or gamepads needed. Solo, a daily seed gives everyone the same pieces for a height
challenge. Concept and market notes: `docs/research/original-game-concepts.md` (C3).

## Rules
- **A turn:** the current player's piece hovers above the tower. Left/Right move it, Up/Down rotate it
  by 15°, OK drops it: the piece glides down its beam to just above whatever is under it and is let
  go there, so a careful drop is gentle. The tower then settles: either everything comes to rest (next
  player), or a piece falls off the plinth (the player who dropped loses the round).
- **Pieces:** convex glass prisms (bar, block, triangle, wedge, trapezoid, hexagon, plank) plus one
  two-part L. The next piece is shown in a corner. Every piece is extruded to the same depth, so the
  tower is a 2D physics problem drawn in 3D.
- **The deal ramps:** the first five pieces of a round are friendly shapes; the triangle, the wedge and
  the L come in after that, at full odds by the 13th piece.
- **Balance, measured** (`host-preview.sh shots tall 18000 1`, the demo AI building carefully, about
  seven 4-player rounds in 5 minutes; the `round over` log line): dropping from the hover height (2.4 m,
  about 6 m/s at impact) with every shape from the start, rounds averaged 8.1 pieces and 3.1 m. The
  ramp alone: 9.4 pieces, 4.1 m. The ramp plus the glide: 10.7 pieces, 4.9 m, the best round 17 pieces
  and 7.8 m. A person aiming with the beam should do better than the AI; this wants a playtest.
- **Events** (party and solo, from the 6th piece on, about one turn in four), announced on a banner
  before the drop:
  - **Wind:** a steady sideways push on the whole tower for the turn, shown by drifting streaks.
  - **Tremor:** the plinth shakes for a second and a half after the drop.
  - **Slippery glass:** this piece has almost no friction; it looks frosted with ice.
- **Party:** pick 2–8 players on the title. Each player has a colour; the tower's pieces take the
  colour of whoever placed them. A round ends at the first collapse; the results show who toppled it,
  the height reached, and the running count of collapses per player ("pizzas owed"). OK starts the
  next round with the next player.
- **Solo (Daily Spire):** the piece sequence and events come from the date. Climb as high as you can;
  the height is the score. Today's best and the all-time best are saved in `/data`.

## Controls (remote first)
| Remote | Gamepad | Action |
|---|---|---|
| Left / Right (hold to move faster) | D-pad, stick | move the piece |
| Up / Down | D-pad up/down, shoulder buttons | rotate 15° |
| OK | A | drop |
| Menu | Y / Start | pause menu |
| Back | B | pause; on the title, exit |

No media keys (Amazon's test criteria for non-media apps). A tiny sway of the hovering piece shows it
is "live".

## Look
- **Glass** is drawn opaque but looks transparent: each pixel refracts a half-resolution copy of the
  background along its normal, with a per-channel offset (dispersion), a Fresnel mix with an analytic
  sunset sky, a warm sun highlight, and bevelled edges that catch the light. No sorting, no blending.
- **Scene:** a dusk gradient sky with a low sun and a few early stars, three layers of low-poly hills in
  atmospheric perspective, a stone plinth whose top carries coloured caustics cast by the glass.
- **Collapse:** slow motion, the camera swings round, pieces that hit the ground shatter into shards
  (small glass prisms in the physics world too), and every impact rings a chime. Then a 4 s replay:
  the last 6 s of every piece's position are kept from each drop on (with the chimes and shatters),
  and the moments around the collapse play again, letterboxed, from a camera that cuts to the other
  side and sweeps round the plinth. OK skips it.
- 4× MSAA on the 3D pass; the UI is SDF text from the same font as Gravity Choir (Chakra Petch).

## Sound (all synthesized)
- Each piece that comes to rest rings a glass bell tuned to its height: the tower climbs a pentatonic
  scale.
- While the tower sways, a glass-harmonica tone swells with the sway.
- Collapse: every physics impact plays a chime, pitched from the scale and as loud as the hit.
- A quiet dusk pad underneath, changing chord as the tower grows.

## Technical notes
- Box2D 3.1.1 (MIT) for the physics, built from source with SIMD off (its NEON path targets 64-bit
  ARM; a few dozen bodies don't need it). Fixed 60 Hz steps, 4 substeps.
- Same platform shell as Gravity Choir: SDL3 with GLES 3.0 on the TV, desktop GL 3.3 on the Mac
  (`host-preview.sh`), scripted runs that render 1080p PNGs for checking and for store art.
- Budget (Mali-G310, `docs/research/gpu-bench.md`): background pass at 960×540, one MSAA 1080p pass
  where glass covers maybe a quarter of the screen, UI on top. Target: a steady 60 fps.
