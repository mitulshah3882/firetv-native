# Prism Spire: Appstore listing kit

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
| `screenshot-1.jpg` … `screenshot-8.jpg` | the title, aiming at 6 m, 7.3 m, an event, a collapse, its replay, the results, the Daily Spire | 1920×1080 |
| `background-1920x1080.jpg` | optional background image | 1920×1080 |

Every image is a frame the game rendered itself: `make-kit.sh` runs the Mac build
(`../host-preview.sh`, the same renderer and shaders as the TV build) through its scripted modes and
keeps the frames listed in the script. The scripts are deterministic, so it reproduces these files.
The demo AI plays the party rounds; frames on the stick look the same (checked against
`gwsi-tool-screenshooter` captures of the title and of play).

## Listing text (draft)

**Title:** Prism Spire

**Short description** (Fire TV shows the first 200 characters):
> Stack glass prisms into a spire at sunset, then pass the remote. Whoever makes it fall owes the pizza. A party game for 2 to 8 players on one remote, plus a daily solo climb.

**Long description:**
> Prism Spire is a party game for the living room. Players take turns adding a glass piece to a spire on a stone plinth in a sunset lake. Move it, turn it, and drop it: it glides down to the tower and settles, or it doesn't. If anything falls off the plinth on your turn, you toppled it, and the tally of who owes the pizza goes up.
>
> One remote is all you need. Pick 2 to 8 players, then pass it along: the game tells you whose turn it is. From the sixth piece on, the dusk gets restless: wind pushes the spire, a tremor shakes the plinth, or the next piece comes out slippery as ice.
>
> On your own, climb the Daily Spire: everyone gets the same pieces and events today, and your best height is saved.
>
> The glass catches the low sun and bends the sky behind it, every piece that comes to rest rings a bell tuned to its height, and a collapse sends shards ringing into the lake, then plays again in slow motion from the other side. All the sound is synthesized live. The game runs natively at 60 frames per second.
>
> Controls: Left and Right move the piece, Up and Down turn it, OK drops it, and Back pauses. Gamepads work too, for whoever's turn it is.

**Feature bullets:**
- A pass-the-remote party game for 2 to 8 players: no phones, no extra controllers
- Glass prisms that refract the sunset, on a lake that reflects it
- Wind, tremors and slippery glass keep every round different
- The Daily Spire: the same pieces for everyone, every day
- A bell for every piece, a chime for every crash: all sound synthesized live

**Keywords:** party, stacking, tower, physics, family, multiplayer, pass and play, puzzle

**Category:** Games › Party (or Games › Casual)

## Answers for the console (draft)
- **Target audience:** all ages content, but not designed for children (choose 16+ to stay out of the
  child-directed programme; see the research doc, 3.4). The "pizza owed" tally is a joke, not a wager.
- **Content rating:** no violence (glass pieces break apart), no profanity, gambling, ads, accounts,
  location or user-generated content.
- **Privacy:** collects and transfers no data. The only thing stored is the Daily Spire's best heights,
  on the device (`/data/prism-spire.sav`).
- **Pricing:** free (Vega devices don't support paid apps).
- **Credits and licences:** Credits on the title screen; `assets/licenses/` in the package (Box2D, SDL,
  the Chakra Petch font).
- **Testing instructions** (the console's field for things a reviewer might not find):
  > Fire TV remote: on the title, Up/Down choose and Left/Right set the number of players for a party; OK starts. In a round, Left/Right move the hovering piece, Up/Down rotate it, and OK drops it; pass the remote to the player named on the left. A round ends when a piece falls off the plinth: a short replay of the collapse plays (OK skips it), then the results, where OK starts the next round and Back returns to the title. Back during play pauses; Back on the title exits. Daily Spire is a solo mode with the same pieces for everyone each day. After three minutes without input the sound goes quiet so the screensaver can start; any button brings it back. No account, network connection or purchase is involved.

## Before submitting
- A playtest with people: round length and the piece mix were tuned with the demo AI only
  (`DESIGN.md`, "Balance, measured").
- Re-check the open questions in the research doc (audio focus with Alexa; the store's test cases).
- Bump `build-number` in `manifest.toml` for every upload, including a Live App Test.
