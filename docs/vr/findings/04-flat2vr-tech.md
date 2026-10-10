# 04 — Flat-to-VR technology landscape (what to study / reuse)

Track owner: research agent 4. Date: 2026-10-09.
Legend: **[V]** = verified this session (GitHub API / source read), **[K]** = prior knowledge, not re-verified, **[?]** = uncertain / community lore.

Context for our target: `bloodborne_pc` = native x86-64 game code + a shadPS4-derived Vulkan renderer (PS4 GCN shaders recompiled to SPIR-V). shadPS4 is GPL-2.0(-or-later) **[K]** — so anything we *link in* must be GPL-2-compatible (MIT / BSD / Apache-2.0 ok-ish; Apache-2.0 is GPL-3-compatible, GPL-2 compat is debated — keep Apache code as a separate loader/DLL, which the OpenXR loader already is). GPL-3-only code (CitraVR) cannot be pasted into a GPL-2-only tree.

---

## 1. praydog's UEVR (Unreal Engine)

- URL: https://github.com/praydog/UEVR — **License: "Copyright (c) 2022-2025 praydog. All rights reserved."** [V] (GitHub reports NOASSERTION; the LICENSE file is all-rights-reserved). => **Study only, do not copy code.** Active (pushed 2026-08) [V].
- Rendering modes [V, README]:
  - **Native Stereo** — flips UE's own built-in stereo path on (IStereoRendering / `FFakeStereoRendering`-style hooks), engine renders both views in one frame. Best perf, can crash/bug.
  - **Synchronized Sequential** — renders the frame twice on the *same engine tick* (game time does not advance between eyes). Fixes most Native Stereo bugs; temporal effects (TAA, motion blur) ghost because history is shared between eyes, so users disable motion blur / switch TAA. This is the closest analog to "replay the frame twice" for us.
  - **AFR** — one eye per engine frame, time advances between eyes => eye desync; last resort.
- Architecture [K]: injected DLL; hooks D3D11/D3D12 Present + game's viewport/scene-view functions found by pattern scan over UE reflection (UObject/FProperty walking at runtime, no SDK needed). Copies each eye's backbuffer into OpenVR textures or OpenXR swapchains (XR_KHR_D3D11_enable / XR_KHR_D3D12_enable); for D3D12 it records its own copy command lists. Two runtime backends behind one abstraction (OpenVR + OpenXR), chosen at injection.
- UI [V/K]: captures UMG/Slate UI into a separate render target and presents it as a world-space quad (OpenXR quad layer / OpenVR overlay) — "automatically handles most UI and projects it into 3D space".
- Input [K]: maps OpenXR/OpenVR actions to a virtual XInput gamepad (hooks XInputGetState), plus motion-controller pose exposure to plugins.
- Plugin API [V]: C++ plugin SDK (`include/`), Lua API (`lua-api/`), Blueprint integration; `vr-plugin-nullifier` neutralises a game's own VR plugin.
- Reusable for us: **design ideas only** — the three-mode taxonomy, the "same tick, two renders" Synchronized Sequential concept, UI-to-quad split, gamepad emulation, plugin surface.

## 2. praydog's REFramework (RE Engine) — closest structural analog

