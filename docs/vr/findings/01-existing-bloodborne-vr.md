# 01 — Does any Bloodborne VR already exist?

Research date: 2026-10-09. Track: prior art for Bloodborne VR and related camera/stereo work.

## Bottom line

**No true-stereo / head-tracked Bloodborne VR exists anywhere, as far as I can find.** It is not released, and I found no credible WIP or rumor either.
Everything labelled "Bloodborne VR" is one of these:
- a flat virtual screen (theater mode),
- a flat first-person mod that people say "looks like VR", or
- a failed vorpX attempt.

The most valuable prior art is not Bloodborne-specific. It is **AstroQuest**, a GPL shadPS4 fork with a working OpenXR host (PC via VDXR/SteamVR, plus Quest standalone), and **Farpoint PC VR**, which is derived from AstroQuest.
Both forks drive **PSVR games**, which already render two eyes. So they solve the OpenXR / Vulkan / pacing plumbing on a shadPS4 renderer, but not the hard part we face: making a flat game render stereo from a head-driven camera.

For the camera side, the community has already located Bloodborne v1.09 camera code. The addresses are in the public shadPS4 patch XMLs (see section 3).

## 1. Direct "Bloodborne VR" hits

| What | URL | What it actually is | Status | Source | Reusable? |
|---|---|---|---|---|---|
| "Bloodborne VR [shadPS4] [META QUEST 2]" (YouTube, Marinne, 2024-10-02) | https://www.youtube.com/watch?v=0wFaJFhX0QE | **Theater only. Verified from the thumbnail:** the Quest Link/Air Link desktop curved virtual screen showing the shadPS4 window. No stereo, no head tracking. Uses shadPS4 0.3.1. | One-off video | n/a | Nothing |
| vorpX forum thread "ShadPS4" (Apr–May 2025) | https://www.vorpx.com/forums/topic/shadps4/ | A user failed to hook shadPS4 with vorpX (all hook modes plus beta), because vorpX has no Vulkan hook. Suggestions in the thread: vorpX Desktop Viewer (theater), SuperDepth3D, Vk3DVision, or first-person mod + opentrack. None was confirmed working. No reply from the vorpX dev. | Dead end | n/a | Confirms there is no vorpX profile, and that vorpX can't hook Vulkan shadPS4 |
| Garden of Eyes "First Person Camera Mod" (Nexus #11, 2021) | https://www.nexusmods.com/bloodborne/mods/11 , https://www.uploadvr.com/bloodborne-first-person-mod/ | **Flat first-person**, PS4 eboot patch. Triggered by the "Great One Hat" item; adjusts weapon animations. Based on Zullie the Witch's DS3 first-person mod (Zullie "walked them through locating camera offsets"). Press (UploadVR, Destructoid, ConsoleCreatures "Bloodborne VR mode") called it "VR-like", but **no VR was ever made**. | Released on Nexus. Works on shadPS4 per later reports | Patch bytes, not source | Yes: shows that first-person camera placement plus head/body-hiding logic has been solved once for v1.09 |
| First-person mod for shadPS4 (Nexus #287, ~Mar 2025; again in the news May 2026) | https://www.nexusmods.com/bloodborne/mods/287 , https://www.dsogaming.com/mods/you-can-now-play-bloodborne-in-first-person-on-pc-via-shadps4/ , demo https://www.youtube.com/watch?v=PnYPLFWORwA (diegolix29) | Flat first-person, playable start to finish on shadPS4. The author wasn't confirmed: Nexus returns 403 to the fetcher, and the articles don't name one. Probably a shadPS4 patch XML. | Released | Patch file | Yes. It is the best candidate for the VR camera anchor (eye position). Its patch addresses should port 1:1 to bbport, which runs the same v1.09 eboot. **Download it and diff its patch lines.** |
| ConsoleCreatures "Bloodborne VR mode" (2021) | https://www.consolecreatures.com/bloodborne-vr-mode-is-cool/ | Just the Garden of Eyes first-person mod under a VR headline | Rumor-style headline | n/a | none |
| PSVR Cinematic Mode / PS Remote Play + Virtual Desktop | (general) | Flat screen. Depth3D can't work on a video stream (no depth buffer) | Works, theater only | n/a | none |
| Supermedo/bloodborne_pc, deadinside28/bloodborne_pc, lkat7/bbport, 4laric and Asdb24 forks | https://github.com/Supermedo/bloodborne_pc , https://github.com/deadinside28/bloodborne_pc | **No VR/stereo/OpenXR issue, PR, or README mention.** Supermedo: a search for "VR" returns zero issues, and so does "camera". deadinside28 has only rendering-bug camera issues (#42 smearing on camera rotation, #47 camera motion-vector convention, #68 sky shows a clamped copy of the game view via ConstantRing). | n/a | Source available | The renderer's motion-vector and camera-convention work (#16, #18, #47) shows the port already extracts per-frame camera/motion data for FSR/DLSS. That is a likely hook point for the view matrix (sibling tracks should check this). |
| shadPS4 upstream issue #3156 "Native VR / OpenXR" (2025-06-25) | https://github.com/shadps4-emu/shadPS4/issues/3156 | Feature request only, citing RPCS3 SBS, DolphinVR, PPSSPP VR and CitraVR. No maintainer reply, no PR. Older issue #1663 asks for PSVR game support. | Open, idle | n/a | Confirms upstream has nothing |

Searched with **no Bloodborne hit**:
- Luke Ross R.E.A.L. VR. He supports Elden Ring and Dark Souls Remastered (FromSoftware PC titles, using AER), but not Bloodborne or any emulator.
- Flat2VR: no mention found.
- 3D Vision, Helix and geo-11: geo-11 is DX11-only, and shadPS4/bbport are Vulkan.
- Depth3D/ReShade: no Bloodborne-specific report. ReShade does support Vulkan, so SuperDepth3D on shadPS4/bbport is technically possible but unreported. That is fake depth, not stereo.
- Vk3DVision (Helifax, Patreon, closed source): a Vulkan stereo wrapper that needs per-game vertex-shader work. No Bloodborne profile found.

## 2. VR forks of console emulators (prior art for the method)

| Project | URL | What it does | Status / license | Reusable for us |
|---|---|---|---|---|
| **AstroQuest** (bigmak94) | https://github.com/bigmak94/AstroQuest (see `README-PC-VR.md`) | shadPS4 fork running ASTRO BOT (PSVR) in real VR. Details below the table. | WIP, v0.20, about 208 stars. **GPL-2.0-or-later** | **High.** The OpenXR host, the Vulkan device/instance extension negotiation, swapchain copy, pose prediction, session-loss handling and Touch action set are all directly portable to bbport's shadPS4-derived renderer. Missing for us: a game-side stereo/camera injection, because PSVR titles render both eyes themselves. |
| **Farpoint PC VR** (evertec82) | https://github.com/evertec82/Farpoint | AstroQuest-derived OpenXR port of Farpoint (PSVR). Tracked controllers, supersampling up to 3072x3456 per eye, "full-refresh frame pacing". Known issues: rainbow textures, stretched geometry, crashes. | WIP dev port. GPL-2.0 | Medium. A second example of the same host, and shows AstroQuest's host is reusable across games. |
| **hookmanuk/rpcs3** "RPCS3 VR" | https://github.com/hookmanuk/rpcs3/releases/tag/v0.0.42-vr4 | RPCS3 fork with per-game VR profiles. Details below the table. | Released, active-ish. RPCS3 is GPL-2.0 | **High, conceptually.** It is the closest analogue to what we want: a flat FromSoftware game (Demon's Souls, the Bloodborne predecessor, same studio lineage) made VR in an emulator. Study its Demon's Souls patches and source to see how it injects head pose into the game camera and how it handles FOV, motion blur, DoF and cutscenes. |
| RPCS3 upstream | — | 3D stereo output (anaglyph/SBS) only for PS3 titles that support stereo natively | Released | Low |
| PPSSPP OpenXR stereo (lvonasek) | https://github.com/hrydgard/ppsspp/pull/15901 | Upstream OpenXR in PPSSPP, with stereo made by rendering twice with an offset view matrix | Merged | Medium as a reference for the "draw twice with an eye offset" technique |

**AstroQuest details:**
- In-process `src/core/vr/openxr_host.cpp` thread ("shadPS4:XrHost").
- Shares the emulator's Vulkan device, and enables the runtime's required extensions in `vk_platform.cpp` and `vk_instance.cpp`.
- The game renders both eyes side by side (PSVR). The SBS image is copied into an OpenXR swapchain and submitted as a projection layer carrying the pose it was rendered with, so the runtime compositor reprojects it.
- 60 fps game at 120 Hz.
- Head pose is predicted `predict_ms` = 20 ms ahead.
- FOV comes from the runtime.
- Touch controllers are mapped through `GameController::ApplyRemoteState`.
- Headset removal triggers a pause via `sceSystemServiceGetStatus`.
- Works with Virtual Desktop VDXR and SteamVR. Pimax Dream Air was reported working.

**RPCS3 VR fork, Demon's Souls profile details:**
- Demon's Souls (BLUS30443) has a VR profile.
- Frame rate follows the headset refresh, with an unlocked-fps patch; the game slows below 20 fps.
- Motion blur is disabled because it "followed the game camera rather than your head".
- "Wider view" FOV patch, 130 degrees by default.
- DoF disabled.
- Cutscene camera interpolation.
- Pre-rendered videos are shown on a fixed screen.
- The release notes don't describe the stereo method or the pose-injection mechanism, so that is unknown until the source is read.

## 3. Camera / free-cam / FOV work that already locates camera code (v1.09 eboot)

All of these are public, from https://github.com/shadps4-emu/ps4_cheats (PATCHES/Bloodborne.xml). shadPS4 patch addresses for eboot.bin v1.09 apply to the same eboot that bbport loads.

- **"Increased camera distance"** (Kyo). Hooks the code at `0x0183AE68` with a jump into a code cave at `0x04D3BC30`, which multiplies a float. It also changes float constants at `0x0183A47D/A87B/A487/A885/A491/A88F`. This is very likely the same patch as Supermedo's "camera distance" cheat. **The player-camera (ChrCam/follow-cam) update function lives around 0x0183A000–0x0183E000.**
- **"Disable Camera Auto Rotation via Movement"** (Imedved, Kyo). NOPs at `0x0183C6E8`, `0x0183C984`, `0x0183C870` and `0x0183DDE6`, in the same function cluster. Useful for VR: the game should never yaw the camera by itself.
- **"Restore Debug Camera"** (Lance McDonald, 2023). The free camera was restored. It was later removed from the current XML, probably after the Jan 2025 Sony DMCA against his patches, but it is still in git history at commit `8419e97f`. Patch bytes are at `0x01972998..0x019729BA` and `0x01DE3328..`. Hold X and press L3 to cycle free-cam modes. This is direct evidence that a full debug free camera exists in the binary and can be driven. **The free-camera object is a strong candidate for injecting head pose** (its position and orientation are written by input code we could replace with HMD pose).
- **"Restore Debug Menu"** (Whitehawkx, auser1337). Gives a debug-menu path. Older PS4-only work in the same vein: BBDBG https://github.com/stagvant/BBDBG (overworld debug camera, archived 2023).
- **FOV:** I found no standalone Bloodborne FOV patch. The vorpX thread mentions a "Nexus mod 76" for FOV increase, which I couldn't verify. "Move camera with mouse and change FOV" (Nexus #193) exists, but its page returned 403, so it's unverified and may be a shadPS4 patch too. Worth fetching manually: it would reveal the projection or FOV float.
- **Lance McDonald's 60 fps patch:** it is the basis of the FPS++ patches (30/60/90/Uncap) in the same XML, which Supermedo ships. Not camera-related.

Caveat: verify how the shadPS4 patch "Address" maps to bbport's load base before reuse. The values are likely offsets relative to the eboot image base.

## 4. Verified vs rumor

**Verified:**
- The Quest video is theater only.
- vorpX doesn't hook shadPS4.
- There is no VR in Supermedo/bbport or upstream shadPS4.
- AstroQuest and Farpoint are real GPL shadPS4 OpenXR forks (PSVR titles only).
- RPCS3 VR fork with Demon's Souls exists.
- Camera patch addresses are as listed above.

**Rumor / unverified:**
- Any "Bloodborne VR mod" headline. It always resolves to the 2021 flat first-person mod.
- The author and mechanism of Nexus #287.
- The Nexus FOV mods (#76, #193).

## Recommendations for the plan

1. Base the OpenXR side on AstroQuest's `openxr_host.cpp` pattern. GPL-2.0 matches shadPS4/bbport, so the license is compatible. Check Supermedo's own license and bundling.
2. Base the game side (stereo plus head pose) on:
   a. the existing camera-function cluster at 0x0183Axxx (Kyo's patches),
   b. Lance's debug free camera as a pose-injection target,
   c. the Nexus #287 first-person patch for the eye anchor.
   Use the RPCS3 Demon's Souls VR profile as the design checklist: disable motion blur and DoF, wider FOV, cutscene handling, unlocked fps.
3. The Bloodborne-specific stereo path is greenfield. Nobody has done render-twice or AER for this game.

## Cross-links

Against `04-flat2vr-tech.md`:

- **Contradiction.** Its emulator table says "RPCS3 / ... VR: no maintained VR forks found". **That is wrong for RPCS3.** hookmanuk/rpcs3 ("RPCS3 VR", release v0.0.42-vr4, https://github.com/hookmanuk/rpcs3/releases/tag/v0.0.42-vr4) exists, with a Demon's Souls VR profile.
  - This undercuts 04's argument that programmable-shader consoles have no VR forks because their matrices are buried in constant buffers.
  - Both hookmanuk/rpcs3 and AstroQuest (a PS4 fork) are counter-examples. AstroQuest, however, only handles PSVR games, which already render two eyes.
  - Worth reading hookmanuk's source to see whether it patches game camera code or rewrites constant buffers.
- **Confirms.** 04's recommended primary path, hooking the native camera/projection functions, fits our finding that the camera-update cluster at eboot `0x0183A000-0x0183E000` and Lance's debug free camera (`0x01972998`, `0x01DE3328`) are already located by the community.
- **Confirms.** 04's note that motion blur and TAA ghost under per-eye rendering matches the RPCS3 VR Demon's Souls profile, which disables motion blur and DoF by default.
- **Complements.** 04's OpenXR frame-loop notes line up with AstroQuest's real implementation (`src/core/vr/openxr_host.cpp`): the shared Vulkan device, a projection layer submitted with its render pose, and a 20 ms predicted pose. AstroQuest is GPL-2.0-or-later, so it is a license-compatible source to copy from, unlike CitraVR (GPL-3) or UEVR (all rights reserved).
