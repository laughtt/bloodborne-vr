BLOODBORNE VR  (early alpha)
============================

*** EARLY ALPHA ***
Expect many bugs and crashes. Please be patient: this is a first public test, not a finished game.
* Crash reports stay on YOUR PC (%LOCALAPPDATA%\BloodborneVR\crashes, or crashes\ next to the program).
  They are never uploaded. If you want to help, look inside report.zip and send it yourself.
* To report a bug: open an issue on the project page (the "Bug report" form) and attach
  logs\bbvr-latest.log. See REPORTING BUGS at the end.
* The dashboard shows this notice once per version (press OK); the launcher also prints it at start.

An unofficial fan project that runs YOUR copy of Bloodborne (PS4, CUSA03173, version 1.09)
natively on Windows with a Vulkan renderer and an OpenXR bridge for VR headsets.

LINKS
-----
* Download (latest version): https://github.com/laughtt/bloodborne-vr/releases/latest
* Support the project:      https://ko-fi.com/josemauriciocarpioherrera
* Follow on X:               https://x.com/carpio_her8717

LEGAL
-----
* You must own Bloodborne and use your own legally obtained, decrypted dump. NO game files are
  included in this download, and none are ever uploaded. Do not ask for, post or link to dumps.
* Unofficial: not affiliated with, endorsed or sponsored by Sony Interactive Entertainment,
  FromSoftware or any trademark holder. PlayStation, PS4 and Bloodborne are trademarks of their
  owners.
* Free software under the GPL (see LICENSE, COPYING.GPL-3.0, THIRD_PARTY_NOTICES.md, SOURCES-THIRD-PARTY.txt, SOURCE.txt).
* Co-op (up to 3 players) uses the default lobby that comes with this release (nothing to type); solo play needs no server.

WHAT YOU NEED
-------------
* Windows 10/11, 64-bit; a GPU with Vulkan 1.3 and a current driver (NVIDIA RTX 2000+, AMD
  RX 5000+ or similar recommended; some AMD cards still crash when the world loads).
* About 6 GB of free memory (RAM + page file) and about 1 GB of disk (the extracted VR data is ~100 MB).
* An OpenXR headset (Quest via Link/Virtual Desktop/Steam Link, Pimax, Index, WMR ...) with its
  software running and set as the ACTIVE OpenXR runtime.
* Your Bloodborne dump, version 1.09 (folder with eboot.bin and dvdroot_ps4).
* Nothing else: Python and all libraries are in this folder.

INSTALL
-------
This download is the portable zip: nothing is installed, and the program, saves, settings and logs all stay in
its own folder. Follow the 4 steps below. (If you downloaded a BloodborneVR-<version>-Setup.exe instead, it
installs for your user only and keeps its data in %LOCALAPPDATA%\BloodborneVR; these steps then do not apply.)

INSTALL THE ZIP IN 4 STEPS
--------------------------
1. Extract the zip somewhere with write access, for example C:\BloodborneVR
   (not Program Files, not inside OneDrive). Keep the whole folder together.
2. Start your headset software and make it the active OpenXR runtime
   (SteamVR: Settings > OpenXR; Oculus app: Settings > General; Pimax Play / WMR: same switch).
3. Run BloodborneVR.exe. It opens the dashboard window. On the first run it asks you to accept the
   legal note, then click "Choose folder" and pick the folder of YOUR game dump (the one that contains
   eboot.bin). The Ready check on the Play page shows what is ready (game folder, headset runtime,
   graphics card and driver, Vulkan, DLSS).
4. Pick a mode (VR, Co-op or Flat) and a VR quality (Low, Medium, High, Super High) on the Play page and press
   the big PLAY button. The window has a sidebar of pages (Play, Graphics, Display & FPS, VR comfort, Combat &
   hands, Controls, Online co-op, Advanced, Log, About); PLAY, Stop and the status line are always at the bottom.
   The first start checks the dump and builds the VR menu and hit-shape data from it (a few minutes,
   once; stays on your PC in out\vr_assets), the Log page shows the progress. Put the
   headset on when it says "Starting". The first start compiles shaders and can take a while.
   Logs are in the logs\ folder next to BloodborneVR.exe.

