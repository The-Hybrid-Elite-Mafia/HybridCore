## The Hybrid Core — 0.5.1 visual overhaul (test build)

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX).

**What changed**
- The console is now drawn on **Main-Faceplate.png**: your chrome, glass and neon art is the screen.
- Knobs are layered (shadow, socket cut from your faceplate, rotating spun-metal body and pointer, fixed light), with glowing value arcs.
- Chrome fader caps with a coloured centre line, recessed slots, glass LED meters with peak hold, chrome M / S / Ø caps that light from inside.
- Poppins type: engraved on chrome, soft-shadowed on glass, neon in the display.
- Glass display shows live engine numbers: latency, DSP load, driver, buffer, dropouts.
- Round buttons: previous / next device, test tones, hardware inputs, clear solo, clear mute. Tabs pick what the 13 stem faders control (main mix or aux 1–3 sends).
- New diagnostics shortcut "5 - Screen Snapshots" renders the console on your graphics card into a desktop folder.
- The Pearl / Obsidian / Titanium themes are gone (they were the drawn look). A different faceplate in the same layout can go in `%APPDATA%\HybridCore\skins\Main-Faceplate.png`.

**How to test:** see `HOW-TO-TEST.txt` (Start menu → The Hybrid Core → How to test).

**Verified in CI** (GitHub Windows Server 2022 runner — no sound card, no GPU): builds, unit tests, VST3 validator, UI snapshots with the live engine compared against the faceplate, the real app starts and draws, installer install / upgrade / uninstall.

**Not tested yet:** your graphics card (look, frame rate, 8× anti-aliasing), sound through real speakers, your ASIO driver, the plug-in inside a real DAW.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".
