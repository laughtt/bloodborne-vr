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
* This alpha is single-player only. Online and co-op features are not part of it.

WHAT YOU NEED
-------------
* Windows 10/11, 64-bit; a GPU with Vulkan 1.3 and a current driver (NVIDIA RTX 2000+, AMD
  RX 5000+ or similar recommended; some AMD cards still crash when the world loads).
* About 6 GB of free memory (RAM + page file) and about 1 GB of disk (the extracted VR data is ~100 MB).
* An OpenXR headset (Quest via Link/Virtual Desktop/Steam Link, Pimax, Index, WMR ...) with its
  software running and set as the ACTIVE OpenXR runtime.
* Your Bloodborne dump, version 1.09 (folder with eboot.bin and dvdroot_ps4).
* Nothing else: Python and all libraries are in this folder.

INSTALL IN 5 STEPS
------------------
1. Unzip this folder somewhere with write access, for example C:\BloodborneVR
   (not Program Files, not inside OneDrive).
2. Start your headset software and make it the active OpenXR runtime
   (SteamVR: Settings > OpenXR; Oculus app: Settings > General; Pimax Play / WMR: same switch).
3. Double-click BloodborneVR.exe. On the first run it asks you to accept the legal note and to
   pick the folder of YOUR game dump. It checks the version and the executable (it will say
   if your dump is not compatible), checks Vulkan and OpenXR, and builds the VR menu and
   hit-shape data from your dump (a few minutes, once; stays on your PC in out\vr_assets).
4. Put the headset on when it says "Starting". The first start compiles shaders and can take a
   while; later starts are faster.
5. Play. Closing the console window or leaving the game ends it. Logs are in logs\ .

WINDOWS WARNING ON FIRST START: BloodborneVR.exe is a tiny unsigned program (the alpha has no code-signing
certificate yet), so Windows SmartScreen may say "Windows protected your PC". Click "More info", then
"Run anyway". If Windows 11 "Smart App Control" blocks it outright, turn that off or use
BloodborneVR-console.bat, which does the same thing. The SHA-256 sums of the download are in
SHA256SUMS.txt next to the zip. A black console window opens next to the settings page: that is
the launcher's log; leave it open while you play. Crash reports: the settings page asks once whether
you want to send them (see REPORTING BUGS below).

Other options:  BloodborneVR.exe --check (checks only)   --mono (flat screen in the headset)
                --game-dir "D:\dump\CUSA03173"           --reset (forget game folder and data)

CONTROLS (alpha; may change)
----------------------------
* Left stick: walk (where you look). Right stick left/right: turn (smooth by default; see
  out\fp.txt "turn=").
* Sword/weapon: swing the controller; the game's own attacks, stamina and animations run.
* Trigger / grip: finger curl; the grip pulses L1 (guard / transform).
* A tapped: dodge/roll in the stick direction. Left stick click while walking: sprint/jump.
* Left stick click held 1 second: recenter. Left stick click while standing: VR dashboard
  (equipment, items, options, gestures).
* Right stick click held (R3 hold): quick-item wheel at your right hand; pick by moving the hand
  or the stick, release to use.
* Menus and the pause screen are drawn in front of you; point with a controller and pull the
  trigger.
Tuning files re-read while the game runs: out\fp.txt (first person, turning, comfort),
out\stereo.txt (stereo scale). They are created from defaults\ on the first run.

SETTINGS MENU
-------------
Before the game starts, a settings page opens (an app window in Edge / Chrome, else your browser;
a text menu if neither works). Tabs: Display / VR, DLSS / Upscaling, Comfort, Combat, Body / arms,
UI, Patches, Advanced. Profiles: Default (the old behaviour), Comfort, Performance, Quality (DLSS)
and Custom (your own values). A blue "live" badge means the option applies while the game runs (the
menu stays open: change it there); "restart" needs a new start. Tick "show this menu next time" off
to skip it; BloodborneVR.exe --menu brings it back, --no-menu skips it, --profile "Comfort" starts a
profile, --reset-settings forgets the saved choices. The page is served only on your own PC
(127.0.0.1) and protected by a one-time token. Changed control files keep a .bak of the original.
DLSS (optional, off by default; falls back to FSR 1) is NOT available in this download: it needs an NVIDIA RTX GPU,
NVIDIA's nvngx_dlss.dll and a bbport_dlss.dll bridge built against NVIDIA's DLSS SDK. NVIDIA's licence does not allow
shipping those with GPL code, and the bridge source is not in this repository yet. The renderer side is in the source
(vk_xr_dlss.*); if you build the bridge yourself, put both DLLs into the bin folder. The DLSS tab says what is missing.
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
* Co-op and online play are intentionally absent from this build.

REPORTING BUGS
--------------
Open an issue on the project page (use the "Bug report" form) and attach logs\bbvr-latest.log.
If the game crashed and you agreed to crash reports, quote the crash id the game printed.
Crash reports are opt-in: you are asked once, you can see the report on your PC first, and
you can turn it off (docs\CRASH_REPORTS.md in the source). Never post saves or game files.
bin\bb-gpu-capabilities.exe prints your GPU's Vulkan features: include its output.
