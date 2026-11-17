# Chroma Orbit: Appstore listing kit

Draft assets and text for an Amazon Appstore submission. Requirements and the open questions are in
[docs/research/appstore-submission.md](../../../docs/research/appstore-submission.md); the full Live
App Test walkthrough is in [Gravity Choir's kit](../../gravity-choir/store/README.md#live-app-test-steps)
(the same steps, with this game's names). Nothing here has been submitted.

## Files
| File | Use | Size |
|---|---|---|
| `app-icon-1280x720.png` | Fire TV app icon (required; no transparency) | 1280×720 |
| `../icon/icon.png` | large icon (also the package's launcher icon) | 512×512 |
| `icon-114.png` | small icon | 114×114 |
| `screenshot-1.jpg` … `screenshot-5.jpg` | the title, Ember, Aurora, a supernova, Night City | 1920×1080 |
| `background-1920x1080.jpg` | optional background image | 1920×1080 |

Every image is a frame the game rendered itself: `make-kit.sh` runs the Mac build
(`../host-preview.sh`, the same renderer and shaders as the TV build) through its scripted modes and
keeps the frames listed in the script. The scripts are deterministic, so it reproduces these files.
The demo AI plays (the `tour` script: a weaker player, and a supernova every 22 pairs so every world
appears). The icon and the background use a still, full board.

## Listing text (draft)

**Title:** Chroma Orbit

**Short description** (Fire TV shows the first 200 characters):
> Coloured pairs fall toward a star. Turn the whole planet to place them, match four, fill rings, and chain clears until the star goes supernova and carries you to the next world.

**Long description:**
> Chroma Orbit is a falling-block puzzle bent into a circle. Pairs of coloured cells fall inward, ring by ring, toward the star at the centre of a planet. You don't move the pair: you turn the planet under it.
>
> Four or more cells of one colour that touch dissolve, and so does any full ring. Whatever was outside falls in, and when that sets off another clear, it's a chain, worth more with every step. Every few pairs the star flares and pushes everything outward, so the board never stays quiet.
>
> Clearing cells feeds the star. When its energy is full, it swells, swallows the rings and goes supernova, and you arrive in the next world: Nebula, Aurora, Ocean, Ember and Night City, each with its own sky, each a little faster. Your best score is saved.
>
> Every clear rings out on the beat, chains climb higher, and all of it is synthesized live. The game runs natively at 60 frames per second.
>
> Controls: Left and Right turn the planet, Down makes the pair fall faster, OK swaps its colours, Up holds it for later, and Back pauses. Gamepads work too.

**Feature bullets:**
- A falling-block puzzle on a ring: you turn the planet, not the piece
- Colour clusters, full rings and chain reactions
- Solar flares push the board outward: no two games settle the same way
- Supernovas carry you through five worlds, each with its own sky
- Made for the Fire TV remote; all sound synthesized live

**Keywords:** puzzle, match, falling blocks, arcade, space, colour, chain, casual

**Category:** Games › Puzzles & Trivia (or Games › Casual)

## Answers for the console (draft)
- **Target audience:** all ages content, but not designed for children (choose 16+ to stay out of the
  child-directed programme; see the research doc, 3.4).
- **Content rating:** no violence, profanity, gambling, ads, accounts, location or user-generated
  content.
- **Privacy:** collects and transfers no data. The only thing stored is the best score, on the device
  (`/data/chroma-orbit.sav`).
- **Pricing:** free (Vega devices don't support paid apps).
- **Credits and licences:** Credits on the title screen; `assets/licenses/` in the package (SDL, the
  Chakra Petch font).
- **Testing instructions** (the console's field for things a reviewer might not find):
  > Fire TV remote: on the title, Up/Down choose and OK starts. In play, Left/Right turn the planet (the falling pair stays at the top), Down makes it fall faster, OK swaps the pair's colours, and Up holds the pair for later. Back pauses; Back on the pause screen resumes, and its TITLE item returns to the title. Back on the title exits. A game ends when a new pair has no room; OK plays again. After three minutes without input the sound goes quiet so the screensaver can start; any button brings it back. No account, network connection or purchase is involved.

## Before submitting
- A playtest with people: the fall speed, flare rate and world goal were tuned with the demo AI and
  `tools/rules-sim.c` only (`DESIGN.md`).
- Re-check the open questions in the research doc (audio focus with Alexa; the store's test cases).
- Bump `build-number` in `manifest.toml` for every upload, including a Live App Test.
