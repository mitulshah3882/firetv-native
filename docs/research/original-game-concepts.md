<!-- Market research by a Claude Code subagent, 2026-11-14. Sources at the end. -->

# Original game concepts for Fire TV (Vega OS), built natively in C on GLES 3.1

_Research-only report, 2026-11-14. Nothing in the repo or on the device was changed._

Existing games are cited only as references for genre and style. Every concept below is original: no characters, IP, names or rule sets are taken from them. All names are **working titles**, and each needs a trademark search before it goes public.

---

## TL;DR
- **Market.** TV gaming in 2025–26 is **casual, social and played with the remote**.
  - In late 2025, Netflix, Amazon Luna and Samsung all made party games with phone controllers their main TV push.
  - Arcade and puzzle titles fill the TV app stores. Sessions last minutes. Most households don't own a gamepad, or don't connect one.
  - Local Fire TV gaming has been "stale for years", and nobody ships a native game on Vega that looks like a console game. That gap is our story.
- **Hardware reality.** The GPU is a Mali-G310: one shader core at up to 500 MHz.
  - Public spec ranges put it at roughly **16–64 GFLOPS** and **1–2 Gpix/s**. None of this is measured yet.
  - That gives about **130–510 FLOPs per pixel at 1080p60 for the whole frame**.
  - On this budget, "premium" means:
    - thin, bright geometry at native 1080p with 4x MSAA, which is close to free on a tiler
    - anything ALU-heavy rendered at 540p or lower
    - one dual-filter bloom chain
    - small additive GPU particles
    - one composite pass
- **Recommendation:**
  1. **Gravity Choir.** A neon arena game where you flip polarity instead of twin-stick aiming. It has the most flash for the least work, and its renderer becomes our "neon engine".
  2. **Chroma Orbit.** A synesthetic puzzle where pieces fall inward onto a star. It has the best fit with the remote and the strongest market evidence.
  3. **Prism Spire.** A glass-tower stacker that a party plays by passing one remote around. It's the social option, and it needs no phones and no gamepads.
- **First steps.** Run a **half-day GPU benchmark** (section 6) first, then a **1-day "Neon Proof"** of Gravity Choir's look on the stick (section 7).

---

## 1. Market research: what works on a TV with a remote

