# Submitting pure-native Vega apps to the Amazon Appstore

Research date: 2026-11-15. SDK on this Mac: 2.0.13986. Read-only research; nothing was built,
installed or submitted. Sources are cited inline:
- `KB:` means an Amazon Devices Builder Tools MCP document.
- A URL means a developer.amazon.com page, read through the same MCP.
- `community:` means an Amazon Developer Community thread answered by Amazon staff. Treat these
  as weaker than the docs.

"Not found in docs" means the MCP search did not turn the fact up. It does not mean the rule
doesn't exist.

## Summary

- **Native apps are not ruled out, and not explicitly supported for the store either.** The
  submission docs ask for a VPKG and the `com.amazon.category.main` category, nothing about the
  app's framework. No doc says a native (non-React-Native) app may or may not be published.
- **All eight apps already meet the hard technical gates.**
  - They declare `com.amazon.category.main` and `[os.version]`.
  - Their package IDs are valid.
  - Their icons are 512×512.
  - Release VPKGs exist and pass `vpt validate`.
- **The real work is test criteria, assets and licensing:**
  - **Audio focus:** vegaplat never listens for focus changes.
  - **Remote play:** the FPS ports need D-pad and OK pressed at once.
  - **Media keys:** several games use them for gameplay.
  - **Fire TV assets:** a 1280×720 icon and 1920×1080 screenshots.
  - **Licences:** GPL notices are missing from the packages, and Land Grab's music is CC BY-NC.
  - **Trademarks:** "Quake", "N64", and upstream project names.
- **Vega devices don't accept paid apps.** All our apps would be free, which also suits the GPL
  and non-commercial assets.
- **The arcade is the riskiest submission.** Cross-package `pkg://` launches may be blocked once
  messaging security is enforced, as it may be off on a developer-mode stick. The arcade is also
  useless with no games installed.
- **Recommended first candidate: Gravity Choir.** It is original MIT code with an OFL font and
  synthesized audio, and is 1.3 MB. It runs at a steady 60 fps on the TV and was designed for the
  remote.

## 1. Does the Appstore accept native Vega apps?

What the docs say:
- Vega apps are submitted "through the Developer Console using the same submission process as you
  do for Fire OS apps" (https://developer.amazon.com/docs/vega/latest/app-submission.html).
- That page lists two prerequisites:
  - a VPKG file;
  - the `main` category (`com.amazon.category.main`) in the manifest. Without it, "your app can't
    launch from the Fire TV Home launcher and will fail the Amazon Appstore submission process".
