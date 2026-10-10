# 37. NVIDIA DLSS for the VR eye images (per-eye temporal, alternate-eye stereo)

Branch `dlss-vr`. Default **off**. `BB_XR_UPSCALE=dlss` turns it on. When DLSS cannot run, the
eye images fall back to FSR 1 + FXAA (findings/30) and the log says why.

Legend: **[T]** unit test (`xr-dlss-test`), **[S]** headless smoke test on the real driver
(`dlss-bridge-smoke`), **[C]** read in code, **[U]** untested in the headset or the running game.

## 0. SDK

- **NVIDIA DLSS SDK 310.9.1** (tag `v310.9.1`, 2026-09-08), from the official repository
  https://github.com/NVIDIA/DLSS. I fetched only `include/`, `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib`,
  `lib/Windows_x86_64/rel/nvngx_dlss.dll` and `LICENSE.txt` into `C:\bloodborne_vr_deps\dlss-sdk-310.9.1\`,
  outside the repo. `nvngx_dlss.dll` has a valid Authenticode signature (NVIDIA Corporation) and file
  version 310.9.1.0. Nothing from the SDK is in git.
- **"DLSS 5":** NVIDIA's official repository has no SDK by that name. Its releases go 310.1 to 310.9.1
  ("DLSS 4 / 4.5": the transformer presets J/K, and L/M added in 310.5.0). A "DLSS 5 manager" or setup
  package from other sources is not NVIDIA's and was not used or looked at. This branch uses the
  newest official SDK, 310.9.1.
- Super Resolution only. No Frame Generation (Streamline) and no Ray Reconstruction.

## 1. Structure (licences kept apart)

| Part | Licence | What |
|---|---|---|
| `gpu/dlss_bridge/bbport_dlss_bridge.{h,cpp}` -> `bbport_dlss.dll` | MIT | the only code that includes the NGX headers and links `nvsdk_ngx_s.lib`; built with MSVC. **ABI 2**: feature slots (0 = the flat game's, 1/2 = the VR eyes), each an independent NGX feature with its own parameters and history. The ABI 1 members keep their place, so ABI 1 callers still work |
| `gpu/dlss_bridge/bbport_dlss_smoke.cpp` | MIT | headless test of a built bridge (section 8) |
| `renderer_vulkan/vk_dlss.{h,cpp}` | GPL | loads the bridge at run time (unchanged loader). It now has slot arguments (default 0) and accepts bridge ABI 1 or 2. VR needs 2 |
| `renderer_vulkan/vk_xr_dlss.{h,cpp}` | GPL | the VR path: per-eye features, jitter, motion vectors, fallback |
| `renderer_vulkan/xr_dlss_logic.h` | GPL | the pure logic (options, sizes, fallback, per-eye bookkeeping, MV math); `tests/test_xr_dlss.cpp` |

The GPL sources contain no NVIDIA code and no NGX header. They talk to `bbport_dlss.dll` through its
C interface, as the flat DLSS path (commit 9514b19) already did.

## 2. Where DLSS runs in the frame

`Presenter::PrepareFrame` (at the flip of a finished game frame), in place of the FSR 1 upscale:

```
game: scene (jittered) -> lighting -> post/tonemap -> display buffer (LDR, sRGB-encoded RGBA8)
                                                       \-> HUD: separate layer (findings/25)