### 1.1 Platforms are betting on casual and party play
- **Netflix** put its first TV games in front of members on **Oct 9, 2025**:
  - The launch line-up was LEGO Party!, Boggle Party, Pictionary: Game Night, Tetris Time Warp and Party Crashers, all **with the phone as the controller**. Boggle Party takes up to 8 players. ([TechCrunch](https://techcrunch.com/2025/10/09/netflix-is-letting-users-play-games-on-their-tv-screens))
  - Netflix organises its games around four pillars: narrative, party, kids and mainstream. ([GamesHub](https://www.gameshub.com/?p=2690254))
  - By January 2026 it described its TV games strategy as **"cloud-first"**. Party titles drove "strong engagement growth", and about a third of members could reach TV games. ([PocketGamer.biz](https://www.pocketgamer.biz/netflix-unveils-new-cloud-first-direction-for-its-games-strategy/))
  - Party games reportedly reached only about 10% of eligible members, "off a small base". ([StreamTV Insider](https://www.streamtvinsider.com/content/netflix-tv-party-games-see-strong-uptake-initial-rollout))
  - The co-CEO graded the gaming effort a "B-". ([Spokesman-Review/WaPo](https://www.spokesman.com/stories/2025/nov/14/netflix-is-revamping-its-gaming-strategy-to-win-mo))
- **Amazon** relaunched Luna in October 2025 around **GameNight**: party games played with a phone, joined by QR code, with no app download, and included with Prime. Deeper games still need a controller. ([Engadget](https://www.engadget.com/gaming/amazon-luna-will-offer-controller-free-party-games-in-an-attempt-to-woo-prime-subscribers-130004416.html), [VGC](https://www.videogameschronicle.com/news/amazons-redesigned-luma-launches-with-gamenight-its-party-library-with-a-ai-snoop-dogg-courtroom-game))
  - In July 2026, Luna moved into the Prime Video app on Fire TV. ([Advanced Television](https://www.advanced-television.com/2026/07/24/amazon-adds-luna-games-to-prime-video/))
- **Samsung** reports that unique casual and party game users on its TVs grew **more than 90% in North America** and more than 200% globally year on year (Oct 2026, company figures). ([PPC Land](https://ppc.land/samsung-tvs-gain-over-90-more-casual-and-party-gamers-in-north-america/))
- **LG** reports gaming usage on webOS up 67% and hours played up 71% year on year. It brought *8 Ball Pool* to TVs with a "Couch Play" mode. ([BriefGlance](https://briefglance.com/articles/the-big-screen-pivot-why-lg-and-miniclip-are-betting-the-living-room-on-casual-gaming))
- **On Fire TV specifically**, AFTVnews says cloud gaming (Luna) is the most popular kind, and the local game selection has been "very stale for years". ([AFTVnews](https://www.aftvnews.com/?p=40199))
  - Vega OS officially supports only React Native and web apps ([CNX Software](https://www.cnx-software.com/2025/10/03/linux-based-vega-os-replaces-android-based-fire-os-in-amazon-fire-tv-stick-4k-select/)), so a native GLES game that looks like a console game is a novelty in its own right.
- **Naavik's sober take** (Oct 2025):
  - **What works:** party games are the current beachhead, and most TV-store games are "basic, arcade-style".
  - **Input:** TV remotes are built for menus, and most users don't own a controller or won't connect one.
  - **Retention:** party games "are rarely played regularly". Netflix's phone controller app has only about 1.5M MAU (Sensor Tower).

  ([Naavik](https://naavik.co/digest/the-promise-and-perils-of-smart-tv-gaming/))

### 1.2 Engagement: which genres work on a couch with a remote
- **Puzzle, arcade score-attack, party and trivia** dominate. Netflix chose a Tetris title for its TV launch, and Luna and Netflix both lead with party games.
- **Proof that remote-first design can carry a prestige game:**
  - *Sayonara Wild Hearts* is controlled with "a directional input and a button". It was built to work on the Apple TV remote and won **Apple Arcade Game of the Year 2019**. ([Apple Developer](https://developer.apple.com/news/?id=33kvkagk), [Simogo](https://www.simogo.com/sayonara-wild-hearts/), [MacRumors](https://www.macrumors.com/2019/12/03/apple-best-of-2019-awards/))
  - Apple still asks tvOS games to support the Siri Remote "where possible", although it has allowed games to require a gamepad since 2016. ([MacRumors](https://macrumors.com/2016/06/14/apple-drops-siri-remote-game-controller-demand))
- **Phone-as-controller is the industry's answer for party games.** Jackbox, Netflix and Luna all use it. Jackbox's own pitch is "No big mess of controllers needed". ([Steam](https://store.steampowered.com/app/331670/Jackbox_Party_Pack/))
  - Jackbox had its "best year ever" in 2020. ([DualShockers/Bloomberg](https://www.dualshockers.com/jackbox-games-200-million-players-bloomberg-report/))
- **Appetite is real but narrow:**
  - 28% of US TV gamers play short casual games daily.
  - Of connected-TV owners, only 14% are interested in casual TV games, and 52% are not interested at all.

  ([Statista/Vorhaus](https://statista.com/statistics/1476054/casual-games-via-tv-usa-frequency-age-group), [Statista](https://www-statista-com.ezproxy.canberra.edu.au/statistics/1476068/interest-in-playing-casual-games-via-connected-tv-usa-age-group))

### 1.3 Session length
- **GameAnalytics 2025 benchmarks:** top-tier mobile games average about **8–9 minutes per session**, and median games **5–6 minutes**. Sessions are getting shorter. ([GameAnalytics via GameIndustryLibrary](https://gameindustrylibrary.com/documents/gameanalytics-mobile-gaming-benchmarks-2025))
- **American Time Use Survey:** game playing averages **24 minutes a day**, against 2 h 37 min of video. ([PPC Land citing VAB/ATUS](https://ppc.land/samsung-tvs-gain-over-90-more-casual-and-party-gamers-in-north-america/))
- **Design precedents:**
  - Super Hexagon's very short runs were deliberate: "Because it's so short, it's... inviting". ([Wikipedia](https://en.wikipedia.org/wiki/Super_Hexagon))
  - Pac-Man CE DX is built around **5- and 10-minute score attacks**. ([GameSpot](https://www.gamespot.com/reviews/pac-man-championship-edition-dx-review/1900-6284565/))
- **Implication:** aim for **3–5 minute runs**, restart in one button press, an attract mode, and a party format of about 15–30 minutes.

### 1.4 Local multiplayer
- About **20% of gamers** name couch co-op as a favourite way to play, rising to 28% of 16–19-year-olds (MIDiA, Q2 2024). ([Statista](https://www.statista.com/statistics/1607985/preference-of-couch-co-up-gaming-by-age))
- Co-op "is the only game mode whose appeal is stable with age" (Quantic Foundry). ([Game Developer](https://gamedeveloper.com/design/survey-co-op-more-popular-than-competitive-multiplayer-especially-if-it-s-local))
- On consoles, demand for party games is huge: *Super Mario Party Jamboree* shipped **7.48M copies** in its first fiscal year. ([Gamereactor](https://www.gamereactor.eu/nintendo-made-24-switch-titles-million-sellers-last-fiscal-year-1539743/))
- **The catch on Fire TV:** a typical home has **one remote and no gamepad** (Naavik). Local multiplayer therefore has to work in one of three ways:
  - **pass the remote** (turn-based)
  - **remote plus optional gamepads**
  - **phones**, which need LAN networking from a native app. That is unverified on Vega: the only network privilege we found documented is `com.amazon.network.privilege.net-info`, for the React Native NetInfo library ([Vega docs](https://developer.amazon.com/docs/vega-api/2.0/react-native-net-info)).

### 1.5 Design rules that follow from the evidence
1. **The remote is the controller.** Use the D-pad, OK, Back and Play/Pause, with no aiming and no chords. Gamepads add players; they're never required.
2. **Runs of 3–5 minutes**, a one-press restart, an attract mode, and a "daily seed" for a reason to come back.
3. **Make it social without phones.** Pass-the-remote or remote-plus-gamepads now. Phone controllers stay a stretch goal until LAN sockets are proven.
4. **Spectacle is the differentiator.** Nobody else on Vega can do native GLES 3.1, compute, MSAA and a live synth. Lean on that.

---

## 2. Visual styles that look premium on a Mali-G310

### 2.1 The budget (public specs, not yet measured; section 6 measures them)
- **The SoC.** MediaTek MT8698: 4× Cortex-A55 at 1.7 GHz, with a **Mali-G310 v2 at up to 500 MHz**. ([CNX Software](https://www.cnx-software.com/2025/10/03/linux-based-vega-os-replaces-android-based-fire-os-in-amazon-fire-tv-stick-4k-select/))
  - The new 3rd-gen Fire TV Stick 4K uses nearly identical silicon. ([AFTVnews](https://www.aftvnews.com/amazon-announces-new-3rd-gen-fire-tv-stick-4k-running-vega-os-with-an-all-new-remote/))
- **The GPU.** The G310 is a single Valhall core, configurable at **32–128 GFLOPS FP32 and 2–4 Gpix/s at 1 GHz**. ([Wikipedia: Mali](https://en.wikipedia.org/wiki/Mali_(processor)))
  - Arm claims 6x the texturing and 4.5x the Vulkan performance of the G31. ([Arm](https://www.arm.com/products/silicon-ip-multimedia/gpu/mali-g310))
  - At 500 MHz that comes to **about 16–64 GFLOPS and 1–2 Gpix/s**.
- **The driver.** Mesa Panfrost is GLES 3.1 conformant on Valhall, at least on the G610. Collabora still lists performance work and low-memory behaviour as open items. ([Collabora](https://www.collabora.com/news-and-blog/news-and-events/taming-the-panthor-opengl-es-31-conformance-achived-mali-g610.html))
  - Panfrost has no published benchmarks against Arm's own driver, so we have to measure.

| Internal resolution | Mpix/s at 60 fps | FLOPs/pixel/frame if the whole GPU did one pass (16–64 GFLOPS) | Full-screen layers of fill (1–2 Gpix/s) |
|---|---|---|---|
| 1920×1080 | 124 | **130–510** | 8–16 (theoretical) |
| 1280×720 | 55 | 290–1,160 | 18–36 |
| 960×540 | 31 | 515–2,060 | 32–64 |
| 640×360 | 14 | 1,160–4,630 | 72–145 |
| 480×270 | 7.8 | 2,050–8,200 | 128–256 |

Assume real efficiency is about 50%. A 64-step raymarch with a 20–30 FLOP SDF costs about 2,000 FLOPs per pixel, which means **480×270 to 640×360** for a full-screen raymarched scene and about 540p for a simple one. Native 1080p is for cheap work: geometry, text, composite.

### 2.2 Techniques, why they read as premium, and what they cost here

| Technique | Why it reads as premium | Cost on the G310 and how to stay in budget | Verdict |
|---|---|---|---|
| **Neon vector and glow** (thin lines, additive) | Pure saturated colour on black is the best case for a TV's contrast. Reviewers credit Geometry Wars' look to its neon, its bloom and above all its warping grid. ([Tuts+ grid](https://code.tutsplus.com/make-a-neon-vector-shooter-for-ios-the-warping-grid--gamedev-14637t), [Killscreen](https://www.killscreen.com/geometry-wars-3-dimensions-attempts-improve-perfection)) | Lines cover few pixels. On a tiler, **blending, depth and 4x MSAA happen in on-chip tile memory and are close to free** ([Arm engineer](https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/5114/how-efficient-is-fragment-discarding-for-large-amounts-of-triangle-overlap/16576), [Arm best practices](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/new-developer-guide-arm-mali-application-developer-best-practices)). Avoid 8x MSAA. | **Best fit** |
| **Bloom chain** | It turns flat colour into "light". | Use **dual filtering** (4 passes down, 4 up) from Arm's SIGGRAPH 2015 work, built for mobile bandwidth ([Bjørge notes](https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/siggraph2015_2D00_mmg_2D00_marius_2D00_notes.pdf)). Start from 960×540, so the whole chain touches under 1 Mpix. Bloom alone cost about 3 ms in a shipped mobile game ([Arm/Nordeus](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/post-processing-effects-on-mobile-optimization-and-alternatives)). If float render targets turn out slow, write an emissive mask instead of using HDR. | **Yes**, about 1.5–3 ms (estimate) |
| **GPU particles** (transform feedback in ES 3.0, or compute with SSBOs in ES 3.1) | Particles that react to music and player input are the core of Tetris Effect's emotional pull. ([PC Gamer](https://www.pcgamer.com/japan-week-mizuguchi-on-the-magic-of-tetris-effect)) | Simulation is cheap: 64k particles × about 50 FLOPs is about 0.2 GFLOPS. Fill is the real cost, so keep sprites small (2–6 px), additive and unsorted. Arm's own ES 3.1 compute sample advects "tens of thousands" of particles on Mali ([Arm SDK](https://arm-software.github.io/opengl-es-sdk-for-android/compute_particles.html)). | **Yes**: 32–64k, measure up to 256k |
| **Stylized low-poly with strong lighting** | Clean shapes, gradient skies, fog and a few saturated lights. Alto's dynamic lighting and weather carry its premium feel. ([Wikipedia](https://en.wikipedia.org/wiki/Alto%27s_Adventure), [Design Milk](https://design-milk.com/altos-adventure-keeps-simple/)) | Low vertex counts, forward shading with 1–4 point lights, vertex-colour gradients, fog to hide draw distance, and baked ambient occlusion. Render at 720p and upscale if the shading gets heavy. | **Yes** |
| **SDF or raymarched scenes at reduced resolution** | Smooth, morphing, "impossible" geometry that no asset pipeline could produce. | ALU-bound: **480×270 to 640×360**, fixed step cap, early exit. Raymarching into a half-resolution buffer is the standard advice ([Imagination](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/ray-marching.html)). Upscaling from too low a resolution blurs the image and breaks at depth edges ([Arm ASR](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/introducing-arm-accuracy-super-resolution)), so use it for **backdrops and hero objects** behind sharp 1080p geometry. | **Selectively** |
| **Audio-reactive visuals** | Synesthesia: you see what you hear. | Free for us: the **game drives the synth**, so visuals key off the same note and beat events, with no FFT. Schedule notes about 75 ms ahead to cancel the measured audio latency. | **Yes, and it's our signature** |
| **CRT and retro-futurism** | It hides low internal resolution and reads as deliberate style. | One full-screen pass for curvature, mask, glow and grain, at about 1 ms. Phosphor persistence costs one feedback buffer. | **Yes** (Phosphor concept) |
| **Juice** (hit-stop, screen shake, tweening, flashes) | Most of what makes a simple game "feel expensive". ([GDC: Juice It or Lose It](https://gdcvault.com/play/1016487/)) | CPU only. | **Always** |

### 2.3 Frame budget template (16.6 ms, all estimates until section 6 is run)
| Pass | Resolution | Target |
|---|---|---|
| Scene geometry (vector lines, low-poly, 4x MSAA, resolved on the tile) | 1080p | 4–6 ms |
| Hero effect (SDF backdrop or plasma) | 480×270–640×360 | 1–3 ms |
| GPU particles, 32–64k, additive | 1080p, or 540p into the bloom input | 1.5–3 ms |
| Bloom, dual filter | 960×540 → 60×34 | 1.5–3 ms |
| Composite (tonemap, chromatic aberration, vignette, grain, UI) | 1080p | 1.5–2 ms |
| **Headroom** (thermal, OS compositor) | | **≥ 3 ms** |

Add dynamic resolution on the heavy passes, dropping their scale when GPU time goes over 14 ms.

---

## 3. Reference games: style and genre only

| Game | What makes it feel premium | What we take from it (a principle, not content) |
|---|---|---|
| **Geometry Wars** | Neon on black, bloom, and a spring grid that bends under every bomb. The grid **visualises power**. ([Tuts+](https://code.tutsplus.com/make-a-neon-vector-shooter-for-ios-the-warping-grid--gamedev-14637t), [GamesRadar](https://www.gamesradar.com/au/geometry-wars-retro-evolved-xbox-live-arcade-review/)) | A physical background that reacts to play |
| **Rez / Area X** | Wireframe looks "timeless", and Area X "generate[s] many, many particles and [has] them react to the sound". ([Fandom GDC](https://www.fandom.com/articles/gdc-rez-infinite-impressions), [Sixth Axis](https://www.thesixthaxis.com/2017/08/09/hands-on-rez-infinite-for-pc-out-today-with-4k-and-vr-support/)) | Player actions become the music |
| **Tetris Effect** | Particles matter only because they react to the music and the player, and music is the emotional core. ([PC Gamer](https://www.pcgamer.com/japan-week-mizuguchi-on-the-magic-of-tetris-effect), [WCCFtech](https://wccftech.com/interview-tetsuya-mizuguchi-synesthesia-tetris-effect-rez-lumines/)) | Stage transitions as set pieces |
| **Lumines Arise (2025)** | Critics praised the "synesthetic grooves" and flagged visual clutter as the risk. ([GamesRadar](https://www.gamesradar.com/games/puzzle/lumines-arise-review)) | Skins that change with the song, and clutter control |
| **Thumper** | Stark art direction, speed, simple controls, and visual cues readable at speed. ([MCV](https://mcvuk.com/business-news/developer-drool-on-why-thumper-is-not-a-rhythm-title/), [Game Developer](https://gamedeveloper.com/design/how-i-thumper-i-uses-striking-visuals-to-complement-gameplay-at-every-turn)) | Intensity from a minimal palette and a hard beat |
| **Super Hexagon** | Tiny runs, instant retry, and a rotating world. ([Wikipedia](https://en.wikipedia.org/wiki/Super_Hexagon)) | Session design |
| **Pac-Man CE DX** | Neon maze, 5-minute score attack, speed that rises with score. ([GameSpot](https://www.gamespot.com/reviews/pac-man-championship-edition-dx-review/1900-6284565/)) | D-pad-only arcade can feel modern |
| **Sayonara Wild Hearts** | A "pop album" structure with one direction and one button. ([Simogo](https://www.simogo.com/sayonara-wild-hearts/)) | Remote-first prestige |
| **Alto's Adventure** | A pared-down palette, dynamic light and weather, and calm. ([Design Milk](https://design-milk.com/altos-adventure-keeps-simple/)) | Lighting does the work, not detail |
| **Mini Motorways** | A minimalist, modular look that carries "all of the important information". ([Miketendo64](https://miketendo64.com/2022/08/14/interview-mini-motorways/)) | Readability on a big screen |
| **Jackbox** | Zero setup: phones are the controllers. ([Steam](https://store.steampowered.com/app/331670/Jackbox_Party_Pack/)) | Friction is the enemy of party play |

---

## 4. Concepts

Scope is counted in **agent sessions** to a polished 1.0: **S ≈ 2–3, M ≈ 4–6, L ≈ 8+**. That assumes the shared "neon renderer" (GLES 3.1 context, bloom, particles, composite, synth bus) is built once in the proof (section 7) and reused. Every concept is **100% procedural**: no art assets and no licensing questions, and every pixel and note comes from our code.

### C1. GRAVITY CHOIR: neon arena
**Pitch:** *You're a star that sings. Pull the swarm in, flip your charge, and smash them together into chords.*
- **Core loop:**
  - 3-minute score-attack waves in a neon arena. Your star has a force field.
  - **Attract** pulls enemies toward you. That's risky, but they slam into each other. **Repel** throws them into walls and into each other.
  - Enemies are destroyed only by **colliding with each other or with walls**: there's no shooting.
  - Chains inside one beat window raise the multiplier. A "conductor" mini-boss arrives every 45 s.
  - Three hits end the run, and restart is instant. There's a daily seed.
- **Controls (remote-first):**
  - D-pad moves, with inertia.
  - **OK flips polarity**; hold it for a stronger pulse.
  - **Play/Pause fires the "Crescendo"**, once per wave.
  - Back pauses.
  - There's no aiming, which removes the twin-stick problem on a remote entirely.
  - **Gamepad and local play:** analog movement, and **up to 4-player co-op**. Each player is a different colour and instrument, and their fields interact: two attractors orbit as a binary star.
- **Visual signature:**
  - A **spring-mass warping grid** of about 96×54 nodes, simulated on the CPU with NEON and drawn as anti-aliased glowing lines at native 1080p with 4x MSAA.
  - Thin vector enemies.
  - **64k GPU particles** for debris and the star's corona.
  - A dual-filter bloom from 540p.
  - Chromatic aberration pulsing on the downbeat, and hit-stop on big chains.
  - *Why it fits the G310:* almost no full-screen opaque shading. The cost is thin lines plus small additive sprites, both of which tilers handle well, plus one low-res bloom chain. Estimated GPU time is 8–11 ms.
- **Audio hook:**
  - Each enemy type is a voice (pluck, bell, pad).
  - Collisions are quantised to 1/16 notes in the current key, and the multiplier adds arrangement layers (drums, then bass, then lead).
  - A polarity flip is a filter sweep, and field strength drives the pad's cutoff.
  - In co-op, each player owns an instrument.
- **Wow moment:**
  - The **Crescendo**: the grid funnels into a singularity and 60k particles spiral in.
  - The music drops to **one bar of silence**.
  - On the downbeat a shockwave rings the whole grid like a drumhead.
- **Scope:** **S–M**. Arena, enemies and the grid are small. Polish and balance take most of the time.
- **Risks:**
  - It could read as a "Geometry Wars clone". Polarity, collision-only kills and music-as-score are the differentiators, so there must be no ship and no bullets.
  - Readability through heavy bloom on TVs with motion smoothing.
  - CPU cost of grid physics plus N² collisions on armv7. Use a spatial hash and cap the grid at about 5k nodes.

### C2. CHROMA ORBIT: synesthetic puzzle
**Pitch:** *A falling-block puzzle where the pieces fall inward, onto a star that blazes brighter with every ring you clear.*
- **Core loop:**
  - Arc-shaped, two-colour segments descend from the outer orbit toward a central star.
  - **You rotate the whole planet**, not the piece.
  - A complete ring dissolves. Same-colour runs fuse and chain.
  - Each stage is a song of 3–4 minutes. Clears raise the song's energy, and at the end of a stage the star goes supernova and the next world blooms in.
  - You lose when the pieces reach the outer orbit.
  - There are no tetromino shapes and no sweeping timeline: the rules and geometry are our own.
- **Controls:**
  - Left and Right rotate the world.
  - Down drops fast.
  - OK swaps the piece's colours.
  - Play/Pause holds a piece.
  - **Local:** two planets side by side, remote against gamepad. Clears send debris to the other player.
- **Visual signature:**
  - The **star is a procedural plasma/fbm shader rendered at 480×270**, with limb darkening, upscaled and bloomed. It looks volumetric and costs about 1–2 ms.
  - The rings are glossy extruded segments with a Fresnel rim and a fake reflection of the star.
  - 30–60k particles of solar wind. Cleared rings disintegrate into particles that orbit the star and fall in.
  - Per-world skins, each one shader plus a palette, cross-faded: nebula, aurora ribbons, ocean caustics, night city.
  - God rays from the star (a radial blur at quarter resolution).
- **Audio hook:**
  - Ring depth maps to stems.
  - Each clear is a chord stab, and chained clears arpeggiate.
  - The star's brightness is the master filter.
  - A danger state brings in a detuned drone.
- **Wow moment:** the **supernova transition**. The star swells and swallows the rings, the screen goes to white on the drop, and a new world unfurls in a new key. It's a stage-change set piece with zero art assets.
- **Scope:** **M**, including 5 world skins.
- **Risks:**
  - The puzzle has to be genuinely good. Prototype the rules on the host first.
  - Clutter. Lumines Arise was criticised for it, so particles should dim in danger states.
  - Distance from Tetris: the radial-inward rule, the rotating world and arc pieces keep it distinct.

### C3. PRISM SPIRE: pass-the-remote party stacker
**Pitch:** *Build a tower of glass one piece at a time, passing one remote around the couch. Whoever topples it buys the pizza.*
- **Core loop:**
  - Turn-based, for 2–8 players. Each turn you get a random glass prism and place it.
  - Physics settles the tower, which sways. Events add wind, tremors and "slippery glass".
  - Rounds take 3–6 minutes.
  - **Solo:** a height challenge with a daily seed, which gives a reason to come back that party games usually lack.
- **Controls:**
  - Left and Right move the piece, Up and Down rotate it, OK drops it.
  - **One remote for the whole party.** No phones, no gamepads, no setup.
- **Visual signature:**
  - Glass done cheaply: **screen-space refraction** that distorts a half-res copy of the background, a dispersion fringe (an RGB offset), a Fresnel rim, and caustic patterns projected on the floor.
  - A dusk gradient sky with stylized low-poly hills.
  - Instanced shards and sparkle particles when things break.
  - 2D rigid-body physics (Box2D, MIT) driving 3D rendering.
- **Audio hook:**
  - Each placed piece rings a bell tuned to its height, so the tower climbs a scale.
  - A wobble is glass-harmonica tremolo.
  - A collapse is **a cascade of chimes driven by the real shard collisions**.
- **Wow moment:** the **slow-motion collapse**. About 200 refracting shards hang in sunset light, each impact plays a chime, and a 3-angle replay follows.
- **Scope:** **M**.
- **Risks:**
  - Party games are rarely replayed (Naavik), which the solo daily mode mitigates.
  - Physics tuning so the game is fair.
  - Refraction looking cheap if it's under-done. Prove it on the device early.

### C4. STARWIRE: generative rhythm runner
**Pitch:** *Ride a spark down an endless wire through a cathedral of light. The song is composed as you play, and your hits play its melody.*
- **Core loop:**
  - Three lanes rush toward the camera. You strike notes, switch lanes on the beat and leap gaps.
  - Songs are **generated from a seed** (tempo, key, progression), so there are infinite tracks, plus a shared "song of the day".
  - Misses strip layers out of the song. Perfect strings open a "chorus" section.
- **Controls:** OK strikes, Left and Right switch lanes, Up jumps. That's ideal for the remote, and a gamepad uses the same layout.
- **Visual signature:**
  - Instanced procedural arches and pillars, fog, and a **planar-reflection floor** rendered at 1/4 resolution.
  - A **raymarched "sky cathedral" backdrop** (SDF domain repetition) at 480×270.
  - A radial speed-streak pass at quarter resolution.
  - An FOV kick and chromatic aberration on the chorus.
- **Audio hook:** the generator's note events *are* the level, and player hits trigger the lead synth.
- **Wow moment:** the chorus. The wire splits into 8 strands, the cathedral dissolves into a starfield at 1.5x speed, and the key changes.
- **Scope:** **M–L**.
- **Risks:**
  - **Latency.** Audio is about 75 ms. Bluetooth remote input latency hasn't been measured. The game needs a calibration screen and generous timing windows.
  - The quality of the procedural music.
  - The genre is crowded.

### C5. PHOSPHOR: you are the electron beam
**Pitch:** *Draw light on a vector monitor to trap the glitches living in the glass.*
- **Core loop:**
  - Territory capture on a simulated vector CRT. You ride the lit edges, and **hold OK to burn a new line** into the dark.
  - Closing a shape fills it with glowing phosphor and traps the glitches inside it.
  - Capture 75% to clear a screen.
  - Each level is a different "monitor": green oscilloscope, amber or colour vector.
- **Controls:** D-pad and OK, which is ideal for a remote. **Co-op:** a second beam on a gamepad.
- **Visual signature:**
  - Beams drawn with a Gaussian profile and an intensity that depends on how long the beam dwells.
  - **Phosphor persistence via a feedback buffer** (previous frame × decay).
  - A CRT glass pass: curvature, reflection, halo and jitter.
  - At level clear, a **3D pull-back** shows a dark room lit by the monitor's glow.
- **Audio hook (unique):**
  - The beam's X and Y drive an oscillator, like an oscilloscope instrument.
  - **Closing a shape plays that shape traced as a waveform**, so what you draw is what you hear.
- **Wow moment:** the camera pulls out through the glass to a wall of 12 monitors, each replaying a level you've cleared.
- **Scope:** **S**.
- **Risks:**
  - The genre's appeal is niche and retro.
  - The flash is subtler than the neon concepts.
  - The audio can turn harsh if it isn't tamed.

### C6. SPARK SUMO: 2–4 player party brawler
**Pitch:** *Glowing pucks on a crumbling disc high above a neon city. Last one on the plate wins.*
- **Core loop:**
  - 60–90 s rounds, best of 5.
  - Hold to charge a dash and release to fire it.
  - The arena shrinks and tilts. Pickups: heavy, magnet, ghost.
  - Bots fill the empty seats.
- **Controls:** the remote is player 1 (D-pad, plus OK to charge). Gamepads are players 2–4. Phone controllers are a stretch goal.
- **Visual signature:**
  - A low-poly arena. **Each puck is a moving point light**, with 4 forward lights and rim-lit pucks.
  - Emissive trails.
  - A city below rendered at half resolution with fog and bloom.
  - A **slow-motion knockout replay** with a low-res depth-of-field blur.
- **Audio hook:**
  - Each puck hums one note of a chord.
  - Collisions are percussion.
  - The tempo rises as the arena shrinks.
- **Wow moment:** a final hit at 10% speed. The camera orbits, sparks hang in the air, the music cuts to a single reverb tail, and the city lights flare in the winner's colour.
- **Scope:** **M**.
- **Risks:**
  - It needs a second controller, and most Fire TV homes don't have one, so the bots must be good.
  - The phone path depends on unproven networking.

### C7. TIDELIGHT: chill bioluminescent garden
**Pitch:** *Grow a glowing reef by moonlight. Every creature you place joins the song.*
- **Core loop:**
  - A no-fail builder on a hex grid. Corals, anemones and fish schools score "harmony" from their neighbours.
  - One tide cycle lasts 4 minutes, with optional goals.
- **Controls:** D-pad cursor and OK to place. Remote only.
- **Visual signature:**
  - Low-poly seabed.
  - Caustics from two scrolling textures.
  - God rays at quarter resolution.
  - Depth fog.
  - Instanced boids (500–2,000 fish, simulated on the CPU).
  - 30k plankton particles that glow at night, plus bloom.
- **Audio hook:** generative ambient. Each species is an instrument with a pattern, neighbours set the harmony, and tides change the chords.
- **Wow moment:** nightfall. The reef lights up section by section in time with the music, and plankton swirl around the cursor.
- **Scope:** **M–L**, mostly balancing.
- **Risks:**
  - Less flash, so a weaker video.
  - Builder balance takes time.

### C8. MOLTEN MARBLE: SDF tilt-maze
**Pitch:** *Tilt a world of liquid chrome to roll a marble home. The level melts and re-forms to the beat.*
- **Core loop:**
  - Levels are SDFs built from smooth unions of shapes, and they **morph on the beat**: bridges form and dissolve.
  - 60-second time attacks.
- **Controls:** the D-pad tilts the world, eased into an analog tilt. OK jumps or brakes. A gamepad stick also works.
- **Visual signature:**
  - **Raymarched at 640×360**: a 48-step sphere trace, 5-tap SDF ambient occlusion, and environment-mapped chrome.
  - Upscaled with sharpening and bloom.
  - **The same SDF runs on the CPU for collision**, which makes a great case-study line.
- **Audio hook:** tilt maps to the filter, morphs land on the beat, and rolling speed drives a noise voice.
- **Wow moment:** between levels, the world liquefies into one metaball blob and re-forms as the next level.
- **Scope:** **L**.
- **Risks:**
  - The highest GPU risk.
  - 360p could look soft on a 4K TV.
  - Digital tilt from a D-pad is imprecise.

---

## 5. Ranking and recommendation

Each criterion is scored 1–5. For "Scope/risk", 5 means easiest.

| # | Concept | Wow / flash | Remote fit | Market evidence | Scope/risk | Case-study value | **Total** |
|---|---|---|---|---|---|---|---|
| 1 | **Gravity Choir** | 5 | 4 | 4 | 4 | 5 | **22** |
| 2 | **Chroma Orbit** | 4 | 5 | 5 | 4 | 4 | **22** |
| 3 | **Prism Spire** | 4 | 5 | 5 | 3 | 4 | **21** |
| 4 | Phosphor | 3 | 5 | 2 | 5 | 4 | 19 |
| 5 | Starwire | 5 | 4 | 3 | 2 | 4 | 18 |
| 6 | Tidelight | 3 | 5 | 3 | 3 | 3 | 17 |
| 7 | Spark Sumo | 4 | 2 | 4 | 3 | 3 | 16 |
| 8 | Molten Marble | 5 | 3 | 2 | 1 | 5 | 16 |

**1. Gravity Choir (build first).** Gravity Choir wins the tie with Chroma Orbit because the user puts flash first, and its renderer (MSAA vector lines, GPU particles, bloom, composite, synth events) is the foundation every other neon concept reuses.
- It has the highest visual ceiling for the smallest scope.
- Its key mechanic (polarity, no aiming) is a **design answer to the remote**, which is a good case-study beat in its own right.
- **Evidence:**
  - Arcade score-attack is the native genre of TV stores (Naavik).
  - Short, restartable runs match measured session lengths (GameAnalytics, Pac-Man CE DX).
  - The neon, bloom and reactive-grid style is proven (Geometry Wars).
  - Particles reacting to music and play are what make such visuals *mean* something (Mizuguchi).
  - Optional 4-player co-op taps a stable, age-independent appetite (Quantic Foundry, MIDiA).

**2. Chroma Orbit (second, reusing the renderer).**
- It has the best remote fit and the broadest audience.
- **Evidence:**
  - Netflix put a Tetris title in its five-game TV launch.
  - 28% of TV gamers play casual games daily (Vorhaus).
  - Synesthetic puzzle games keep winning critics (Lumines Arise, 2025).
- Its stage-change "supernova" makes a stronger *second* video beat than a second neon shooter would.

**3. Prism Spire (the social pick).** Party is where every TV platform is investing: Netflix, Luna GameNight, Samsung's +90%, LG's Couch Play. Prism Spire gets the party benefit **without phone-controller friction** (Netflix's controller app has about 1.5M MAU) and without gamepads, which Fire TV homes mostly lack. A daily solo seed offsets the "party games aren't replayed" problem.

**Honourable mention: Phosphor.** It's the smallest scope, and "the shape you draw is the waveform you hear" is the most original idea in the set. It's a good fast follow-up or a jam-style side project.

---

## 6. Run this first: a half-day on-device GPU benchmark (`apps/gpu-bench`)

We have no fill-rate or shader-cost numbers, so measure before committing. Build a native SDL3 app with a raw GLES 3.1 context. Our Vega SDL driver already implements `SDL_GL_CreateContext` with an ES 3.x context and `GL_GetProcAddress`.

It writes a CSV to `/data` and a summary to the log, and pulls results with `copy-from`. It measures:
1. **Capabilities dump:**
   - all 126 extensions, saved to the repo docs this time
   - in particular `EXT_color_buffer_half_float`, `EXT_color_buffer_float`, `EXT_disjoint_timer_query`, `EXT_multisampled_render_to_texture`, `EXT_shader_framebuffer_fetch`, `KHR_texture_compression_astc_ldr`
   - `GL_MAX_COMPUTE_WORK_GROUP_*` and `GL_MAX_SAMPLES`
2. **Fill rate:**
   - N full-screen quads, opaque against additive, at RGBA8, RGB10_A2 and RGBA16F
   - result: Gpix/s, and the largest N that holds 60 fps at 1080p
3. **ALU:** a full-screen shader with K dependent FMAs or transcendentals at 1080p, 720p, 540p and 360p, giving effective GFLOPS (FP32 and `mediump`).
4. **Texture:** N bilinear taps, mipmapped against not, giving Gtex/s.
5. **Render-target round trips:** the cost of a full-screen RTT write and read at 1080p in RGBA8 and RGBA16F, and of 4x MSAA via `glBlitFramebuffer` resolve compared with `EXT_multisampled_render_to_texture`.
6. **Bloom:** a dual-filter chain against a downscaled separable Gaussian, starting from 1080p and from 540p.
7. **Particles:** 16k, 64k and 256k particles, three ways: transform feedback, compute plus SSBO, and a CPU NEON stream into a VBO. Point sprites against instanced quads, with 2, 4 and 8 px sprites.
8. **Raymarch reference:** a fixed 64-step SDF scene at 320×180, 480×270, 640×360 and 960×540.
9. **Lines:** 20k anti-aliased line quads at 4x MSAA, the Gravity Choir grid case.
10. **Timing method:**
    - `EXT_disjoint_timer_query` if it's present.
    - Otherwise use `glFinish()` around each pass and take the CPU median of 120 frames.
    - Also record "max workload at a steady 60 fps".
11. **Thermal soak:** 10 minutes at about 80% load, logging frame time every 10 s. The stick is fanless, so we need to know whether it throttles.
12. **Side checks** for later concepts:
    - **Remote input latency.** Use a light sensor if the user has one; otherwise timestamp injected keys against present.
    - **LAN socket:** can a native app `bind()` and `listen()` on the LAN? This gates phone controllers. Test it on the device, and **don't use any network service the user hasn't approved**.

**Deliverable:** a `docs/gpu-budget.md` table, which replaces the estimates in sections 2.1 and 2.3.

---

## 7. A 1-day graphics proof for Gravity Choir ("Neon Proof")

**Goal:** prove on the TV that the look holds a locked 60 fps at 1080p output, and capture case-study stills.

| Hours | Build |
|---|---|
| 0–1.5 | App skeleton:<br>- `apps/neon-proof` via `vega_add_sdl3_app`, with an SDL GL context (ES 3.1) and functions resolved with `SDL_GL_GetProcAddress`<br>- a tiny GL helper: shader compile, FBO, fullscreen triangle<br>- an on-screen perf HUD showing per-pass ms, from section 6's timing method |
| 1.5–3 | **Warping grid:**<br>- a 96×54 spring-mass grid on the CPU, pulled by a gravity well that orbits on a scripted path<br>- drawn as 1080p anti-aliased line quads (with a soft falloff in the fragment shader) into a 4x MSAA target |
| 3–4.5 | **GPU particles:**<br>- 64k particles via compute (with a transform-feedback fallback): spiral-in, burst and drag<br>- additive, 2–5 px sprites |
| 4.5–6 | **Post chain:**<br>- emissive/HDR target (RGBA16F if it's renderable, otherwise RGBA8 plus an emissive mask)<br>- dual-filter bloom (4 down, 4 up from 540p)<br>- composite: filmic tonemap, vignette, beat-pulsed chromatic aberration, film grain |
| 6–7 | **Synth hookup:**<br>- reuse the arcade synth with a beat clock<br>- every particle burst and grid impulse is a quantised note, scheduled about 75 ms ahead so sound and light land together |
| 7–8 | **Scripted 20-second "Crescendo" sequence** (singularity, one bar of silence, shockwave):<br>- `tools/vega.sh shot` stills at 3 timestamps<br>- logs of p50/p99 frame time, RSS and per-pass GPU ms<br>- a 10-minute soak |

**Pass criteria:**
- p99 frame time ≤ 16.7 ms with 64k particles and bloom on.
- At least 3 ms of headroom.
- RSS under 120 MB.
- No throttling over 10 minutes.
- Stills that look good on the TV, not just on the Mac.

**Fallbacks if it misses:**
- particles down to 32k, or rendered into the 540p bloom input
- grid line count down
- bloom starting from 1/4 resolution
- RGBA8 with an emissive mask

**Video note:** `gwsi-tool-screenshooter` captures stills only. A motion clip for the case study needs an HDMI capture card on the user's side, or a host-preview render of the same code.

---

## 8. Risks shared by all concepts
- **Panfrost performance is unknown** on G310 at 500 MHz. Section 6 de-risks it.
- **Thermal throttling** on a fanless stick: soak-test it.
- **Audio latency of about 75 ms** is fine for arcade and puzzle games, and needs calibration for rhythm games (Starwire).
- **TV motion smoothing and overscan** can smear neon lines. Keep UI 5% inside the safe area, and consider a "Game mode" hint on first run.
- **The 420 MB memory cap** isn't a constraint here: every concept fits in under 150 MB. Several of our apps resident at once *has* caused memory pressure before, though, so launch the game from the arcade and free its resources when it goes to the background.
- **Originality:** keep mechanics and names our own, and do a trademark check before announcing.
- **Phone controllers:** whether a native app may listen on the LAN is unverified on Vega, so it gates any phone-controller feature.

---

## Sources
- **Netflix:**
  - [TechCrunch: Netflix TV games launch (Oct 2025)](https://techcrunch.com/2025/10/09/netflix-is-letting-users-play-games-on-their-tv-screens)
  - [GamesHub: Netflix four-pillar strategy](https://www.gameshub.com/?p=2690254)
  - [PocketGamer.biz: Netflix cloud-first TV games (Jan 2026)](https://www.pocketgamer.biz/netflix-unveils-new-cloud-first-direction-for-its-games-strategy/)
  - [StreamTV Insider: Netflix party games uptake](https://www.streamtvinsider.com/content/netflix-tv-party-games-see-strong-uptake-initial-rollout)
  - [Spokesman-Review: Netflix games strategy revamp](https://www.spokesman.com/stories/2025/nov/14/netflix-is-revamping-its-gaming-strategy-to-win-mo)
- **Amazon:**
  - [Engadget: Luna controller-free party games](https://www.engadget.com/gaming/amazon-luna-will-offer-controller-free-party-games-in-an-attempt-to-woo-prime-subscribers-130004416.html)
  - [VGC: Luna GameNight](https://www.videogameschronicle.com/news/amazons-redesigned-luma-launches-with-gamenight-its-party-library-with-a-ai-snoop-dogg-courtroom-game)
  - [Advanced Television: Luna in Prime Video (Jul 2026)](https://www.advanced-television.com/2026/07/24/amazon-adds-luna-games-to-prime-video/)
  - [AFTVnews: Fire TV Games Hub](https://www.aftvnews.com/?p=40199)
  - [AFTVnews: 3rd-gen Fire TV Stick 4K](https://www.aftvnews.com/amazon-announces-new-3rd-gen-fire-tv-stick-4k-running-vega-os-with-an-all-new-remote/)
  - [Vega docs: NetInfo privilege](https://developer.amazon.com/docs/vega-api/2.0/react-native-net-info)
- **Other TV platforms and market data:**
  - [PPC Land: Samsung casual/party +90%](https://ppc.land/samsung-tvs-gain-over-90-more-casual-and-party-gamers-in-north-america/)
  - [BriefGlance: LG + Miniclip](https://briefglance.com/articles/the-big-screen-pivot-why-lg-and-miniclip-are-betting-the-living-room-on-casual-gaming)
  - [Naavik: The Reality of Smart TV Gaming (Oct 2025)](https://naavik.co/digest/the-promise-and-perils-of-smart-tv-gaming/)
  - [Statista/Vorhaus: casual games via TV frequency](https://statista.com/statistics/1476054/casual-games-via-tv-usa-frequency-age-group)
  - [Statista/Vorhaus: interest in casual CTV games](https://www-statista-com.ezproxy.canberra.edu.au/statistics/1476068/interest-in-playing-casual-games-via-connected-tv-usa-age-group)
  - [GameAnalytics 2025 benchmarks](https://gameindustrylibrary.com/documents/gameanalytics-mobile-gaming-benchmarks-2025)
  - [Statista/MIDiA: couch co-op preference](https://www.statista.com/statistics/1607985/preference-of-couch-co-up-gaming-by-age)
  - [Game Developer: Quantic Foundry co-op survey](https://gamedeveloper.com/design/survey-co-op-more-popular-than-competitive-multiplayer-especially-if-it-s-local)
  - [Gamereactor: Mario Party Jamboree 7.48M](https://www.gamereactor.eu/nintendo-made-24-switch-titles-million-sellers-last-fiscal-year-1539743/)
  - [Steam: Jackbox Party Pack](https://store.steampowered.com/app/331670/Jackbox_Party_Pack/)
  - [DualShockers: Jackbox 2020](https://www.dualshockers.com/jackbox-games-200-million-players-bloomberg-report/)
- **Apple:**
  - [MacRumors: Siri Remote requirement dropped (2016)](https://macrumors.com/2016/06/14/apple-drops-siri-remote-game-controller-demand)
  - [Apple Developer: Behind the Design, Sayonara Wild Hearts](https://developer.apple.com/news/?id=33kvkagk)
  - [Simogo: Sayonara Wild Hearts](https://www.simogo.com/sayonara-wild-hearts/)
  - [MacRumors: Apple Best of 2019](https://www.macrumors.com/2019/12/03/apple-best-of-2019-awards/)
- **Hardware and drivers:**
  - [CNX Software: Fire TV Stick 4K Select specs](https://www.cnx-software.com/2025/10/03/linux-based-vega-os-replaces-android-based-fire-os-in-amazon-fire-tv-stick-4k-select/)
  - [Wikipedia: Mali GPU table](https://en.wikipedia.org/wiki/Mali_(processor))
  - [Arm: Mali-G310](https://www.arm.com/products/silicon-ip-multimedia/gpu/mali-g310)
  - [Collabora: Panfrost/Panthor GLES 3.1 conformance](https://www.collabora.com/news-and-blog/news-and-events/taming-the-panthor-opengl-es-31-conformance-achived-mali-g610.html)
- **Graphics techniques:**
  - [Arm: SIGGRAPH 2015 bandwidth-efficient rendering (dual filter)](https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/siggraph2015_2D00_mmg_2D00_marius_2D00_notes.pdf)
  - [Arm: Post-processing on mobile](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/post-processing-effects-on-mobile-optimization-and-alternatives)
  - [Arm forum: MSAA and blending on tilers](https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/5114/how-efficient-is-fragment-discarding-for-large-amounts-of-triangle-overlap/16576)
  - [Arm: Mali best practices guide](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/new-developer-guide-arm-mali-application-developer-best-practices)
  - [Arm SDK: compute particles](https://arm-software.github.io/opengl-es-sdk-for-android/compute_particles.html)
  - [Imagination: ray marching guidance](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/ray-marching.html)
  - [Arm: Accuracy Super Resolution](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/introducing-arm-accuracy-super-resolution)
  - [GDC Vault: Juice It or Lose It](https://gdcvault.com/play/1016487/)
- **Reference games:**
  - [Tuts+: warping grid](https://code.tutsplus.com/make-a-neon-vector-shooter-for-ios-the-warping-grid--gamedev-14637t)
  - [Killscreen: Geometry Wars 3](https://www.killscreen.com/geometry-wars-3-dimensions-attempts-improve-perfection)
  - [GamesRadar: Geometry Wars review](https://www.gamesradar.com/au/geometry-wars-retro-evolved-xbox-live-arcade-review/)
  - [PC Gamer: Mizuguchi on Tetris Effect](https://www.pcgamer.com/japan-week-mizuguchi-on-the-magic-of-tetris-effect)
  - [WCCFtech: Mizuguchi interview](https://wccftech.com/interview-tetsuya-mizuguchi-synesthesia-tetris-effect-rez-lumines/)
  - [Sixth Axis: Rez Infinite hands-on](https://www.thesixthaxis.com/2017/08/09/hands-on-rez-infinite-for-pc-out-today-with-4k-and-vr-support/)
  - [Fandom: Rez Infinite GDC impressions](https://www.fandom.com/articles/gdc-rez-infinite-impressions)
  - [GamesRadar: Lumines Arise review](https://www.gamesradar.com/games/puzzle/lumines-arise-review)
  - [MCV: Drool on Thumper](https://mcvuk.com/business-news/developer-drool-on-why-thumper-is-not-a-rhythm-title/)
  - [Game Developer: Thumper visuals](https://gamedeveloper.com/design/how-i-thumper-i-uses-striking-visuals-to-complement-gameplay-at-every-turn)
  - [Wikipedia: Super Hexagon](https://en.wikipedia.org/wiki/Super_Hexagon)
  - [GameSpot: Pac-Man CE DX](https://www.gamespot.com/reviews/pac-man-championship-edition-dx-review/1900-6284565/)
  - [Wikipedia: Alto's Adventure](https://en.wikipedia.org/wiki/Alto%27s_Adventure)
  - [Design Milk: Alto's Adventure](https://design-milk.com/altos-adventure-keeps-simple/)
  - [Miketendo64: Mini Motorways interview](https://miketendo64.com/2022/08/14/interview-mini-motorways/)

**Caveats:**
- The G310 throughput figures are spec-derived ranges, not measurements.
- Several market figures are company-reported (Samsung, LG, Netflix).
- The Netflix "~10% reach" figure comes from earnings coverage I could only read in search summaries (the StreamTV Insider page returned 403).