WINDOWS WARNING ON FIRST START: BloodborneVR.exe is an unsigned program (the alpha has no code-signing
certificate yet), so Windows SmartScreen may say "Windows protected your PC". Click "More info", then
"Run anyway". If Windows 11 "Smart App Control" blocks it outright, turn that off. The SHA-256 sums of
the download are in SHA256SUMS.txt next to the zip.

Web settings page instead of the dashboard: BloodborneVR.exe --web (or BloodborneVR-console.bat). Both edit
the same settings. Any other launcher option also runs the console launcher:
  BloodborneVR.exe --check (checks only)   --mono (VR render mode Mono)  --mode vr|coop|flat  --quality Low|Medium|High|"Super High"
  --game-dir "D:\dump\CUSA03173"           --reset (forget game folder and data)

CONTROLS (alpha; may change)
----------------------------
Picture: controls.png in this folder (a labelled controller map).

  Control                  Does
  -----------------------  ---------------------------------------------------------
  B (right, upper)         Roll / dodge; hold to sprint
  A (right, lower)         Switch the right-hand weapon (one press, one swap)
  Y (left, upper)          Interact: doors, levers, pick up, talk
  X (left, lower)          Switch the left-hand weapon (one press, one swap)
  Left stick               Move
  Left stick click         Moving: jump. Standing: open / close the dashboard. Held 1 s: recenter
  Right stick              Turn / camera (smooth or snap: Options > Comfort)
  Right stick click        Open the item wheel
  Left trigger             Hold: aim the gun (red laser); release: shoot. A left-hand thrust also shoots
  Right trigger            Nothing in the world (pointer click in the dashboard)
  Left grip                Transform (L1)
  Right grip               Heavy / charge modifier for swings; a quick tap swaps the right weapon
  Left menu button         Options (pause); held 1 s: recenter

There is no heal button: heal from the item wheel, or bring a vial to your mouth.
In the game's own menus: A confirms, B goes back, either stick moves the cursor.

  Gesture                                  Does
  ---------------------------------------  ---------------------------------------------
  Swing the right hand                     Light attack; with the right grip held: heavy attack
  Thrust the left hand (trigger held)      Shoot
  Shoot an attacking enemy just in time    Parry (stagger)
  Jab the right hand at a staggered enemy  Visceral attack (up to 2 m away, automatic after a parry)

Tuning files re-read while the game runs: out\fp.txt (first person, turning, comfort),
out\stereo.txt (stereo scale). They are created from defaults\ on the first run.

ITEMS: PICK, THEN USE WITH A GESTURE
------------------------------------
1. Open the item wheel (right stick click) or the dashboard (left stick click, standing).
2. Point at the item and pull the right trigger: it goes straight into your hand (no grip squeeze). The weapon of that
   hand hides and that hand cannot attack until the item is used or put back.
3. Use it:

  Item                                   Gesture
  -------------------------------------  ------------------------------------------------
  Blood vials, pellets, drinks           Bring it to your mouth (generous zone, forgiving)
  Bells                                  Bring it to your head
  Papers (fire, bolt)                    Touch it to the blade, or to the big zone in the air
                                         just in front of the blade (toward you)
  Molotovs, knives, pebbles, oil urns    Throw: hold the grip to aim (an arc and a ring show where it
                                         lands), let go to throw
  Hunter's marks, shaman bone blade      Crush: squeeze hard for 0.4 s

4. Put it back: press a trigger, or open any menu.
Drag and drop (dashboard): squeeze the grip on an inventory card, drag it to a quick slot, let go to assign it.
Hits with your weapon and bounces off walls give a haptic pulse. The grey hand markers and the gun laser are
off by default.

PLAYING CO-OP (up to 3 players, join codes)
-------------------------------------------
Co-op is EXPERIMENTAL in this alpha. It uses the default lobby that comes with this release: there is
nothing to type. Playing over the internet is not fully tested. Everyone needs game version 1.09 and this same
release. The VR and Flat modes need no server and no account.

