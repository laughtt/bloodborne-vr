BLOODBORNE VR  (alpha, single-player)
=====================================

An unofficial fan project that runs YOUR copy of Bloodborne (PS4, CUSA03173, version 1.09)
natively on Windows with a Vulkan renderer and an OpenXR bridge for VR headsets.

LEGAL
-----
* You must own Bloodborne and use your own legally obtained, decrypted dump. NO game files are
  included in this download, and none are ever uploaded. Do not ask for, post or link to dumps.
* Unofficial: not affiliated with, endorsed or sponsored by Sony Interactive Entertainment,
  FromSoftware or any trademark holder. PlayStation, PS4 and Bloodborne are trademarks of their
  owners.
* Free software under the GPL (see LICENSE, THIRD_PARTY_NOTICES.md, SOURCE.txt).
* This alpha is single-player only.

WHAT YOU NEED
-------------
* Windows 10/11, 64-bit; a GPU with Vulkan 1.3 and a current driver (NVIDIA RTX 2000+, AMD
  RX 5000+ or similar recommended; some AMD cards still crash when the world loads).
* About 6 GB of free memory (RAM + page file) and about 1 GB of disk (the extracted VR data is ~100 MB).
* An OpenXR headset (Quest via Link/Virtual Desktop/Steam Link, Pimax, Index, WMR ...) with its
  software running and set as the ACTIVE OpenXR runtime.
* Your Bloodborne dump, version 1.09 (folder with eboot.bin and dvdroot_ps4).
* Nothing else: Python and all libraries are in this folder.

INSTALL IN 4 STEPS
------------------
1. Extract the zip somewhere with write access, for example C:\BloodborneVR
   (not Program Files, not inside OneDrive). Keep the whole folder together.
2. Start your headset software and make it the active OpenXR runtime
   (SteamVR: Settings > OpenXR; Oculus app: Settings > General; Pimax Play / WMR: same switch).
3. Run BloodborneVR.exe. It opens the dashboard window. On the first run it asks you to accept the
   legal note and whether crash reports may be sent, then click "Choose folder" and pick the folder
   of YOUR game dump (the one that contains eboot.bin). The "This PC" list shows what is ready
   (game dump, VR runtime, graphics card, DLSS files, crash reports).
4. Pick a profile (Default, Comfort, Performance, Quality (DLSS)) and press the big START VR button.
   The first start checks the dump and builds the VR menu and hit-shape data from it (a few minutes,
   once; stays on your PC in out\vr_assets), the log under the button shows the progress. Put the
   headset on when it says "Starting". The first start compiles shaders and can take a while.
   Logs are in logs\ .

WINDOWS WARNING ON FIRST START: BloodborneVR.exe is an unsigned program (the alpha has no code-signing
certificate yet), so Windows SmartScreen may say "Windows protected your PC". Click "More info", then
"Run anyway". If Windows 11 "Smart App Control" blocks it outright, turn that off. The SHA-256 sums of
the download are in SHA256SUMS.txt next to the zip.

Web settings page instead of the dashboard: BloodborneVR.exe --web (or BloodborneVR-console.bat). Both edit
the same settings. Any other launcher option also runs the console launcher:
  BloodborneVR.exe --check (checks only)   --mono (flat screen in the headset)
  --game-dir "D:\dump\CUSA03173"           --reset (forget game folder and data)

CONTROLS (alpha; may change)
----------------------------
Picture: controls.png in this folder (a labelled controller map).
* Left stick: move. Right stick: camera / turn (smooth or snap: Options > Comfort).
* Right-hand swing: light attack (R1). Swing with the right grip held, or a very hard swing:
  heavy attack (R2); keep the grip held after the swing to charge.
* Quick right-grip tap (under 0.35 s, no swing): swap the right-hand weapon.
* Right trigger: nothing in the world.
* Left trigger (hold): hold the gun (L2). Left-hand thrust forward: shoot.
* Left grip (squeeze): L1 (transform).
* A / B / X / Y: Circle (dodge, sprint) / Cross (interact) / Square (use quick item) /
  Triangle (blood vial).
* Right stick click (tap): item wheel; pick with your right hand (touch or point) or the stick.
  Lock-on is off by default.
* Left stick click while standing: VR dashboard (equipment, items, options). While moving: jump.
  Held 1 second: recenter.
* Left menu button: Options (pause).
* Swing at a staggered enemy: visceral attack (stagger one with a gun parry, or a charged heavy
  from behind).
* In the game's own menus (Options, pause): either stick moves the cursor (hold to repeat),
  A or either trigger confirms, B goes back, the menu button closes the menu.
TODO: verify every row on the current build in the headset before release.
Tuning files re-read while the game runs: out\fp.txt (first person, turning, comfort),
out\stereo.txt (stereo scale). They are created from defaults\ on the first run.

SETTINGS (Options and DLSS tabs of the dashboard)
-----------------------------------------------
Groups: Display / VR, Comfort, Combat, Body / arms, UI, Patches, Advanced (switch "Advanced" on for more); the DLSS
tab has the upscaler options. Profiles: Default (the old behaviour), Comfort, Performance, Quality (DLSS) and
Custom (your own values; editing any option switches to it). A blue LIVE tag means the option applies while the game
runs (keep the dashboard open and change it there); RESTART needs a new start. Changes are saved at once.
The web page (--web) shows the same options, opens in Edge / Chrome / your browser, is served only on your own PC
(127.0.0.1) and is protected by a one-time token. Changed control files keep a .bak of the original.
DLSS (optional, off by default; falls back to FSR 1) is bring-your-own and NOT part of this download: it needs an
NVIDIA RTX GPU, NVIDIA's nvngx_dlss.dll and a bbport_dlss.dll bridge that you build against NVIDIA's DLSS SDK (the
bridge source is not published yet). Put both DLLs into the bin folder. The DLSS tab says what is missing.
ReShade: a globally installed ReShade Vulkan layer is kept out of the game automatically (set BB_ALLOW_RESHADE=1 to allow it).
Choosing an OpenXR runtime in Advanced affects only this game. SteamVR on Pimax can show double
images: pick the Pimax runtime.

KNOWN ISSUES
------------
* Alpha: expect rough edges, bugs, and comfort problems. Take breaks; sit if you feel unwell.
* Some AMD graphics cards crash when the game world loads.
* The character preview on the character-creation screen can stay empty (the character itself
  is created correctly).
* Only version 1.09 works (the patches are for that version). Other versions are refused.
* The first start after a driver update recompiles shaders.

REPORTING BUGS
--------------
Open an issue on the project page (use the "Bug report" form) and attach logs\bbvr-latest.log.
If the game crashed and you agreed to crash reports, quote the crash id the game printed.
Crash reports are opt-in: you are asked once, you can see the report on your PC first, and
you can turn it off (docs\CRASH_REPORTS.md in the source). Never post saves or game files.
bin\bb-gpu-capabilities.exe prints your GPU's Vulkan features: include its output.