- The general page repeats both requirements and names no framework
  (https://developer.amazon.com/docs/app-submission/submitting-apps-to-amazon-appstore.html).

What is **not found in docs:** any statement that the Appstore accepts, rejects or treats
differently a VPKG whose only component is a native `IInteractiveComponent` with no
`runtime-module`.

Context that points both ways:
- The Vega 2.0 release notes present native APIs as a way to add C++ views and tasks *to a React
  Native app*. They call native tooling "open beta" and "early access" (community: "Vega 2.0
  Launching", https://community.amazondeveloper.com/t/vega-2-0-launching/29849).
- The same notes say `vega project doctor` runs compatibility lint "for projects with
  `kepler.projectType` set to `native`". So native projects are a recognised project type.
- There is a public "Vega Native API reference" for 2.0, including `IInteractiveComponent` under
  `com.amazon.kepler.application`
  (https://developer.amazon.com/docs/vega-native-api/2.0/interfaces.IInteractiveComponent.com_amazon_kepler_application).

Practical answer: nothing in the docs blocks us. A Live App Test (section 2.9) is the cheapest way
to learn whether ingestion accepts a native VPKG before a full review. If it is rejected, ask
Amazon developer support.

## 2. Package and submission mechanics

### 2.1 Account and console flow
- You need an Amazon developer account (free) and identity verification
  (https://developer.amazon.com/docs/app-submission/understanding-submission.html).
- The console flow (https://developer.amazon.com/docs/vega/latest/app-submission.html):
  1. **Add a New App.**
  2. **Upload Your App File:** upload the VPKG.
  3. **Target Your App:** Edit supported devices → the **Amazon Fire TV** tab → the **Amazon Vega
     TV** section.
  4. **Appstore Details.**
  5. **Review & Submit.**
- The Vega page also says: "Do not mention 'Vega' in any field."
- Approval emails go only to the account owner's address
  (https://developer.amazon.com/docs/app-submission/review-submit.html). If an app stays "In
  review" for six days, you may use Contact Us.

### 2.2 Release vs debug builds
- **Upload the release VPKG.** The docs give the release path as the one "to upload for app
  submission" (https://developer.amazon.com/docs/vega/latest/app-submission.html).
- Measure KPIs on the release variant
  (https://developer.amazon.com/docs/vega/0.24/measure-app-kpis).
- An Amazon staff reply says that for Live App Testing "debug vpkg will not work and release vpkg
  has to be used" (community: "Using @amazon-devices/keplerscript-appstore-iap-lib 2.12.13 causes
  in-app purchases to fail").
- Where our builds are:
  - Our builds put the VPKG at `apps/<app>/build/armv7-release/local/packages/<app>.vpkg`. That is
    not the documented `build/armv7-release/<project>_armv7.vpkg`, because the documented layout
    is the React Native one.
  - `tools/vega.sh build <app> armv7 Release` produces it.
  - All eight apps have one, built on 2026-11-14 or 2026-11-15.
- `vega build -b Release` is also the CLI default (`vega build --help`).

### 2.3 Signing
- **What the docs say:**
  - SDK 2.0's `vega build` signs by default with a "developer signing profile" (`--profile`).
    `--no-profile` "builds without developer signing (degraded, not sideload-ready)" (`vega build
    --help`).
  - Our release VPKGs already contain `meta-info/signature/digest.sig` and `digest.json.sig`
    (`vega exec vpt show-contents`).
  - The general Appstore docs say that when you submit, "Amazon removes the signature you used to
    sign your app and re-signs it with an Amazon signature"
    (https://developer.amazon.com/docs/app-submission/understanding-submission.html, "Amazon app
    signatures").
  - A Live TV workflow warns that a self-made debug key "is NOT suitable for Amazon Appstore
    submission" and should be replaced "with a production key". It shows a manual
    `vpt pull` / `vpt sign` / `vpt push` flow (KB: `vega_linear_tv_setup_workflow.md`, Step 4).
  - An Amazon staff reply says re-signing a VPKG with a different certificate breaks Appstore
    recognition for IAP (community: "IAP DevTest returns receipt id null").
- **Not found in docs:** how to get a "production" VPKG signing key, or whether the developer
  signing profile is acceptable for upload.
- **Recommendation:** upload the release VPKG exactly as `vega build` signs it, and don't re-sign
  it by hand. Use a Live App Test to confirm the console accepts it. If the console asks for a
  different signature, that is the time to ask support.

### 2.4 Package ID
- **Rules:**
  - Reverse-domain form, lowercase letters, digits and dots only, at most 128 characters, and
    globally unique in the Appstore (KB: `vega_app_manifest.md`).
  - The ID "must be unique, and it shouldn't include `amazon`" (Upload Your App File,
    https://developer.amazon.com/docs/app-submission/upload-app-file.html).
  - A package name already used by another developer, including in a Live App Test, fails
    upload. Amazon support has to free it (same page).
- **Ours:** `com.firetvnative.gravitychoir`, `.arcade`, `.doom`, `.quake`, `.supertux`,
  `.supertuxkart`, `.n64.brewvolley`, `.n64.landgrab`. All comply.
- **Low-risk note:** "firetv" is part of an Amazon product name. The docs only send you to
  Amazon's Trademark, Brand, and Marketing Guidelines for *titles* that mention Amazon products
  (Appstore Details, "Display title",
  https://developer.amazon.com/docs/app-submission/appstore-details.html). Not found in docs:
  whether a package ID is customer-visible or covered by those guidelines. Keep "Fire TV" out of
  display titles and descriptions unless you follow those guidelines.

### 2.5 Versioning
- Two values: a free-form version string and an increasing build number (1 to 2^63−1). "When you
  update your app, both the version and build numbers must be greater than the previous version
  you submitted" (https://developer.amazon.com/docs/vega/latest/app-version.html; KB:
  `vega_app_versioning_api.md`).
- **Every uploaded binary needs a build number that no earlier upload used**
  (https://developer.amazon.com/docs/app-submission/upload-app-file.html). The Appstore uses the
  build number to block downgrades (https://developer.amazon.com/docs/vega/latest/app-submission.html).
- **Ours:**
  - Our manifests set `version` and `build-number` in `[package]`. `vega exec vpt info --json`
    confirms the VPKG carries them (Gravity Choir: `"version": "1.0.0", "build_number": 1`).
  - `vega build` also accepts `--build-version` and `--build-number`.
  - Bump `build-number` for every upload, including Live App Test uploads (inference from the
    "any previous binary" rule).
  - Use 1.0.0 as the public version for a first release.

### 2.6 Manifest fields that matter for submission
- **`com.amazon.category.main` (required).** Exactly one interactive component may have it
  (https://developer.amazon.com/docs/vega/latest/app-submission.html; components snippet via
  search, https://developer.amazon.com/docs/vega/2.0/manifest-components).
  - `com.amazon.category.launcher` is "reserved for system applications, and would be an error
    for any other". Don't use it for the arcade.
  - All eight of our manifests have `main` and no other category.
- **`[os.version]` (required since SDK 0.24).**
  - The Appstore "blocks install and submission on devices older than" `min` (KB:
    `vega_app_manifest.md`).
  - The 2.0 release notes say OS 2.0 "has rolled out to all Vega-powered Fire TV devices" and
    that "for most apps, set both `min` and `target` to `2.0`" (community: Vega 2.0 Launching).
  - Ours: `min = "2.0"`, `target = "2.0"`. Correct.
- **`[needs]`.** The console disables any device that lacks a `needs` module. A community bug
  report shows the Fire TV Stick HD auto-disabled at submission because of
  `/com.amazon.kepler.vector_icons_2@IVectorIcons_1`, and then the submission was rejected with
  "Developer Intent is not set on the following device" (community: "Fire TV Stick HD
  auto-disabled at Appstore submission…").
  - Our only `needs` entry is the OS module `/com.amazon.vega.os@IVega_2_0`, so all three Vega
    sticks should stay selectable. Check the Target screen anyway.
  - SDK 2.0 generates the OS module entries from `[os.version]` and says not to hand-write them
    (community: Vega 2.0 Launching, "Breaking changes"). Our source manifests still contain it.
    That is harmless (the packaged manifest is identical), but `vega project update-manifest`
    would remove it.
- **`[wants]`.**
  - We want `com.amazon.audio.stream` and `com.amazon.audio.control`.
  - The audio-focus doc names `com.amazon.audio.control` as required for focus APIs (KB:
    `vega_audio_focus_api.md`).
  - Not found in docs: any `wants` entry that blocks certification.
- **`[[needs.privilege]] com.amazon.system.audio`.** This appears as an audio-playback example in
  KB: `vega_app_manifest.md`. Our apps play audio without it, so it isn't needed in practice.
  Not found in docs: whether review checks for it.
- **Icon.** The manifest icon is "PNG format, 512x512 pixels" (KB: `vega_app_manifest.md`).

### 2.7 Architectures
- **Upload armv7 only.**
  - The release example for submission is `build/armv7-release/<project>_armv7.vpkg`
    (https://developer.amazon.com/docs/vega/latest/app-submission.html).
  - The build docs label armv7 "to run your app on the Fire TV Stick", aarch64 for "the virtual
    device for M-series machines" and x86_64 for the virtual device on x86
    (https://developer.amazon.com/docs/vega/0.24/build-an-app).
  - Our sticks are armv7 userspace (CLAUDE.md).
- The 64-bit rules on the "Understanding submission" page are about Fire OS APK native-library
  folders. Not found in docs: any Vega aarch64 requirement.
- Two binaries in one listing can't target the same device
  (https://developer.amazon.com/docs/app-submission/target-app.html). An aarch64 VPKG would have
  no device to target anyway.

### 2.8 Size
- Uploads can be up to 2.5 GB. Above 50 MB, or growth of more than 10% since the last
  submission, the console shows a warning but still lets you submit
  (https://developer.amazon.com/docs/app-submission/understanding-submission.html).
- Fire TV test criteria:
  - "Be 2 GB or less in size (recommended)".
  - Test 2.1: under 4 GB installed.
  - Test 2.2: install ideally within 15 s.
  (https://developer.amazon.com/docs/app-testing/test-criteria.html)
- Our release VPKG sizes:
  - Gravity Choir 1.3 MB, arcade 2.9 MB, Brew Volley 2.6 MB, Land Grab 2.1 MB, Doom 10.3 MB.
  - Quake 55 MB, SuperTuxKart 245 MB and SuperTux 278 MB get the over-50 MB warning.

### 2.9 Pricing, availability and Live App Testing
- **Free only on Vega for now.**
  - An Amazon staff reply says Vega devices (Fire TV Stick 4K Select, Fire TV Stick HD (2nd gen),
    Fire TV Stick 4K) "don't support paid apps" and that rejection is expected. Their suggestion:
    a free app with an IAP entitlement (community: "Paid App Support on Vega",
    https://community.amazondeveloper.com/t/paid-app-support-on-vega/29890).
  - Not found in the official docs. All our candidates are free, so this doesn't constrain us.
- **Live App Testing (LAT)** accepts VPKGs and runs against Amazon's production environment
  before you publish (KB: `live_app_testing_overview.md`).
  - LAT skips some listing fields (Fire TV assets, privacy, long description and others). You
    fill them in after promoting the test
    (https://developer.amazon.com/docs/vega/2.0/live-app-testing).
  - Use LAT to check three things:
    - that the console accepts a native VPKG;
    - that it installs on a stick *without* developer mode;
    - that the arcade's cross-app launch still works there (section 5).
- **Appstore Quality Central doesn't support VPKGs.** An Amazon staff reply says Quality
  Central's automated testing and Live Device Interaction are Fire OS only (community: "Can't we
  test Vega apps on Appstore Quality Center?"). Test on our own sticks.

## 3. Store assets and listing

### 3.1 Images and video
- **Fire TV assets** (https://developer.amazon.com/docs/app-submission/appstore-details.html,
  Fire TV tab, seen in the search snippet). The App Icon details also appear in community: "What
  images and videos are needed for Fire TV apps?".
  - **App Icon (required):** 1280×720 PNG with **no transparency**. Keep the subject and title
    inside the content-safe area, because badges overlay the icon.
  - **Screenshots:** 3 to 10 at 1920×1080, JPG or 24-bit PNG without transparency.
  - **Background image (optional):** 1920×1080.
  - **Feature Rotator:** listed on the tab. Not found in docs: whether it is required.
- **Tablet and website assets (required even for TV-only apps):**
  - a 114×114 small icon and a 512×512 large icon, both "PNG (transparent)";
  - 3–10 screenshots, with 1920×1080 among the allowed sizes;
  - optionally a 1024×500 promotional image and up to 5 videos (720p, at least 1200 kbps, at
    most 60 min, under 262.14 MB).
  - Sources: the Appstore Details page (tablet tab) and test 3.15 of the test criteria.
- **Asset rules:**
  - Don't scale up a PNG icon. No borders or rounded corners.
  - The file extension must match the real image type.
  - Screenshots show only the app.
  - Videos must not show unsupported devices.
  (https://developer.amazon.com/docs/app-submission/appstore-details.html)
- **What we have:**
  - Our 512×512 icons are fine for the large icon.
  - The arcade card art is 480×360, too small for the 1280×720 icon. Don't upscale it.
  - Capture new 1920×1080 frames on the stick with `gwsi-tool-screenshooter` (the docs name it for
    Vega screenshots). Our `tools/vega.sh shot` scales to 960 wide, so pull the full-size PNG.
  - For Gravity Choir, `host-preview.sh` can render frames on the Mac.

### 3.2 Text
- **Short description** (https://developer.amazon.com/docs/app-submission/appstore-details.html):
  up to 1,200 English characters, and Fire TV shows only the first 200. Paragraph breaks are
  stripped.
- **Long description:** up to 4,000 characters. Shown on the website, not on Fire TV.
- **Fields:** 3–5 feature bullets, optional keywords. Plain text only: HTML fails review.
- **Listing policy** (https://developer.amazon.com/docs/policy-center/listing-promo.html):
  metadata must be accurate and "unique from other apps you have previously submitted", with no
  ALL CAPS and no emojis. Write a distinct listing for each game.

### 3.3 Category
- Pick the closest category (Games, then a subcategory) on the Upload screen
  (https://developer.amazon.com/docs/app-submission/upload-app-file.html, "App category").
- This choice matters for Home behaviour: test 4.4 says that for "apps categorized as games",
  Home shows a "Game Paused" dialog (https://developer.amazon.com/docs/app-testing/test-criteria.html).
- Not found in docs: whether Vega shows that dialog.

### 3.4 Target audience, content rating, privacy
All three are on the Target Your App screen
(https://developer.amazon.com/docs/app-submission/target-app.html).
- **Target audience.** If the audience includes children, the Child-Directed App (COPPA) policy
  applies. Choose "16 and above" unless a game is designed for children.
- **Content rating questionnaire.**
  - Subject matter: violence, cartoon violence, drugs, nudity, sex, intolerance, profanity,
    academic.
  - Additional information: account creation or personal data, ads, gambling, location,
    user-generated content (UGC) or user-to-user communication.
  - Expected answers:
    - Doom and Quake need the violence answers.
    - SuperTuxKart probably needs account creation and UGC answers (add-ons, online play).
  - Content ratings can only change with a new binary.
- **User data privacy.** If you answer **Yes** to "Does your app collect user data or transfer
  user data to third-parties?", you must give a privacy-policy URL and fill in the questionnaire.
  - Gravity Choir, the arcade, Doom, Quake and the N64 games make no network calls and collect
    nothing, so the answer is No and no policy is needed.
  - SuperTuxKart talks to STK's servers (add-ons, news, accounts), so treat it as Yes.
- **UGC rule.** Apps with user-generated content "must include a method for filtering
  objectionable content, an age verification system, and a mechanism to report and remove
  offensive content" (content policy snippet,
  https://developer.amazon.com/docs/policy-center/understanding-content-policy.html). A store
  build of SuperTuxKart (and of SuperTux, if its add-on manager works) should turn online add-ons
  and online play off, or meet this rule.

### 3.5 Testing instructions and IP documentation
- **Testing instructions:** describe features that are hard to discover. List the remote and
  gamepad controls there (https://developer.amazon.com/docs/app-submission/review-submit.html).
- **IP documentation:** if an app uses third-party IP, Amazon may ask for "a confirmation letter
  from the IP rights holder … or a copy of the applicable license agreement", as a PDF sent to
  amazon-developer-program@amazon.com (https://developer.amazon.com/docs/app-submission/target-app.html).
  - For our ports that means the GPL, BSD, CC and OFL texts.
  - For trademarked names, possibly a permission letter (section 6).

## 4. Certification and test criteria relevant to games

### 4.1 What the Vega review runs
"Below are tests that Amazon Appstore runs on submitted apps" (KB: `vega_test_before_submission.md`;
https://developer.amazon.com/docs/vega/0.24/test-before-submission.html).
- **The tests:**
  - exit with Back and relaunch;
  - exit with Home and relaunch, with "No audio leaks from app on the Fire TV launcher";
  - switch between apps with no audio overlap;
  - HDMI hot-plug;
  - TV power off and input switch;
  - relaunch after a device restart;
  - **after the screensaver:** no app audio over the screensaver, and Back from the screensaver
    returns to the app;
  - **an Alexa interruption:** "No audio overlap between Alexa response and app audio";
  - Bluetooth audio output.
- **A physical stick is mandatory:** "you must test your app on a Fire TV Stick before
  submitting". Our three sticks qualify.

### 4.2 General Fire TV test criteria that apply to games
Source for this section: https://developer.amazon.com/docs/app-testing/test-criteria.html.
- **Remote (groups 4.1–4.8):**
  - The game must be usable with one hand.
  - It must not require motion events or rapid input.
  - It should "avoid simultaneous button inputs, especially for core functionality". Test 4.8
    singles out "D-pad input paired with the Select button".
  - Select must act on the highlighted item, and the D-pad must move focus.
- **Back (4.5):** optional, but it must not force-close or loop.
- **Home (4.4):** for games, a "Game Paused" dialog; the app must save its state.
- **Exit time (2.22):** under 2 s after Home.
- **Media keys (4.6, 5.6):** "In non-media apps, the media buttons should not be used for some
  other function". This is a "should". The one exemption is frameworks such as Unity that can't
  pass keys through to the system, and we can (Back already does).
- **Other keys (4.7):** a single button press must never exit to Home, except from the main menu.
- **Loading (2.3):** "If loading takes longer than 15 seconds, the app must indicate this to the
  user."
- **Performance (2.5, 2.7):** don't drop below 25 fps for an extended period. 55–60 fps is
  recommended for fast motion. No crashes or hard locks.
- **eMMC writes (2.6):** under 50 MB/h in the foreground and 2 MB/h in the background.
- **Screen (2.9):** fill at least 90% of the screen (80% minimum, centred). Landscape. Readable
  from 3 m.
- **Navigation (2.13):** no screen may trap the user, and users must never be forced out with
  Home.
- **Audio (2.18, 3.13):** acquire and abandon audio focus correctly. Stop audio when the user
  exits. Give up focus when another app starts playing.
- **Screensaver (3.14):** stop audio when the screensaver or standby starts.
- **Data (3.7, 3.8):** saves persist across restarts and updates.
- **Controllers (group 5, 3.12, 3.13):**
  - "All apps for which game controller compatibility has been declared" are tested with the
    Amazon Fire Game Controller:
    - A confirms;
    - B goes back;
    - Home pauses the game;
    - D-pad and sticks navigate.
  - Only Bluetooth HID gamepads are supported.
  - Local multiplayer must accept several controllers at once.
- **How a Vega app declares game-controller support or a controller requirement: not found in
  docs.** Fire OS does it through the Android manifest. This matters for Doom, Quake and maybe
  SuperTuxKart, which are only really playable with a pad.

### 4.3 Vega KPIs
- Guidelines from the KPI Visualizer
  (https://developer.amazon.com/docs/vega/0.24/measure-app-kpis; the 2.0 page has the same
  table):
  - TTFF under 1.5 s cold and 0.5 s warm.
  - TTFD under 8 s cold and 1.5 s warm.
  - **Foreground memory (PSS) under 400 MiB, background under 150 MiB.**
- Run them with `vega exec perf kpi-visualizer --app-name=<component> [--certification]`.
- A native app needs its own "fully drawn" trace marker for TTFD (KB:
  `vega_native_report_fully_drawn.md`). Without it, TTFD shows as −1.
- Not found in docs: whether these KPI guidelines are pass/fail review gates. Treat them as
  targets.

### 4.4 ANR and memory kills
- ANR: "An ANR happens when an app fails to respond to the Lifecycle Manager (LCM) or to system
  calls. LCM then forcibly terminates the unresponsive app" (KB: `vega_headless_tasks_api.md`;
  KB: `vega_analyze_anr_crash.md`).
  - Not found in docs: exact timeouts.
  - Our own measurement: no first frame for about 20 s means AppNotResponding (CLAUDE.md,
    decision 27). vegaplat now presents a black frame at GL init.
- Out-of-memory kills arrive as `SIGKILL` and crash reporters can't record them (Vega 2.0 known
  issues).

### 4.5 Audio focus on Vega
- The doc's required order (KB: `vega_audio_focus_api.md`):
  1. create a session;
  2. register a focus listener **before** requesting focus;
  3. request focus;
  4. react to GRANTED, DUCKED, PAUSED, STOPPED, RELEASED, MUTED;
  5. release focus "when playback completes or focus is no longer needed".
- The native IDL has the same model: `IAudioFocusSession::addFocusChangedListener()` with
  `IFocusChangedListener::onFocusGranted/Ducked/Paused/Stopped/Released/Muted`. This comes from
  the generated headers under `build/private/vodka/.../apmf/iface/com/amazon/kepler/audio/`.
- **What vegaplat does today** (`engine/vegaplat/src/vegaplat.cpp`, `vp_audio_open`):
  - It requests focus once and releases it only in `vp_audio_close`.
  - It registers no listener.
  - `vp_audio_set_paused` exists but nothing calls it.
  - The games pause their SDL streams on background, so writes stop. But focus is held while
    backgrounded, and Alexa ducking or pausing is ignored.
  - This is the most likely cause of failing the Alexa and app-switching cases above.

### 4.6 Screensaver
- Vega has no API to keep the screen on. The screensaver starts after five minutes without
  local input (Vega 2.0 known issues, "Screensaver starts during gameplay with a phone
  controller").
- Remote or gamepad play counts as local input, so this hits only attract modes and idle
  screens. The screensaver backgrounds the app, and it must then be silent (4.1).

## 5. Can an app launch other apps? (the arcade)

**Launching other apps is a documented feature:**
- The messaging system exists so apps can open other apps by URI: `pkg://<component-id>`,
  `amzns://apps?<id_type>=<id_value>` (by ASIN, package ID or store ID) and custom schemes (KB:
  `vega_deep_linking_api.md`).
- You may deep-link to an app's Appstore detail page by ASIN
  (https://developer.amazon.com/docs/vega/0.24/deep-links-vega-apps, section 2).

**The catch is the default privilege on `pkg://`:**
- Leaving out `[[message]]` for a `pkg://` URI is equivalent to declaring
  `sender-privileges = ["self"]`, where "`self` allows only the application package the module is
  part of to use the message" (https://developer.amazon.com/docs/vega/0.24/manifest-message).
- Read literally, only the game's own package may send `pkg://com.firetvnative.doom.main`.
- Our arcade does this successfully today, but only on a developer-mode stick.
- The deep-linking KB lists `vmsgr enforce-security --package <pkg>` as a separate test step,
  which suggests developer mode doesn't enforce messaging security.

**Inference to verify (`vmsgr check-access`, `vmsgr enforce-security`, then a Live App Test on a
non-developer stick): an Appstore install of the arcade may fail to launch the games.** The
documented cross-app pattern (KB: `vega_deep_linking_api.md`) is:
- each game declares a `[[message]]` (a custom scheme, or its `pkg://` URI) with
  `sender-privileges = ["*"]`;
- the game maps it with `[[offers.message-target]]` and exports it in an `[[offers.module]]`;
- the arcade declares `[[wants.module]]` for each game's module.

**Review risks:**
- **Launcher policy.** The policy prohibits apps that "override or alter the native device OS or
  user experience (e.g., … launchers …)"
  (https://developer.amazon.com/docs/policy-center/device-and-system-abuse.html). The submission
  FAQ says launcher apps that override the native experience won't be published
  (https://developer.amazon.com/docs/app-submission/review-submit.html).
  - The arcade is an ordinary app that opens other apps, not a home-screen replacement.
  - Still, don't call it a "launcher" in metadata. Never use `com.amazon.category.launcher`.
- **Dependence on other apps.** Test 3.9 says an app that relies on another app must not
  force-close when the other app is missing. Tests 3.1 and 3.2 say it must deliver its core
  function and every advertised feature. The presubmission checklist says "minimum functionality".
  - With no games installed, the arcade is a menu of "Couldn't open X. Is it installed?"
    messages (`apps/arcade/src/arcade.c`, line 807).
  - Better: send the user to the game's Appstore page with `amzns://apps?...`, and submit the
    arcade only after its games are live.
- **Unique content.** The presubmission checklist warns that apps "repetitive of other apps
  already available" may be rejected
  (https://developer.amazon.com/docs/app-submission/presubmission-checklist.html). A game hub
  plus separate game listings should be fine if each listing is distinct.

## 6. Licensing flags (not legal advice)

**What the Appstore policy says:**
- An app that "fails to abide by the terms of an open-source software license, such as failing to
  provide attribution … and/or modifications to the open-source code used if required by the
  license" is an IP violation and is rejected
  (https://developer.amazon.com/docs/policy-center/intellectual-property.html).
- A disclaimer does not make third-party IP use acceptable
  (https://developer.amazon.com/docs/policy-center/faq-policy.html).
- Most review failures are content-policy failures, and "you must own the rights to the content"
  (https://developer.amazon.com/docs/app-submission/target-app.html).

### 6.1 GPL apps (Doom, Quake: GPL-2.0+; SuperTux, SuperTuxKart: GPL-3.0+)
- **Source availability.**
  - The GPL requires that recipients of the binary can get the corresponding source and the
    licence text.
  - Our source is public on GitHub, but the packages don't say so. `vpt show-contents` finds no
    GPL text in the Doom, Quake, SuperTux or SuperTuxKart VPKGs:
    - Doom ships only `FREEDOOM-COPYING.txt`;
    - Quake ships LibreQuake's docs;
    - SuperTux ships `AUTHORS`;
    - SuperTuxKart ships `CREDITS` and font licences.
  - Fix: add an in-app licence/credits screen with the GPL text, and a source link pinned to the
    exact tag. Put the link in the long description too.
- **Store terms.** Whether Appstore distribution terms add "further restrictions" that clash with
  the GPL is the well-known App Store question. Not found in docs: Amazon's position.
  - The docs describe the "Apply Amazon DRM?" field only for Fire OS binaries
    (https://developer.amazon.com/docs/app-submission/upload-app-file.html).
  - Not found in docs: whether VPKG distribution applies any DRM or encryption.
  - The practical risk is an upstream copyright holder objecting. Ask the upstream projects
    first (next point).
- **Upstream consent and names.** SuperTux and SuperTuxKart are active projects, and STK ships
  its own official builds on other stores.
  - Listing an unofficial port under their name risks the "mischaracterizes its relationship
    with a brand" and "similar name" examples in the deception policy
    (https://developer.amazon.com/docs/policy-center/intellectual-property.html).
  - Ask the maintainers first. Label the listing clearly as an unofficial port, or let them
    publish it.

### 6.2 CC-BY-SA data (SuperTux, SuperTuxKart)
- Attribution is required wherever the game ships. Credits files exist in the packages; also
  mention the authors in the listing.
- ShareAlike: our ASTC-converted STK textures are adaptations. They must stay under CC-BY-SA, and
  the conversion scripts are already public.
- CC BY-SA forbids applying technological measures that restrict the licensed rights. Not found
  in docs: whether VPKG delivery counts.

### 6.3 Non-commercial and undocumented assets (Land Grab)
Source: `n64/jam24/CREDITS.md`.
- **Music is CC BY-NC 4.0.** Distribution through a commercial store, even for free, is a grey
  area for NC. Replace the two tracks, or get written permission from Ultrasyd and Phekkis.
- **Harness assets have no documented sources:** `squarewave.ttf` and the start/countdown sounds.
  Confirm or replace them before shipping.
- **Credits are not in the package.** The fonts are CC BY 3.0 and OFL, and the sound effects are
  CC BY 3.0/4.0, so credits must ship. The VPKG has no credits or licence file at all.
- **Name.** The concept credits "Blokus". Don't use that trademark in the listing.

### 6.4 Brew Volley
Sources: libdragon `examples/brew-volley/README.md` and `n64/recomp/brew_volley.toml`.
- **Licences are clean:** code Unlicense, art CC0, music public domain, background photo under the
  Unsplash licence.
- **Missing from the VPKG:** the Pacifico font is OFL, so its licence text must ship with it.
  The package has neither the OFL text nor any credits.

### 6.5 Trademarks
- **"Quake"** in "LibreQuake", and **"Doom"** in "Freedoom" and in our package ID: id Software
  owns these marks.
  - Freedoom and LibreQuake are the projects' own names. Still, keep "Doom" and "Quake" out of
    titles, keywords and descriptions except as plain factual references to engine heritage.
  - Expect an IP-documentation request.
- **"N64"** in "Brew Volley (N64)" and "Land Grab (N64)": a Nintendo mark. Remove it from display
  titles and metadata.
- **Emulators.** The content policy names "illegal … emulators" under security violations
  (https://developer.amazon.com/docs/policy-center/understanding-content-policy.html).
  - Our N64 apps are native recompilations of freely licensed homebrew that load no ROMs.
  - Describe them that way, and don't call them emulators or mention cartridges.
- **"Fire TV" and "Vega":** see 2.4. The Vega submission page says not to mention "Vega" in any
  field.

### 6.6 Fonts (OFL)
- The OFL allows bundling with software, including commercial software, if the licence goes with
  the font. The font may not be sold on its own.
- Already done:
  - the arcade ships `OFL-ChakraPetch.txt` and `OFL-PressStart2P.txt`;
  - Gravity Choir ships `OFL.txt`;
  - SuperTux and SuperTuxKart ship their font licences.
- Missing: Brew Volley (Pacifico) and Land Grab (Banana Brick).

## 7. Per-app readiness

Common to all eight:
- the manifest has `com.amazon.category.main`, `[os.version] 2.0/2.0`, audio `wants` and a
  valid ID;
- a 512×512 PNG icon (`apps/<app>/icon/icon.png`, all measured);
- a release armv7 VPKG;
- `vpt validate` passes ABI validation with one warning: "dlopen usages found … make sure that
  your dlopen dependencies are either bundled in your vpkg or available on the official Vega ABI
  list". We `dlopen("libGLESv2.so.2")`.
  - Not found in docs: whether `libGLESv2.so.2` is on the public OS ABI list
    (https://developer.amazon.com/docs/vega/2.0/linker-namespacing describes the list but the
    search did not return its contents).
  - Ask support if review flags it.

Also common to all eight: the audio-focus listener gap (4.5) and the missing Fire TV assets
(3.1).

| App | Code / data licence | Already fine | Missing before submission | Verdict |
|---|---|---|---|---|
| **gravity-choir** | MIT; OFL font (licence in VPKG); synthesized audio | 1.3 MB, v1.0.0 build 1; 60 fps over a 2 h TV run; designed for the remote (inertia, so move then tap OK); Back goes play → pause → title → exit; background pauses; best score in `/data`; no network | audio focus listener; Fire TV assets; run the Vega test cases (4.1) on a stick; listen to the audio on the TV; try a gamepad; Play/Pause triggers the Crescendo (media-key guideline 4.6: also map it to a non-media input, or accept the risk) | **First candidate** |
| **arcade** | MIT; OFL fonts (licences in VPKG) | 2.9 MB; handles a missing game without crashing; Play/Pause toggles music (a media-like use) | cross-package launch under enforced messaging security (section 5); Appstore fallback for missing games; useless until games are live; avoid "launcher" wording; synthesized music in attract mode must stop for the screensaver | Last |
| **doom** (Freedoom) | GPL-2.0+ engine; Freedoom BSD-3 data | 10.3 MB; OPL music; "Press Y" prompts answered by OK | GPL text and source link in the app; fire and move need D-pad and OK together (4.2), so a gamepad is effectively required, and how to declare that on Vega is not in the docs; media keys mapped to use/strafe; violence rating; "Doom" trademark | Later |
| **quake** (LibreQuake) | GPL-2.0+ engine; LibreQuake BSD-3 data | 55 MB (over the 50 MB warning); map loads 9–13 s (under 15 s) | as Doom, plus media keys mapped to jump/weapons; "Quake" in the title | Later |
| **supertux** | GPL-3.0+; CC-BY-SA data | 278 MB; plays with the remote (OK jumps, Back opens the menu) | GPL text and source link; a visible loading indicator (it loads for over 20 s behind a black frame, decision 27, against 4.2's 15 s rule); running and jumping use D-pad and OK together; check whether the add-on manager works (UGC); upstream consent for the name | Later |
| **supertuxkart** | GPL-3.0+; CC-BY-SA, CC0 and GPL data | 245 MB; 57–60 fps on 11 tracks; HTTPS add-ons, news and accounts work | Black Forest 22 fps and Cocoa Temple 35 fps (4.2: not below 25 fps for long); measure foreground PSS against 400 MiB and background against 150 MiB; privacy policy and UGC rules unless online features are off; gamepad untested; upstream consent | Later (most work) |
| **n64-brew-volley** | Unlicense code; CC0 art; public-domain music; Unsplash photo; OFL font | 2.6 MB; 60 fps | upstream README says "should be played with two controllers" (remote = P1, gamepad = P2), so check that one reviewer with only a remote can play; OFL text and credits; remove "N64" from the title; describe as a native port, not emulation | Not yet |
| **n64-landgrab** | MIT code; CC BY-NC music; CC BY / OFL fonts; CC BY / CC0 sound effects | 2.1 MB | replace or clear the NC music and undocumented harness assets; ship credits; remove "N64" from the title; no "Blokus" | Not yet |

## 8. Recommended first submission: Gravity Choir

Why Gravity Choir:
- **It carries the least licensing and IP risk:** our own MIT code, one OFL font with its licence
  already in the package, and audio synthesized at run time, with no third-party trademarks.
- **It fits the remote criteria:** inertia movement means a player releases the D-pad before
  tapping OK.
- **It needs nothing from the review that we can't supply:** no network, no account, no data
  collection, so the answers are No and there is no privacy policy.
- **It has the best performance record of our apps:** a measured steady 60 fps.
- **It is tiny,** so installs are quick and there is no size warning.

Work list before submitting it:
1. **Audio focus in vegaplat.**
   - Register an `IFocusChangedListener` before `requestAudioFocus()`.
   - Pause or duck on PAUSED, DUCKED or STOPPED, and resume on GRANTED.
   - Release focus on background and request it again on foreground.
   - Then rerun the Alexa and app-switching cases. Every app benefits.
2. **Run the Vega test cases on a stick, Release build:**
   - Back exit and relaunch;
   - Home and relaunch;
   - switching apps;
   - screensaver: let it idle on the title screen with music and check silence;
   - restart;
   - Alexa interruption;
   - HDMI or input switch;
   - Bluetooth audio, if a speaker is available.
   Time the exit after Home (under 2 s).
3. **Run the KPI Visualizer** (`--certification`) for cold and warm start and foreground and
   background memory. Optionally add the native fully-drawn marker (KB:
   `vega_native_report_fully_drawn.md`).
4. **Decide the Play/Pause question.** Either also map the Crescendo to a non-media input, or
   keep Play/Pause and say so in Testing Instructions.
5. **Add an About or Credits screen** with the MIT notice, the OFL credit for Chakra Petch and a
   source link.
6. **Make the assets:**
   - 1280×720 Fire TV icon (no alpha);
   - 3–10 screenshots at 1920×1080 from the stick (or `host-preview.sh`);
   - 114×114 and 512×512 icons with alpha;
   - optional 1920×1080 background.
   Write the short description so the first 200 characters carry the pitch.
7. **Fill in the listing:** category Games (arcade or action subcategory), free, audience 16+ or
   all ages as chosen, rating questionnaire (cartoon-like abstract violence at most), privacy No.
8. **Check the gamepad and the 4K sticks,** or limit the listing's claims to the remote.
9. **Live App Test first,** with the release VPKG at build number 2 or higher. Confirm that it
   uploads, targets all three Vega sticks, installs and launches from Home on a stick without
   developer mode, and that the best score survives an update. Then promote it and submit with
   a fresh build number.

## 9. Open questions (not found in docs)

- Whether a native-only VPKG passes ingestion and review. Answer it with a Live App Test.
- How to get a "production" VPKG signing key, and whether the default developer-profile
  signature is what the console expects.
- Whether `libGLESv2.so.2` (loaded with `dlopen`) is on the public Vega ABI list.
- How a Vega app declares game-controller support or a controller requirement for review.
- Whether cross-package `pkg://` launches are allowed with messaging security enforced.
- Whether Vega shows a "Game Paused" dialog on Home for apps in the Games category.
- Whether VPKG distribution applies DRM or other technical measures (relevant to GPL and CC BY-SA).
- Whether the KPI guidelines are pass/fail gates in review.
- Whether the Feature Rotator image is required for Fire TV listings.

## 7. Follow-up findings (2026-11-15, packaging the licences)
Found while adding `assets/licenses/` and a `NOTICE.txt` to every package (not legal advice):
- **SuperTux 0.7.0 has two CC BY-NC 4.0 assets** (`images/objects/conveyor_belt/*` and `kill.wav`, per its `data/AUTHORS`). That's the same non-commercial question as Land Grab's music. A free listing may be fine, but replacing them would remove the question.
- **Land Grab's Anita Semi-Square font:** the jam credits say CC BY 3.0, but the TTF's own metadata says CC BY-SA 4.0. The NOTICE states both. Its Squarewave font has no documented licence (copyright field "jeti").
- **Amazon's code inside our GPL apps:** every app statically links the IDL import library generated from the Vega SDK (`<target>_idl`) and the SDK toolchain's libc++. Whether the GPL's system-library exception covers these needs a look before submitting any GPL game. The original MIT games (Gravity Choir) don't have this question.
- **Source offer:** each NOTICE points at the GitHub repository. Strict GPL compliance for a store build would tag the exact commit of each submitted build, and may mirror the third-party source archives next to it.
- Licence texts that couldn't be shipped verbatim are named with a link in the NOTICE instead: obstack (LGPL-2.1+, SuperTux) and newlib (parts recompiled into the N64 apps).
