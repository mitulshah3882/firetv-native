# Gravity Choir: Appstore listing kit

Draft assets and text for an Amazon Appstore submission. Requirements and the open questions are in
[docs/research/appstore-submission.md](../../../docs/research/appstore-submission.md). Nothing here has
been submitted.

## Files
| File | Use | Size |
|---|---|---|
| `app-icon-1280x720.png` | Fire TV app icon (required; no transparency) | 1280×720 |
| `../icon/icon.png` | large icon (also the package's launcher icon) | 512×512 |
| `icon-114.png` | small icon | 114×114 |
| `screenshot-1.jpg` … `screenshot-5.jpg` | screenshots: title, three moments of play, the Crescendo | 1920×1080 |
| `background-1920x1080.jpg` | optional background image | 1920×1080 |

The icon, background and screenshots are frames rendered by the game itself: the Mac build (`../host-preview.sh`) runs the same renderer and shaders as the TV build, and its scripted modes (`keyart`, `title`, `play`, `crescendo`) have the demo AI play. To regenerate, run `host-preview.sh shots <script> [every] [count]` and pick frames. Before submitting, also capture a few frames on a stick (`gwsi-tool-screenshooter`) as a check that they match.

## Listing text (draft)

**Title:** Gravity Choir

**Short description** (Fire TV shows the first 200 characters):
> You are a star that sings. Pull a neon swarm into your gravity, flip your field to push, and smash them into each other: nothing shoots, and every crash is a note in the music. Chain crashes to climb the arpeggio, bring the band in with your multiplier, and when the meter fills, unleash the Crescendo. Made for the remote, with up to four players on gamepads.

**Long description:**
> Gravity Choir is an arcade game about gravity and music. You are a star in a glowing arena that bends around you. Enemies drift toward you, and you can't shoot them. Instead you pull them in with your field, flip it to push, and throw them into each other and into the walls. Every collision plays a note on the beat, taken from the chord of the moment, so a chain of crashes climbs an arpeggio and a good run sounds like a song.
>
> Keep the chain going and your multiplier rises; each step brings another part of the band in: pads, drums, bass, lead and choir. Crashes fill the Crescendo meter. Press Play when it's full and everything spirals into the centre for two bars, then detonates on the downbeat.
>
> The music is synthesized live as you play, with no recordings, across four movements in related keys. The game runs natively at 60 frames per second.
>
> Controls: the D-pad moves, OK flips your field between pull and push (hold it for a stronger field), Menu (or Play/Pause) starts the Crescendo, and Back pauses. Gamepads join as players two to four.

**Feature bullets:**
- Nothing shoots: enemies only break by colliding, so every kill is a throw
- Every crash is a note on the beat; chains climb an arpeggio
- Live-synthesized soundtrack that grows with your multiplier
- The Crescendo: a two-bar singularity that ends on the downbeat
- Plays with the Fire TV remote; up to four players with gamepads

**Keywords:** arcade, music, rhythm, neon, gravity, physics, score attack, party

**Category:** Games › Arcade

## Answers for the console (draft)
- **Target audience:** all ages content, but not designed for children (choose 16+ to stay out of the child-directed programme; see the research doc, 3.4).
- **Content rating:** no violence against people or animals (abstract shapes break apart), no profanity, gambling, ads, accounts, location or user-generated content.
- **Privacy:** collects and transfers no data. The only thing stored is the best score, on the device (`/data/gravity-choir.sav`).
- **Pricing:** free (Vega devices don't support paid apps).
- **Credits and licences:** Menu on the title screen; `assets/licenses/` and `assets/fonts/OFL.txt` in the package.
- **Testing instructions** (the console's field for things a reviewer might not find):
  > Fire TV remote: the D-pad moves your star. OK flips your gravity field between pull and push (hold OK for a stronger field). When the Crescendo meter along the bottom glows, press Menu (or Play/Pause) to trigger the Crescendo. Back pauses; Back on the pause screen returns to the title, and Back on the title exits. On the title screen, Menu shows the credits. Gamepads join as players 2 to 4 (A flips, X or Y triggers the Crescendo, Start pauses). After three minutes without input the music goes quiet so the screensaver can start; any button brings it back. No account, network connection or purchase is involved.

## Live App Test steps
A Live App Test (LAT) puts the app in the real Appstore for a list of testers only (you). It answers the
big open question: does the console accept a native-only package, and does it install on a stick from
the store? Steps from Amazon's LAT guide (`live_app_testing_get_started.md` in the Builder Tools docs):

1. **The package:** `apps/gravity-choir/build/armv7-release/local/packages/gravity-choir.vpkg`
   (version 1.0.0, build 2). Upload it exactly as built; don't re-sign it.
2. **Make room on the stick:** the sideloaded copy has the same package ID, so ask Claude to uninstall
   it from the stick first (or Settings → Applications → Manage Installed Applications → Gravity Choir
   → Uninstall).
3. Sign in at https://developer.amazon.com/dashboard → **My Apps** → **Add new app**. Fill in the New
   App Submission page (title "Gravity Choir"), then **Save**.
4. Under the app's name, select **Live App Testing** → **Create a new Live App Test** → **Confirm**.
5. Upload the VPKG. The console picks the supported devices from the file type (Vega devices for a
   VPKG); leave them all ticked.
6. Fill in the **Required Details**:
   - **App title:** Gravity Choir
   - **App category:** Games › Arcade
   - **App icon:** `../icon/icon.png` (512×512) and `icon-114.png` if it asks for a small one
   - **Language support:** English
   - **Content rating:** the answers under "Answers for the console" above (no violence against people
     or animals, no ads, no data collection, no accounts)
   - **Export compliance:** the app uses no encryption and makes no network connections
   - Leave availability (all countries), pricing (Free) and accelerated timelines (off) at their
     defaults.
   - Don't mention "Vega" in any field (Amazon's rule).
7. **Manage testers** → **Add new testers** → add your own email (the Amazon account the stick is
   signed in with) → **Create new tester group** (say, "me") → tick the consent statement → **Add**.
8. **Submit Test**, confirm the tester group. The invitation email can take several hours.
9. In the email, open the link for your marketplace → **Send to Device** → pick the stick. On the stick,
   open Gravity Choir and play a round; Back on the title exits.
10. Tell Claude what happened, including any console warnings or rejection text. If the console rejects
    the package, the exact message matters.

## Before submitting for real
- Re-check the open questions in the research doc: audio focus with Alexa, and the store's test cases
  (section 4.1).
- Bump `build-number` in `manifest.toml` for every upload, including another Live App Test.
