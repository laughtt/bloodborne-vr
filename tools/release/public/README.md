# Bloodborne VR (alpha)

Play your own copy of **Bloodborne** (PS4, CUSA03173, version 1.09) in VR on Windows. The game's
own PS4 x86-64 code runs natively on a shadPS4-derived Vulkan renderer; an OpenXR bridge adds
stereo rendering, head tracking, first-person view, controller-driven arms and weapons, and VR
menus.

> **Early alpha.** Single-player is the supported mode; co-op is experimental. Expect bugs and comfort issues.
>
> **No game files are included.** You need your own legally obtained, decrypted dump. This is an
> unofficial fan project, not affiliated with, endorsed or sponsored by Sony Interactive
> Entertainment, FromSoftware or any trademark holder. PlayStation, PS4 and Bloodborne are
> trademarks of their owners.

## Play (testers)

**Download `BloodborneVR-<version>-Setup.exe` from Releases, run it** (SmartScreen: the alpha is not
code-signed, so click **More info, then Run anyway**), then **Start menu, Bloodborne VR, choose your
game folder, START VR**. It installs for your user only (no admin rights); uninstalling keeps your
saves and settings. The same release has `BloodborneVR-<version>.zip`, the portable option: unzip it
anywhere with write access and double-click `BloodborneVR.exe`. The zip's `README.txt` has the install
steps, known issues and how to report bugs. The controls are summarised below.

## Controls

![Bloodborne VR controls](docs/images/controls.png)

| Controller input | In game |
|---|---|
| Left stick | Move |
| Right stick | Camera / turn (smooth or snap, see the Comfort tab) |
| Right-hand swing | Light attack (R1) |
| Swing with the right grip held, or a very hard swing | Heavy attack (R2); keep the grip held after the swing to charge |
| Quick right-grip tap (under 0.35 s, no swing) | Swap the right-hand weapon |
| Right trigger | Nothing in the world |
| Left trigger (hold) | Hold the gun (L2) |
| Left-hand thrust forward (gun held) | Shoot |
| Left grip (squeeze) | L1 (transform) |
| A (right controller) | Swap the right-hand weapon |
| B (right controller) | Roll / dodge, hold to sprint |
| X (left controller) | Swap the left-hand weapon |
| Y (left controller) | Interact (doors, pickups, ladders) |
| Heal | Pick the Blood Vial on the quick wheel, or bring it to your mouth (left hand, left grip held) |
| Right stick click (tap) | Open the item wheel: pick with your right hand (touch or point) or the stick |
| Left stick click while standing | Open the VR dashboard (equipment, items, options) |
| Left stick click while moving | Jump |
| Left stick click, held 1 s | Recenter |
| Left menu button | Options (pause); held 1 s: recenter |
| Swing at a staggered enemy | Visceral attack (stagger one with a gun parry, or a charged heavy from behind) |

Lock-on is off by default: a right stick click opens the item wheel instead.

**In the game's own menus** (Options, pause): either stick moves the cursor (hold to repeat), **A or either
trigger** confirms, **B** goes back, and the menu button closes the menu.

## Physical items

* Pick an item on the item wheel: it appears in your hand as a simple marker. Squeeze that hand's grip within 5 s to hold it; letting go without using it puts it back.
* **Vials, pills and other drinkables:** bring it to your mouth to use it.
* **Weapon buffs (paper, oil):** rub it along the blade with the other hand (the one without a weapon).
* **Molotovs, throwing knives and the like:** swing and let go of the grip to throw; the throw is aimed from your hand.
* Hits with your weapon and bounces off walls give a haptic pulse in the controller.
* The grey hand markers and the gun laser are off by default.

## Co-op: how to join (up to 3 players)

Co-op is **EXPERIMENTAL** in this alpha. It uses the default lobby that comes with this release: there is
nothing to type. Playing over the internet is not fully tested. Everyone needs game version 1.09 and this same
release. **Play solo** needs no server and no account.

**A) HOST (the player whose world the others join)**

1. Open `BloodborneVR.exe` (the dashboard).
2. On the **Play** tab press **Play online**.
3. Press **Host a game**. A room code like `K7Q-M4X` appears (it also shows in the dashboard title).
4. Send the code to your friends (Discord, text message ...). Up to 2 friends can join (3 players in total).
5. Press **Start with this room**, load your character and walk a little way into an area. Do not stand in the
   area's entry zone, and the area's boss must still be alive.
6. Nothing to press in the game: your bell rings by itself and your friends appear.

**B) FRIEND (joining)**

1. Open `BloodborneVR.exe`.
2. On the **Play** tab press **Play online**.
3. Type the room code under **Join a friend** and press **Join**.
4. Press **Start with this room**, load your character and stay in the same region as the host.
5. Nothing to press in the game: your bell rings by itself and the host summons you. If you die or get sent home,
   it rings again by itself.

**Running your own lobby?** Under **Play online** press the small **Change** next to "Lobby: default", enter your
address and press **Use**. **Use the default lobby** switches back.

**If it does not work**

- Everyone must use the same release and have game version 1.09, and the friend must type the host's room code exactly.
- The host must not stand in an area's entry zone, and that area's boss must not be dead yet.
- Wait about a minute. If you saw "failed to acquire server information", restart the game.
- The Windows firewall must allow BloodborneVR (allow it when Windows asks).
- Co-op is experimental and internet play is not fully tested.