PrepareFrame: [VR DLSS: display buffer + scene depth + camera MVs -> eye image size] -> pp_pass -> frame image -> eye swapchain
```

- **Input:** the display buffer, which is the game's finished frame. It is tonemapped LDR with the
  game's post-processing applied. With the HUD layer on (the default in VR), the UI is not in it. The
  input is sampled through an UNORM view of the sRGB-encoded values (perceptual space, as DLSS wants
  for LDR), and DLSS writes the same encoding. The presenter then samples the output through an sRGB
  view, exactly as it samples the display buffer.
- **Exposure / HDR:** no `IsHDR` flag, no auto-exposure, pre-exposure 1. HDR (10-bit) output frames
  skip DLSS.
- **Why after tonemap and not on the HDR scene, as the flat path does:** the eye image size is
  decided here (the swapchain is sized from the frame), and the game's post/UI chain stays at the
  guest size. The post effects that are left (bloom, tonemap) are jittered with the scene, which DLSS
  handles. Motion blur, DoF and chromatic aberration are off in the VR patches. The game's own AA is
  off too (the orange quad, findings/30 §2), and DLSS replaces it.
- **HUD:** if the HUD is not separated (`BB_XR_HUD` off), the log warns that it is in the eye image and
  gets camera motion vectors, so it may ghost.

## 3. Temporal state per eye (alternate eyes, `BB_XR_MODE=alt`)

The game renders L R L R. One DLSS history over that sequence would treat the other eye's image, 64
mm away, as the previous frame. So:

- **One feature per eye** (bridge slots 1 and 2). A frame goes to the feature of the eye it is paired
  with (`xrstereo::Decision`, the same pairing that hands it to that eye's swapchain). Each feature
  sees only its own eye's frames, so its history is that eye's previous frame, two game frames back.
  [T, S: two features evaluated with different inputs, each output matches its own input]
- **Held frames** (shown to no eye) leave both histories alone. **Mono / menu / no-camera frames**
  mark both histories stale. A history is also reset after a frame gap (more than 4 game frames since
  that eye's last frame), a camera cut (more than 1.5 m or about 45 degrees between that eye's frames),
  a settings or size change (the feature is recreated), and a toggle. [T]
- **Mono mode** (`BB_XR_MODE` not alt): one stream, slot 1.

### 3.1 Jitter (per-eye Halton)

The scene draws (the draws into the scene depth) get a sub-pixel viewport shift. This is the same
mechanism and code path the flat upscaler uses (`Rasterizer::draw_jitter`), and it is equivalent to
jittering the eye projection. The projection itself comes from the camera hooks (`runtime_vr_stereo`,
H1), and the GPU-side shift needs no change there.

- Each eye has its own Halton(2,3) sequence, phase n = 1, 2, 3, ..., with `8 * (out/in)^2` phases
  (NVIDIA's rule). One global sequence over L R L R would give each eye every other index. In base 2,
  odd indices always have their first digit set, so the left eye's x jitter would stay in [0, 0.5).
  [T]
- The eye of a frame is known for certain only at its flip, after its draws. The jitter is chosen at
  the frame's first scene draw for the predicted eye: the opposite of the last eye frame, because flip
  N is prepared before frame N+1's draws, on the same command stream after the draw pipe drains. The
  jitter actually used is kept with the frame (`XrDlss::OnFrameBoundary` at the display pass) and
  passed to DLSS. A misprediction (the same eye twice) costs only sequence order, never correctness.
  [T]
- The DLSS jitter offset is the viewport shift in render pixels, with the same sign convention as the
  flat DLSS path. `BB_XR_DLSS_JITTER_SIGN=-1` flips it for testing. [U]

## 4. Motion vectors and depth

- **What the flat integration uses** (`vk_camera_motion`, `vk_object_motion`): camera reprojection of
  the scene depth (scene constants: view, inverse view, projection), plus object vectors from the
  G-buffer shaders' clip positions where a draw matched its previous frame (`motion_history.h`).
- **VR:** camera reprojection only, from the current camera to **that eye's previous camera**:
  `reproject = prev_same_eye.view * current.inv_view`, with both projections. It runs on the existing
  `camera_motion.comp` pass (`CameraMotion::RecordMotionWith`, mode 0). Head motion and body/stick
  motion over the two frames are both in the camera, so they are covered. Jitter is removed (the
  shader unjitters with the frame's jitter). The vectors are previous minus current, in render pixels
  (`MVLowRes`, scale 1). [T: still camera gives 0, the jitter is not in the vectors, translation and
  rotation give the expected shifts, and the other eye's camera would give a false disparity of about
  60 px at 1 m]
- **No object vectors in VR.** They are relative to the previous game frame, which with alternating
  eyes is the other eye. Making them per eye would need a second history of skinned positions. Effect:
  moving enemies, NPCs, the hunter's arms and weapon, cloth and particles have only camera motion.
  DLSS's own rejection limits ghosting, but expect some smearing or softness on fast-moving characters,
  more than in the flat game (each eye's history step is 2 frames, about 28 ms at 72 FPS). [U]
- **Depth:** the G-buffer pass depth (`CameraMotion::OnGBufferPass`), captured at the display pass as
  `FinishedDepth()` (D32 or D32S8, depth aspect). The projection is D3D style (depth = zs + zo/z), near
  0 and far 1: **not inverted** (`DepthInverted` = 0). Clear depth (1.0) is sky, which gets
  rotation-only motion.

## 5. Resolution

- Render size = the display buffer: 1920x1080 by default, or the startup patch `BB_RENDER_RES`
  (`2560x1440` was measured working in findings/30 §5, with `BB_OUTPUT_RES` and `BB_DMEM_MB=9152`).
  Lower render sizes (for example 1280x720) are untested with the patch. [U]
- Output = the eye image, which the eye swapchain copies 1:1:
  - `BB_XR_DLSS_QUALITY` unset (or `auto`): the `BB_XR_EYE_RES` size (default 2 = 3840x2160), and the
    mode follows the ratio (2x = Performance, 1.5x = Quality, ...).
  - `BB_XR_DLSS_QUALITY=dlaa|quality|balanced|performance|ultra`: render size x 1 / 1.5 / 1.72 / 2 /
    3. From 1080p: 1920x1080 (DLAA), 2880x1620, 3349x1884, 3840x2160, 5760x3240. Clamped to 8192.
    [T]
- A 4090 example: `BB_RENDER_RES=2560x1440 BB_OUTPUT_RES=2560x1440 BB_DMEM_MB=9152` with
  `QUALITY=quality` gives 3840x2160 eyes from 1440p. [U]

## 6. Options

| Variable (control-file key) | Values | Default |
|---|---|---|
| `BB_XR_UPSCALE` (`upscale`) | `off` \| `fsr1` \| `dlss` | off |
| `BB_XR_DLSS_QUALITY` (`dlss_quality`) | `auto` \| `dlaa` \| `quality` \| `balanced` \| `performance` \| `ultra` | auto |
| `BB_XR_DLSS_PRESET` (`dlss_preset`) | `default` \| `J` \| `K` \| `L` \| `M` (or 0-15). A, B, C, D, G, H, I, N, O select no model in 310.9.1 (warned); E and F are deprecated | default |
| `BB_XR_DLSS_SHARPEN` (`dlss_sharpen`) | 0..1. RCAS after DLSS at the output size (the FSR pass in a new 1:1 mode). NGX's own sharpness is also passed but ignored by the DLSS 4 models | 0 |
| `BB_XR_EYE_RES` (`res`) | the output size with `auto` | 2 |
| `BB_XR_AA` (`aa`) | FXAA on frames DLSS does not take | off |
| `BB_DLSS=0` | do not load the DLSS DLLs at all | |
| `BB_XR_DLSS_JITTER_SIGN=-1` | debug: the opposite jitter sign | |

**Hot toggle: yes.** `BB_XR_IQ_CONTROL=<file>` is re-read within 250 ms. `upscale=dlss` and
`upscale=fsr1`/`off` switch without a restart, and so do quality, preset and sharpen. The NGX device
extensions are enabled at start whenever the DLLs are present (`BB_DLSS` not 0), so DLSS can be
switched on later. Switching off releases both features. A size, quality or preset change recreates
them, and their histories reset.

**Fallback** (`XrDlssLogic::Select`, [T]). The eye images keep the DLSS output size through FSR 1,
with FXAA, and one `VR DLSS: not used: <reason>` line is logged. The reasons:
- the DLLs are missing;
- not an NVIDIA GPU, or no DLSS on this GPU/driver (`Dlss::Problem()`);
- the bridge has ABI 1;
- `BB_UPSCALER=none` (no motion pass);
- the flat temporal upscaler is jittering;
- a run-time failure;
- HDR output.

Frames DLSS skips only for a moment (menus, held frames) get FSR 1, plus FXAA if `aa=fxaa`.

## 7. Limitations

- Each eye's history is two game frames old. That is about 36 Hz per eye at 72 FPS: larger motion
  per step than flat DLSS gets, so it is softer in fast head turns. True stereo (findings/31) would
  give each eye a 72 Hz history.
- No object motion vectors (section 4).
- Mono/menu frames that have a scene camera are still jittered (±0.5 px) but not reconstructed.
- GPU cost: one DLSS evaluation per game frame, at the eye size. Two features are resident, roughly
  2x the flat VRAM (some hundreds of MB at 4K).
- Changing the render resolution needs a restart (it is a startup patch). The output size and
  everything else are live.
- The jitter sign and the visual result have not been checked in the headset (section 9).

## 8. Build and test

```
# SDK (official only):
C:\bloodborne_vr_deps\dlss-sdk-310.9.1\{include,lib\Windows_x86_64\x64\nvsdk_ngx_s.lib,lib\Windows_x86_64\rel\nvngx_dlss.dll,LICENSE.txt}
# port + tests (MSYS2 CLANG64)
cmake -S gpu -B out/gpu-dlss -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off -DBB_LTO=OFF \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DBB_DLSS_SDK_ROOT=<repo>_deps/dlss-sdk-310.9.1
ninja -C out/gpu-dlss -j4 bb-probe xr-dlss-test dlss-bridge-smoke
ninja -C out/gpu-dlss bbport-dlss        # MSVC bridge -> out/gpu-dlss/dlss-bridge/Release/bbport_dlss.dll
out/gpu-dlss/xr-dlss-test.exe            # logic
dlss-bridge-smoke.exe <dir with bbport_dlss.dll + nvngx_dlss.dll>   # real NGX, no game
```

`BB_DLSS_SDK_ROOT` is the new CMake option for the SDK path. `packaging/windows/build_dlss.sh` still
works as before.

Results on this machine (RTX 4090, driver 32.0.16.1692):
- `xr-dlss-test`: all passed.
- `dlss-bridge-smoke`: `DLSS slot 1/2: 960x540 -> 1920x1080`, outputs L 127,127,127 / R 204,51,25
  (each eye matches its own input), PASS.
- `bb-probe` builds with the VR DLSS path.

## 9. Not tested yet

Running in the game or the headset (not launched while the headset was in use). To check:
- `VR DLSS: on` and the per-eye feature lines in the log;
- the 10 s `VR DLSS: 10 s: evaluated L n R n` balance;
- no swimming on static geometry while the head turns (motion vectors and jitter sign; try
  `BB_XR_DLSS_JITTER_SIGN=-1` if edges crawl);
- ghosting on enemies;
- GPU time (`BB_GPU_PROFILE=1`, the `VR IQ: dlss` mark);
- the hot toggle through `out/dlss-vr/iq.txt`.

## 10. Licence notes (NVIDIA RTX SDKs licence, v. March 14 2024, in the SDK's `LICENSE.txt`)

- **Redistribution of `nvngx_dlss.dll` is allowed** (§1c) "as incorporated in object code format into
  a software application", subject to §2:
  - (a) the application must have material additional functionality;
  - (c) distribution must be under terms at least as protective as NVIDIA's;
  - (d) known non-compliant use must be reported to NVIDIA.
- §4b: no stand-alone distribution of the SDK. §4a: no reverse engineering. §4e: the SDK must not be
  made subject to an open-source licence that requires source disclosure or free redistribution.
  This is why the NGX code stays in the separate MIT `bbport_dlss.dll` and the GPL port only loads
  it at run time. Whether shipping NVIDIA's binary next to a GPL program is acceptable is a
  legal question this branch does not decide.
- Supplement §4: **notify NVIDIA before a commercial release**.
- Supplement §7.1(b): **NVIDIA attribution and marks on splash screen, about box and credits**.
- Supplement §1: use only on systems with NVIDIA GPUs. §6.1: NVIDIA may update the DLL over the
  air.
- Therefore nothing here adds `nvngx_dlss.dll` to release tooling. The binary is only in the local
  `out/dlss-vr/` test folder, with `NVIDIA-DLSS-LICENSE.txt`. Before any public release that ships
  it, someone must decide whether the GPL/§4e interaction, the attribution and the notification are
  handled.

## 11. Device lost on a one-frame eye size flip (fixed 2026-10-10)

Three crashes (`user/crashes/20261010-122419-exit-1193` FSR 1 on SteamVR,
`20261010-144052-exit-f634` and `20261010-145315-exit-d1f9` DLSS; the last one while healing)
end the same way: `VR: <eye> eye images 1920x1080` and, one frame of that eye later,
`... eye images 2880x1620` (3840x2160 with FSR 1), then `Device lost during submit`. With DLSS the
eye's feature is recreated twice as well (`DLSS slot 1: 1920x1080 -> 1920x1080`, then
`-> 2880x1620`). 1920x1080 is the window size, not an eye size.

Cause: the frame size the GPU thread uses in `GetRenderFrame` was written by the present thread in
two steps per present, first the window size (`SetExpectedGameSize`), then the eye IQ size
over it. The GPU thread could read it in between and make one frame at the window size; the eye
swapchain followed it (the bridge's `AcquireEye` destroys and recreates a swapchain whose size
changes, `xrDestroySwapchain`, while earlier copies into it and the runtime's submissions may still
run), and so did the DLSS feature and output image. A one-frame change of the IQ source (the
display buffer size) would have done the same.

Fix (`renderer_vulkan/xr_resize_logic.h`, `tests/test_xr_resize.cpp`, `ninja xr-resize-test`):
- the present thread computes the size once and publishes it whole (one atomic);
- a new size is used only after it held 3 presents (`SizeDebounce`); the first one applies at once;
- before an eye / flat / HUD swapchain this side wrote is recreated, the presenter waits for the
  graphics queue (under the submit mutex, so the runtime's submissions are covered);
- a new frame size waits for the draw scheduler's submitted work before the FSR / FXAA pass images
  and DLSS outputs are recreated; DLSS motion and output images wait the same way (features already
  did).

Log lines: `VR: frame size A -> B (<reason>; window, display buffer, eye IQ)` when the size really
changes; `VR: frame size: holding back B (...)` (at most once a second) for a target that did not
hold, with what it was made of; `VR: eye L swapchain A -> B: waited N ms for the GPU ...`;
`VR: frames now B (were A): GPU idle before the passes resize`; `VR DLSS: eye output A -> B`.
In normal play none of them should appear after start-up; a live `res` / `dlss_quality` change
shows one `frame size` line, then one swapchain line per eye.

## 12. Arms and weapon smear: the bias-current-colour mask (2026-10-10)

The player's arms and weapon move with the hands, but the motion vectors are camera-only
(section 4), so DLSS reprojects those pixels with a wrong vector and blends in the wrong history:
blurry, smeared arms. NGX has an input for exactly this, `NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask`
(`pInBiasCurrentColorMask` in `NVSDK_NGX_VK_DLSS_Eval_Params`): 1 = trust the current frame's colour.

- **Bridge ABI 3** (`BBPORT_DLSS_BRIDGE_ABI`): `BbDlssEvaluate` gets `bias_mask` (a `BbDlssImage`) at
  the end; a null view means none (the flat game, ABI 2 callers). `EvaluateSlot` maps it to
  `pInBiasCurrentColorMask`.
- **Mask pass** (`host_shaders/xr_dlss_mask.comp`, run in `XrDlss::Run` right after the motion pass,
  same depth view): R8_UNORM at render size; linearises the eye depth with the current projection
  (`z = zo / (depth - zs)`), takes the radial view distance, writes 1 where it is below
  `dlss_mask_m`, else 0. Sky/cleared depth is never masked. Binary (no soft edge). The same math is
  `XrDlssLogic::LinearDepth/ViewDistance/NearMask` (unit tested in `xr-dlss-test`).
- **Cost**: one 8x8 compute dispatch per game frame at render size, one R8 image.
- **Side effect**: any geometry nearer than the distance (a wall you press against) is also
  "trust current", i.e. slightly less temporal stability there.

| Variable (control-file key) | Values | Default |
|---|---|---|
| `BB_XR_DLSS_MASK` (`dlss_mask`) | `0` \| `1` | 1 |
| `BB_XR_DLSS_MASK_M` (`dlss_mask_m`) | metres from the camera, 0 = off, capped at 5 | 0.9 |

Live through `BB_XR_IQ_CONTROL` (the start menu: DLSS tab, "DLSS arm/weapon mask" and "Arm mask
distance (m)"). The log shows `VR DLSS: arm/weapon bias mask on (surfaces nearer than 0.90 m use the
current colour)` when DLSS starts and on every change, and the `VR IQ (...)` line ends with
`arm mask 0.90 m` / `arm mask off`.

**DLL / exe pairing.** An exe built with ABI 3 loads an ABI 1 or 2 bridge (the mask is then off, one
`VR DLSS: arm mask requested but the bridge has no bias mask` line). An older exe (ABI 2) does not
load an ABI 3 bridge, because the bridge would read a larger struct than the exe fills in: it logs
`DLSS: unavailable (bbport_dlss.dll is ABI 3 but this build needs ABI 1-2 ...)` and falls back to
FSR 1. So bb-probe-all34 needs the ABI 2 dll (`out/bbport_dlss.abi2.dll`), bb-probe-all35 and later
the ABI 3 one.

Not tested in the headset yet: the 0.9 m default is a guess for the arm and weapon reach in
first person. `dlss-bridge-smoke` checks that the bridge accepts the mask (right eye) and that the
output still matches its input.

## 13. Per-eye audit, feature logging, jitter off switch (branch dlss-eye-audit)

Audit result (a study of a Cyberpunk VR port suggested a shared per-eye cache as a typical bug): no
shared state found. Every per-evaluate field is per eye: the feature (bridge slot 1/2, own history,
`InRenderSubrectDimensions` from that slot's own create description), output image and views
(`XrDlss::Impl::eyes[]`), jitter sequence (`Tracker::EyeState::jitter_seq`), previous camera, reset
flag, frame time. The change detect is `Dlss::HasFeature(desc, slot)`, per slot, so eye 2 never sees
the other eye's description. MV scale, exposure and pre-exposure are constants 1 in the bridge;
sharpness comes from the settings. The motion vector image and the mask image are shared between
the eyes on purpose: they are rewritten and consumed within each eye's own evaluate, in command
order, with barriers. ABI unchanged (3).

Added:
- Every feature create/release is logged per eye with the reason (`first use`, `resize`,
  `quality change`, `preset change`, `reset` = released by a settings change or DLSS toggle):
  `VR DLSS: eye L feature created (resize): render WxH -> output WxH, quality, preset X`.
  `XrDlssLogic::CreateReasonFor` decides the reason (unit tested), `RecreateGuard` warns once
  (`VR DLSS: WARNING: eye R feature created N times within 2 s ...`) when one eye is created more
  than 3 times in 2 s.
- `dlss_jitter=0|1` in iq.txt (env `BB_XR_DLSS_JITTER`, default 1). 0 zeroes the per-eye jitter in
  the projection offset (`XrDlss::SceneJitter`), in the value passed to DLSS and in the motion
  vector / mask passes, and restarts the histories. Logged as `VR IQ (control file): ... jitter
  OFF (diagnostic)` and `VR DLSS: jitter OFF/on`. Use it to see whether a shimmer or blur comes from
  the jitter. Tests: `xr-dlss-test` (recreate reasons, guard, per-eye Halton, jitter off).