- URL: https://github.com/praydog/REFramework — **MIT** [V], active (2026-09).
- Custom engine, no built-in stereo, so it uses **AFR** with per-eye camera injection via hooked engine camera/projection functions, D3D11/D3D12 hooks, OpenVR + OpenXR backends, ImGui overlay, Lua scripting, motion-controller/weapon aiming, and per-eye TAA/history fix-ups [K + V via vrframework citations].
- **Big find: https://github.com/elliotttate/vrframework — MIT** [V] (derivative of REFramework). A ~6,000-line, 17-part guide plus an *engine-agnostic* core scaffold: injection, D3D hooks, ImGui overlay, VR runtime abstraction (OpenVR/OpenXR), frame timing, and an SPI `IEngineAdapter` / `FrameTimeline` / `StereoView` (`view[2]`, `projection[2]`, `presenter_frame`). Studies REFramework, starfield2vr (Creation Engine 2) and anvilengine2vr (Ubisoft Anvil) with ~180 file:line citations. Directly relevant guides:
  - 08 stereo strategies: all three reference mods chose AFR because re-entering a closed render pipeline twice per frame trips per-frame state (descriptor heaps, transient allocators). Projection built from runtime frustum tangents with asymmetric skew terms; "log the engine's own projection first and match its handedness/depth convention (reversed-Z!)".
  - Injection site families: (a) hook a function with an out-param matrix (Anvil `onCalcProjection`, filter main camera via `farPlane > 1201`), (b) patch the **camera constant buffer** at RE'd offsets (Creation 2) — the latter is exactly what shadPS4 would do.
  - 10 TAA: under AFR, double-buffer **every** cross-frame resource and matrix by eye parity (TAA colour history, depth history, motion vectors, prev-view-proj constant, exposure). Creation 2 keeps `pastProjections` per parity; Anvil keys `copies[]` on `frame % 2`.
  - 07 frame timing: Reflex markers vs two-hook pacing.
- Reusable: vrframework core (MIT) is a candidate base for our "VR layer" — but it is D3D-centric; we need a Vulkan path. Its SPI shape is a good template for our library's engine-adapter boundary.

## 3. Luke Ross R.E.A.L. VR (closed, Patreon)

- URL: https://www.patreon.com/LukeRoss (no source). [K]
- **AER (Alternate Eye Rendering)**: renders one eye per frame alternately; the eye not rendered this frame is shown from its last image reprojected (headset-pose + depth-based), so each eye updates at half rate. Visible side effect: ghosting/double edges on fast motion [V, mixed-news].
- **AER v2** (Mar 2023): improved reconstruction with less ghosting; v19 added a GPU-heavier optical-flow estimator (off by default) [V]. Later added DLSS Ray Reconstruction compatibility [V].
- "Synchronous"/"full stereo" mode per-game: [?] — community reports some titles offer rendering both eyes per tick at higher cost; not confirmed from public docs.
- Engine-specific per game (RAGE, REDengine, Kingdom Come etc.), hooks the game camera directly (not shader-level).
- Reusable: the *idea* that AFR judder can be hidden by reprojecting the stale eye with depth + optical flow. For us, a cheaper equivalent is to submit depth (XR_KHR_composition_layer_depth) and let Meta ASW / runtime handle it, or do our own per-eye reprojection.

## 4. VorpX (closed, commercial)

- URL: https://www.vorpx.com — closed, paid. [K]
- **Geometry 3D**: true stereo — renders each frame twice with per-eye camera modification done at the shader/constant level (works in DX9–DX12 with per-game profiles). Best quality, ~2x GPU cost, frequent shadow/post-effect misalignment needing per-game shader fixes.
- **Z3D / Z-Adaptive / Z-Normal**: single-render, depth-buffer-based reprojection to synthesize the two eyes (cheap, no double draw). Artifacts: disocclusion halos at edges, flat-looking transparencies/particles, HUD gets depth from whatever is underneath. Z-Normal is the cheapest, flattest variant [?].
- **DirectVR**: per-game memory hooks so headset rotation drives the game camera directly and FOV is set to match the HMD, rather than emulating mouse input. Also "EdgePeek" / virtual cinema modes for flat fallback.
- Reusable: nothing as code; the UX layering (full-3D -> depth-3D -> cinema fallback) is a good fallback ladder for a generic VR layer.

## 5. Depth-based stereo (post-process)