**Leave a room / play solo:** press **Leave** (the host sees **Close room**) in the Play online card, or **Leave room**
on the **Online** tab. **Back** returns to the Play tab, where **Play solo** starts the game without a room.

## Comfort, one-eye viewing and weaker PCs

On the dashboard's **Play** tab, under the profile cards, **View** switches between **3D** and **Flat screen (mono)**, and
picking a profile card (**Default**, **Comfort**, **Performance**, **Quality (DLSS)**) saves it at once. On a PC without an
NVIDIA RTX card the Play tab also says "Low fps? Try Performance."

- **Cannot see 3D with both eyes, flicker or discomfort:** set **View** to **Flat screen (mono)** (or Options tab, Display / VR,
  **Stereo mode** = mono, or start with `BloodborneVR.exe --mono`). You get one big flat screen in the headset and the
  controllers still work. If the depth looks wrong in 3D, try **Swap eyes** (Options, switch **Advanced** on) and **Stereo scale**.
- **Motion sickness:** pick the **Comfort** profile (snap turning, locked and eased camera animation, steadier camera).
- **Weaker PC or low fps:** pick the **Performance** profile (FSR 1 at 1.5x the frame instead of 2x, lower model detail).
  You can also lower **Frame limit (fps)** and **Eye resolution** (Options tab, Display / VR and DLSS). With an NVIDIA RTX card,
  the **Quality (DLSS)** profile gives the best picture.
- **Defaults:** Stereo mode alt (true 3D), Frame limit 72 fps, FSR 1 upscaling (DLSS quality on an RTX card), eye
  resolution 2x the 1080p frame, FXAA, smooth turning (90 degrees per second), Stereo scale 0.77.

## Updating (keep your saves)

* **Setup.exe:** run the new `BloodborneVR-<version>-Setup.exe`; it installs over the old version. Your saves and settings (`%LOCALAPPDATA%\BloodborneVR\user`) are kept, and uninstalling keeps them too.
* **Zip:** extract the new zip into a new folder, then copy the old folder's `user\` folder (and `bbvr_config.json` if it exists) into it. You can also extract the new zip over the old folder (your `user\` folder is not part of the zip, so it stays). Optional: copy `out\` too to skip the first-start data build.
* **From the zip to Setup.exe:** copy the zip folder's `user\savedata` to `%LOCALAPPDATA%\BloodborneVR\user\` (create the folder if needed) before the first start of the installed version.

## What works (see `docs/vr/`)

First-person camera with head tracking, true stereo (alternate-eye and renderer stereo), arm IK
and weapons that follow the controllers, comfort dash and turning, stamina and swing feedback,
hit-shape scaling, VR menus (quick wheel, inventory, weapon wheel), hand-thrown goods in the
sandbox. Research notes and design documents are in `docs/vr/findings/`.

Setting: **Hitboxes** (dashboard, Combat): *Big* (default) enlarges your weapon's hit shapes, *Original* keeps the game's own; enemies are never changed.

## Build from source (Windows)

1. Install MSYS2 and open the **CLANG64** shell. Packages: `packaging/windows/README.md`.
2. `git clone --recursive <this repo>`, then `bash build.sh`
   (outputs in `out/`: `bb-probe.exe`, `bb-gpu-capabilities.exe`, `bbport_xr.dll`).
   `build.sh` also applies the patch series in `gpu/patches/fsr-vulkan/` to the `gpu/third_party/fsr-vulkan`
   submodule (needed; the submodules are pinned in this repository). The DLSS bridge source is in `gpu/dlss_bridge/` (MIT); it is built separately
   against NVIDIA's DLSS SDK (not included). The binary release ships the built `bbport_dlss.dll` and NVIDIA's
   `nvngx_dlss.dll` (see `THIRD_PARTY_NOTICES.md`).
3. Run with your own dump:
   `BB_GAME_DIR=/c/games/CUSA03173 python run.py` (flat) or see `tools/release/zip/bbvr_launcher.py`
   for the VR environment variables.
4. Package: `python tools/release/make_release.py` (needs the build output; run
   `python tools/release/export_public.py` first, the packager reads its output).

Unit tests are `EXCLUDE_FROM_ALL` targets in `gpu/CMakeLists.txt` and `gpu/xr_bridge/`.

## Help and bugs

Use the issue templates. Attach `logs/bbvr-latest.log` and never post saves or game files.
If the game crashes a report is saved on your PC and never sent automatically (`docs/CRASH_REPORTS.md`). Contributing: `CONTRIBUTING.md`.

## Licence

GPL-2.0-or-later (`LICENSE`); binary releases are GPLv3 because they bundle Apache-2.0 and
GPLv3 libraries (`COPYING.GPL-3.0`, `THIRD_PARTY_NOTICES.md`).

## Credits

Built on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28, the Windows port
by [Supermedo](https://github.com/Supermedo), the [shadPS4](https://github.com/shadps4-emu/shadPS4)
emulator, community patches by illusion, Kyo and others, FSR-Vulkan (FireBurn), Dear ImGui,
LibAtrac9, and the OpenXR / Khronos projects.