A) HOST (the player whose world the others join)

1. Open BloodborneVR.exe (the dashboard).
2. On the Play page pick Co-op.
3. Press Host a game. A room code like K7Q-M4X appears (it stays on the Play page while the room lasts).
4. Send the code to your friends (Discord, text message ...). Up to 2 friends can join (3 players in total).
5. Press PLAY, load your character and walk a little way into an area. Do not stand in the
   area's entry zone, and the area's boss must still be alive.
6. Nothing to press in the game: your bell rings by itself and your friends appear.

B) FRIEND (joining)

1. Open BloodborneVR.exe.
2. On the Play page pick Co-op.
3. Type the room code under "or join a friend" and press Join.
4. Press PLAY, load your character and stay in the same region as the host.
5. Nothing to press in the game: your bell rings by itself and the host summons you. If you die or get sent home,
   it rings again by itself.

Running your own lobby? Under Co-op press the small Change next to "Lobby: default", enter your
address and press Use. "Use the default lobby" switches back.

If it does not work

  * Everyone must use the same release and have game version 1.09, and the friend must type the host's room code exactly.
  * The host must not stand in an area's entry zone, and that area's boss must not be dead yet.
  * Wait about a minute. If you saw "failed to acquire server information", restart the game.
  * The Windows firewall must allow BloodborneVR (allow it when Windows asks).
  * Co-op is experimental and internet play is not fully tested.

Leave a room / play solo: press Leave (the host sees Close room) in the room card, or Leave room
on the Online co-op page. Pick VR or Flat to play without a room.

COMFORT, ONE-EYE VIEWING AND WEAKER PCS
----------------------------------------
On the dashboard's Play page, Mode is VR, Co-op or Flat, VR quality is Low, Medium, High or Super High (High plus DLSS-NR before the upscaler: needs your own nvngx_dlssnr.dll, which is not included; about 50-54 fps on an RTX 4090, full stereo; see docs/dlss-nr.md) (the Graphics page
lists what each one sets) and VR render mode is Stereo (true 3D, the default) or Mono. Every change is saved at once;
the big PLAY button at the bottom starts the game and Stop ends it.

  * Cannot see 3D with both eyes, flicker, eye strain or discomfort: set VR render mode to Mono (Play page, or
    Display & FPS; or start with BloodborneVR.exe --mono). You get one big flat screen in the headset and the
    controllers still work. If the depth looks wrong in Stereo, try Swap eyes and Stereo scale (Display & FPS,
    switch "Show advanced options" on).
  * Motion sickness: the VR comfort page has snap turning, the locked and eased camera animation and a steadier camera
    (the older Comfort profile in Advanced sets them in one go).
  * Weaker PC or low fps: pick Low (or Medium) quality: a smaller eye image, and lower model detail on Low. You can
    also lower Frame limit (fps). The preset table on the Graphics page shows the eye image size of each one.
  * Defaults: VR render mode Stereo, quality High (DLSS quality on an RTX card, else FSR 1 at 2x the 1080p frame),
    Frame limit 72 fps, FXAA, smooth turning (90 degrees per second), Stereo scale 0.77.
  * Flat mode plays on the monitor with no headset; Co-op can be played in VR or on the flat screen.

UPDATING (KEEP YOUR SAVES)
--------------------------
* Close the game and the dashboard first. Extract the new zip into a NEW folder, then copy the old folder's
  user\ folder (and bbvr_config.json if it exists) into it. You can also extract the new zip over the old folder
  (your user\ folder is not part of the zip, so it stays). Optional: copy out\ too to skip the first-start data
  build. The dashboard only tells you when a newer version exists; it does not install it for you.
* If you later switch to a Setup.exe version: copy the zip folder's user\savedata to
  %LOCALAPPDATA%\BloodborneVR\user\ (create the folder if needed) before the first start of the installed version.