- **SuperDepth3D / SuperDepth3D_VR** (ReShade): https://github.com/BlueSkyDefender/Depth3D — **proprietary ("Copyright Depth3D – All Rights Reserved", no redistribution)** [V, shader header]. Parallax-occlusion-style displacement from the depth buffer to make SBS images; VR variant pairs with VRto3D / Virtual Desktop SBS. Study only.
- **VRto3D**: https://github.com/oneup03/VRto3D — **LGPL-3.0** [V], active. An OpenVR (SteamVR) *driver* that presents SBS/TaB 3D output from games as a headset — the reverse direction of what we need, but its SteamVR driver plumbing is a clean reference.
- **3Dmigoto** (https://github.com/bo3b/3Dmigoto — GPL, see `LICENSE.GPL.txt` [V]): DX11 wrapper that decompiles/patches HLSL shaders and edits constant buffers to fix stereo (originally on top of NVIDIA 3D Vision automatic stereo). The core technique: in each vertex shader, after clip-space position is computed, apply the **NVIDIA stereo formula** `pos.x += eye * separation * (pos.w - convergence)` — this gives stereo **without knowing any view/projection matrix**. Then hand-fix screen-space effects (shadows, SSR, deferred lighting, volumetrics) that reconstruct world position from depth via an inverse projection (they need the same per-eye correction applied in the pixel shader). Helix Mod (DX9, closed) and **geo-11** (3Dmigoto lineage, free DX11/12 geometry stereo usable with VR headsets via SBS) [?] are the same family.
- Tradeoff summary: depth-reprojection is cheap and generic but low quality (halos, wrong transparencies, no true parallax behind objects, no proper per-eye FOV/asymmetric frusta); geometry stereo is correct but needs every screen-space effect fixed per game.
- Reusable for us: the **clip-space stereo formula injected into recompiled SPIR-V vertex shaders** is a viable "zero RE" bootstrap in shadPS4 (we control the GCN->SPIR-V recompiler, so we can append the formula to every vertex shader's position export). Quality ceiling is lower than proper per-eye matrices (no asymmetric FOV, separation tuned by hand), and post effects need fixing.

## 6. Emulator VR precedent (closest analog to "replay PS4 draws twice")

| Project | URL | License | How it gets stereo | Notes |
|---|---|---|---|---|
| **PPSSPP OpenXR** (lvonasek, mainline) | https://github.com/hrydgard/ppsspp `Common/VR/` | GPL-2.0+ [V: NOASSERTION on API, repo is GPL-2+] [K] | PSP GE has *explicit* projection/view/world matrix registers. `PPSSPPVR.cpp` [V] replaces the game projection with one built from OpenXR fov tangents (`UpdateVRProjection`), left-multiplies HMD view onto game view per eye (`hmdView * gameView`), detects 2D/HUD draws by projection heuristics (`Is2DVRObject`: ortho, huge scale, identity), mirrors axes from projection sign. Per-game hacks in `assets/compatvr.ini` (ForceFlatScreen, ProjectionHack, IdentityViewHack, ForceMono). Stereo via **multiview** in the shader generator (VertexShaderGenerator / Vulkan pipeline use view index). Flat "screen" mode with reprojection on, immersive mode with reprojection off. | Best open, readable, GPL-2-compatible reference for OpenXR loop + matrix substitution in an emulator. Mostly Android/Quest-standalone (GLES), Vulkan path exists. |
| **Dolphin VR** (Carl Kenner fork) | https://github.com/CarlKenner/dolphin | GPL-2.0 [V], inactive (2023) | GameCube/Wii fixed-function XF projection registers: detect perspective vs ortho projection, replace FOV, add per-eye offset in the vertex shader, push ortho (HUD) draws to a virtual screen depth; per-game "VR hacks" (skip draws by projection/viewport, scale, camera forward). Used Oculus SDK => GPL licensing conflict, Dolphin devs disowned it [V]. | Mainline Dolphin has its own stereoscopy (SBS/TaB/anaglyph) done **single-pass** via geometry-shader layering with a per-eye offset applied in the vertex/geometry stage — chosen because emulators are draw-call bound [V forum]. |
| **CitraVR** (amwatson) | https://github.com/amwatson/CitraVR | **GPL-3.0** [V], active | 3DS games already render native stereo (two eyes), so it just maps both eyes to OpenXR layers on Quest. | Useful for OpenXR/Quest plumbing; license incompatible with GPL-2-only. |
| Mario Galaxy VR (MCassador) | https://github.com/MCassador/SuperMarioGalaxy-VR | none listed [V] | Game-specific Dolphin-based VR | Shows "per-game emulator fork" pattern. |
| RPCS3 / PCSX2 / Cemu / Ryujinx / yuzu / Xenia VR | — | — | No maintained VR forks found [V search]; PCSX2 has had stereo-hack discussions only [K]. | Programmable-shader consoles have no matrix registers, which is why they lack VR forks — matrices are buried in constant buffers. |

**Key lesson for PS4/shadPS4:** PSP/GC VR works because matrices are in fixed-function registers. PS4 (like PS3/Switch/Xbox) passes matrices in shader constant buffers (V# buffer descriptors pointed to by user-data SGPRs). Options, in increasing quality:
1. Clip-space stereo formula appended to every recompiled vertex shader (3Dmigoto-style, no matrix knowledge).
2. Identify the game's camera constant buffer layout (RenderDoc capture of shadPS4 + pattern of a 4x4 with perspective signature, à la PPSSPP heuristics and Creation-2 CB patching) and rewrite it per eye at upload/bind time.
3. Since bloodborne_pc has **native x86-64 game code**, hook the game's camera/projection functions directly (REFramework/Anvil out-param style) — highest quality, enables per-eye asymmetric frusta, HUD separation and motion controls. This is the recommended primary path; (1)/(2) are fallbacks for draws we cannot attribute.

## 7. OpenXR on Vulkan — practical notes

Spec: https://registry.khronos.org/OpenXR/ ; loader + samples: https://github.com/KhronosGroup/OpenXR-SDK-Source (**Apache-2.0** [V]); good small samples: OpenXR-SDK-Source `hello_xr` (Vulkan plugin), https://github.com/jherico/OpenXR-Samples [K].

- **XR_KHR_vulkan_enable2**: `xrGetVulkanGraphicsRequirements2KHR` -> `xrCreateVulkanInstanceKHR` (runtime adds its instance extensions) -> `xrGetVulkanGraphicsDevice2KHR` (runtime picks the physical device — must be the GPU the HMD is attached to) -> `xrCreateVulkanDeviceKHR`. Implication for shadPS4: its Vulkan instance/device creation must be routed through these calls (or use vulkan_enable v1 to *query* extension strings and add them yourself). Plan an init-order refactor: OpenXR instance+system first, then renderer.
- **Session binding**: `XrGraphicsBindingVulkan2KHR {instance, physicalDevice, device, queueFamilyIndex, queueIndex}`. The runtime submits on **that VkQueue** inside `xrEndFrame`/`xrReleaseSwapchainImage` — Vulkan queues require external synchronization, so we must lock the same mutex shadPS4 uses around `vkQueueSubmit`/present, or give OpenXR a dedicated queue. Classic crash source.
- **Swapchains**: one array swapchain (`arraySize=2`, for multiview/layered rendering) or two swapchains; pick from `xrEnumerateSwapchainFormats` (runtimes prefer sRGB: `VK_FORMAT_R8G8B8A8_SRGB`/`B8G8R8A8_SRGB`; beware gamma double-application if the game's final pass already outputs sRGB-encoded values into UNORM — create a UNORM view or blit with conversion). Images must be in `COLOR_ATTACHMENT_OPTIMAL` when released. Easiest integration: keep the game rendering to its own images and **blit/copy into the swapchain image** after the eye's final pass (what UEVR/REFramework do on D3D).
- **Frame loop**: `xrWaitFrame` (blocks for pacing; run on render thread or a dedicated pacing thread) -> `xrBeginFrame` -> `xrLocateViews(predictedDisplayTime)` -> render -> `xrEndFrame` with layers. Use predicted display time for the camera pose actually rendered (store pose per eye with the frame for correct reprojection).
- **HUD/menus**: `XrCompositionLayerQuad` (core) — separate swapchain for the 2D UI, compositor samples it directly => crisper text, stays stable during reprojection. `XR_KHR_composition_layer_cylinder` for curved menus. Requires separating UI draws (render-target / ortho-projection heuristic as in PPSSPP `Is2DVRObject`).
- **Depth submission**: `XR_KHR_composition_layer_depth` — chain `XrCompositionLayerDepthInfoKHR` (depth swapchain, minDepth/maxDepth, nearZ/farZ; reversed-Z expressed by nearZ > farZ) onto the projection layer views. Meta PC runtime uses it for positional ASW (big win under Link) [K]; SteamVR accepts it but its motion smoothing is mostly depth-agnostic [?]; VDXR forwards to its own SSW [?]. Cheap to add, do it.
- **Motion vectors**: `XR_FB_space_warp` (AppSW) is Quest-standalone only, not available over Link/VD on PC [K].
- **Foveated / quad views**: `XR_VARJO_quad_views` + `XR_VARJO_foveated_rendering` (4 views: 2 wide low-res + 2 eye-tracked inset hi-res). Pimax Crystal: Pimax's OpenXR runtime supports quad views + eye tracking [K/?]; Quest Pro: available via **Quad-Views-Foveated** API layer (https://github.com/mbucchia/Quad-Views-Foveated, **MIT** [V], low activity since 2025-02) which emulates quad views on top of any runtime and uses Meta/VDXR eye tracking. Eye tracking extensions: `XR_EXT_eye_gaze_interaction` (VDXR, Pimax), `XR_FB_eye_tracking_social` (Meta Link, needs dev toggle). For a shadPS4 game the cheap version is fixed/eye-tracked foveation via `VK_KHR_fragment_shading_rate` / `XR_FB_foveation` (Meta) rather than true 4-view rendering, because 4 views = 4 game renders.
- **API layers**: OpenXR Toolkit (https://github.com/mbucchia/OpenXR-Toolkit, **MIT** [V], maintenance mode) — upscaling (FSR/NIS), fixed/eye-tracked foveated rendering via VRS, overlays; it hooks D3D11/D3D12 apps (Vulkan support limited [K]) — study its layer skeleton and VRS foveation code. An API layer is also an option for shipping our own "VR bridge" as a layer rather than a linked library.
- **Runtime differences** (PC):
  - *Meta Quest Link / Air Link* — Meta PC OpenXR runtime; good D3D11/12, Vulkan supported; ASW with depth; eye/face tracking for Quest Pro behind dev settings; supports quad layers, depth layer.
  - *Virtual Desktop* — **VDXR** (https://github.com/mbucchia/VirtualDesktop-OpenXR, **MIT** [V], active 2026-10) is VD's own OpenXR runtime, bypassing SteamVR; lowest latency/overhead, exposes eye tracking + quad views on Quest Pro, uses VD's SSW. Readable reference implementation of an OpenXR runtime (helpful for debugging what we submit).
  - *SteamVR (incl. Steam Link for Quest, Pimax via SteamVR)* — OpenXR via SteamVR runtime; Vulkan works but historically had extra copy/format quirks; Motion Smoothing instead of ASW; supports OpenVR fallback for older games.
  - *Pimax* — Pimax Play exposes its own OpenXR runtime (native, quad views on Crystal) or route through SteamVR; wide FOV => canted displays (non-parallel view orientations) — **never assume parallel eye frusta**, always use `xrLocateViews` poses+fovs per eye (Pimax "parallel projections" compatibility mode exists for games that get this wrong).
  - Active runtime is selected by the `ActiveRuntime` registry key / `XR_RUNTIME_JSON` env var; test against all three.

---

## Recommended building blocks for a reusable VR bridge library (ranked)

1. **OpenXR core + Vulkan backend of our own (license-clean)**, built on the Khronos loader (Apache-2.0) with hello_xr's Vulkan plugin as the template. Expose: init-order hooks (`xrCreateVulkanInstanceKHR/DeviceKHR` wrappers so the host renderer's instance/device go through OpenXR), a queue-lock callback, per-eye swapchain blit API (`submitEye(eye, VkImage, layout, rect)`), quad-layer API for HUD, optional depth API (`XR_KHR_composition_layer_depth`), predicted pose/fov getters. Add a D3D11/D3D12 backend later for other flat games.
2. **Engine-adapter SPI modeled on vrframework (MIT)**: `IEngineAdapter` / `FrameTimeline` / `StereoView{view[2], proj[2], frame parity}`; modes = Synchronized Sequential (two renders same tick, preferred for us since we own the renderer and the game logic) / AFR (fallback) / Native multiview (future). Borrow its parity-keyed history double-buffering pattern for TAA/motion vectors/prev-VP constants.
3. **Projection/camera math module**: asymmetric projection from OpenXR fov tangents matched to the host's convention (handedness, row/column-major, reversed-Z, 0..1 depth); world-scale, recenter, snap/smooth turn, decoupled pitch. Reference: PPSSPP `UpdateVRProjection/UpdateVRView` (GPL-2+) and vrframework guide 08/09.
4. **Renderer-side stereo injection for shadPS4**: (a) game-code camera hook (primary, native x86 code), (b) constant-buffer matrix patch at bind time, (c) fallback clip-space stereo formula appended to recompiled SPIR-V vertex shaders (3Dmigoto technique) — all behind one "stereo injector" interface, plus a per-draw classifier (world / HUD / post / shadow) using PPSSPP-style projection heuristics and render-target identity.
5. **HUD/UI separation -> quad layer** (UEVR/PPSSPP pattern): redirect identified UI draws into an offscreen RT, submit as `XrCompositionLayerQuad`; fallback = render UI into both eyes at fixed depth.
6. **Input layer**: OpenXR action sets with a virtual-gamepad mapping (UEVR/REFramework style) first, motion-controller aiming/weapon pose as an opt-in per game; binding profiles for Touch, Index, Pimax/Vive controllers.
7. **Per-game compat/config DB** (PPSSPP `compatvr.ini` pattern): ForceMono, projection hacks, draw skips, UI-RT IDs, TAA policy, world scale.
8. **Optional extras, later**: depth-reprojection/AER-style stale-eye reconstruction (Luke Ross idea) if Synchronized Sequential is too slow; foveation via `VK_KHR_fragment_shading_rate` + eye gaze; quad views only for Pimax/Quest Pro power users (study Quad-Views-Foveated, OpenXR-Toolkit).

**Do not copy code from**: UEVR (all rights reserved), SuperDepth3D (proprietary), VorpX / R.E.A.L. (closed). CitraVR is GPL-3 (ideas only for a GPL-2-only tree). OK to reuse with attribution: REFramework + vrframework (MIT), VDXR / OpenXR-Toolkit / Quad-Views-Foveated (MIT), OpenXR SDK (Apache-2.0), PPSSPP VR (GPL-2+, compatible with shadPS4's GPL), 3Dmigoto (GPL, check version), VRto3D (LGPL-3).

## Cross-links

No sibling findings files existed in `bbvr/findings/` at time of writing (checked 2026-10-09). Expected siblings (01–03, 05) likely cover: the bloodborne_pc / shadPS4 renderer internals (where to hook the frame, how GCN shaders are recompiled -> relevant to section 6 options 1–2 and building block 4), the game's camera/x86 code (-> option 3), and runtime/hardware testing (-> section 7 runtime differences). Readers should reconcile the "Synchronized Sequential vs AFR" choice here with the renderer-internals agent's findings on whether shadPS4 can safely replay a frame's command stream twice.

Sources: https://github.com/praydog/UEVR , https://github.com/praydog/REFramework , https://github.com/elliotttate/vrframework , https://github.com/hrydgard/ppsspp/tree/master/Common/VR , https://www.ppsspp.org/docs/reference/vr-apk , https://emulation.gametechwiki.com/index.php/Dolphin_VR , https://forums.dolphin-emu.org/Thread-stereoscopic-3d , https://github.com/amwatson/CitraVR , https://github.com/bo3b/3Dmigoto , https://github.com/BlueSkyDefender/Depth3D , https://github.com/oneup03/VRto3D , https://github.com/mbucchia/VirtualDesktop-OpenXR , https://github.com/mbucchia/Quad-Views-Foveated , https://github.com/mbucchia/OpenXR-Toolkit , https://github.com/KhronosGroup/OpenXR-SDK-Source , https://mixed-news.com/en/real-vr-mod-dlss-ray-reconstruction/ , https://mixed-news.com/en/kingdom-come-deliverance-ii-vr-mod/