SETTINGS (the option pages of the dashboard)
-------------------------------------------
Pages: Graphics (VR quality presets, upscaler, DLSS, game effects), Display & FPS (frame limit, fps counter, render mode),
VR comfort (turning, camera, dodging, HUD and menus, wrist bar colours), Combat & hands, Advanced (switch "Show
advanced options" on, in the sidebar, for more). VR quality presets (Low, Medium, High, Super High; all true stereo, both eyes rendered): the scene is always rendered at
1920x1080 and the upscaler makes the eye image, so a bigger eye image looks sharper and costs more GPU time. Low: 1920x1080
per eye (DLSS at the render size), lower model detail. Medium: 2400x1350 (Display resolution 85%). Low starts at 70%. High: DLSS quality, 2880x1620 (FSR 1 at 2x
without DLSS). Super High: the same eye image with DLSS-NR on (placed before the upscaler), greyed out without your own nvngx_dlssnr.dll (about 50-54 fps on an RTX 4090, full stereo). Display size (Play page): your headset (Auto, a named one, custom) and Display resolution (50-150%) set the eye image for it and win over the preset's own size; the preset picks the upscaler and DLSS-NR. Any single value you change on the Graphics page stays on top of the preset (marked
EDITED; "Reset to preset" undoes it); choosing a preset drops those edits. The older profiles (Comfort, Performance,
Quality (DLSS), Custom) are on the Advanced page. A gold LIVE tag means the option applies while the game
runs (keep the dashboard open and change it there); RESTART needs a new start. Changes are saved at once.
Combat & hands > Hitboxes: Big (default) makes your weapon's hit shapes wider and a little longer; Original uses exactly the game's own
shapes. Enemies are never changed. Live.
The web page (--web) shows the same options, opens in Edge / Chrome / your browser, is served only on your own PC
(127.0.0.1) and is protected by a one-time token. Changed control files keep a .bak of the original.
DLSS is the default upscaler on an NVIDIA RTX GPU (High: quality, preset K; other GPUs use FSR 1) and works out of the box: bin/nvngx_dlss.dll
(NVIDIA's DLSS runtime, unmodified, redistributed under the NVIDIA RTX SDKs LICENSE) and bin/bbport_dlss.dll (our bridge)
are part of this download. This program uses NVIDIA DLSS; the licence and the end-user terms are in
licenses/NVIDIA-DLSS-LICENSE.txt and licenses/NVIDIA-DLSS-NOTICE.txt. Not sponsored or endorsed by NVIDIA.
DLSS-NR (Super High; experimental): an optional NVIDIA neural-rendering pass (RTX GPU only). Its model file nvngx_dlssnr.dll is
NVIDIA's, is not part of the public DLSS SDK and is NOT included; this project does not link to a download. If you already have a
copy on your PC, press "Find file..." on the Super High card (or the Graphics page): the launcher copies it next to bb-probe.exe.
"What is this?" opens the short help page (docs/dlss-nr.md). Without the file the game runs High. Super High places DLSS-NR before
the upscaler (dlssnr_place=pre, 1 pass); the Graphics page has the other settings.
ReShade: a globally installed ReShade Vulkan layer is kept out of the game automatically.
Choosing an OpenXR runtime on the Advanced page affects only this game. SteamVR on Pimax can show double
images: pick the Pimax runtime.

KNOWN ISSUES
------------
* EARLY ALPHA: expect many bugs, crashes and comfort problems. Take breaks; sit if you feel unwell.
* Some AMD graphics cards crash when the game world loads.
* The character preview on the character-creation screen can stay empty (the character itself
  is created correctly).
* Only version 1.09 works (the patches are for that version). Other versions are refused.
* The first start after a driver update recompiles shaders.

REPORTING BUGS
--------------
Open an issue on the project page (use the "Bug report" form) and attach logs\bbvr-latest.log.
If the game crashed, a report was saved in %LOCALAPPDATA%\BloodborneVR\crashes (the dashboard's About tab
has an "Open crash reports folder" button). Nothing is sent automatically: if you want to help, send that
report.zip to the developer (your user name, folders and IP addresses are removed from it; look inside first).
Never post saves or game files.
bin\bb-gpu-capabilities.exe prints your GPU's Vulkan features: include its output.
