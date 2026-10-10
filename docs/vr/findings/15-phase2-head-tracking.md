# 15: Phase 2 head tracking: camera injection, culling/LOD, recentering, headset-free test

Date: 2026-10-09. Code: `src/runtime_vr_head.c` (hook handlers), `src/vr_head_math.h` (pure math, tested by
`tests/test_vr_head.c`), `src/runtime_hooks.c` (registry), `patches/Bloodborne.xml` (hook lines),
`tools/vr_head_test.py` (headset-free test). Builds on 08 (camera chain, hook sites), 09, 10 (T0), 11 (comfort).

Legend: **[M]** measured in the running game (flat, fake head), **[C]** confirmed from the disassembly, **[S]** design.

## 1. Results

| Question | Answer |
|---|---|
| Does the injected rotation reach the picture? | **Yes [M].** Fake head yaw 10° left: the scene moves +240 px right (expected 1371·tan 10° = 242 px for the game's 43° vertical FOV at 1920×1080); pitch 10° up: +240 px down; yaw 30°: +736 px; roll 15° left: the world tilts clockwise. Mean difference 35 vs 9 between two straight captures. |
| H1 or H2? | **H1 drives the rendered view [M].** H1 alone gives the full shift (diff to "both" at noise level, lowest in the newly revealed band, so no culling loss). H2 alone changes nothing visible. Keep both installed: H2 keeps the r12+0x50 context consistent (its consumer is not identified; HUD marker projection is the candidate, see T0). |
| Camera matrix convention | **Confirmed [M][C]:** four float4 rows right, up, forward, position; orthonormal (error 1e-7), det(right, up, forward) = +1, w lanes 0/0/0/1, world +Y up, left-handed (+Z forward), row-vector (world = local·M). FOV vertical radians (0.7505 = 43°), aspect 1.7778, near 0.0501, far 3000. H2's format-B matrix equals H1's (max diff 0). |
| Does a wider FOV widen CPU culling? | **Yes [M][C].** At FOV 100° the draw camera, the scene camera, its culling copy (+0x90) and the r12+0x50 context all carry 1.7453 rad (P11 = 0.8391 = cot 50°); SetCamera extracts the planes from that projection; the 100° frame is complete to the edges. **H3 (widen-only) is not needed for mono.** |
| LOD | **Drops with the FOV [M]:** SetCamera's LOD scale tan(fov/2)/tan(0.48)/(h/720) goes 0.50 → 1.53 at 100°. Optional fix hook "VR LOD Fix (SetCamera)" (section 4). |

## 2. The seam (08 §3, §7)

```
draw step 0x22ddf30 (r12 = draw-camera manager)
  0x22de0bf  [SprjCamera+8] -> private PersCam [r12+0x28]
  H1 0x22de113 (replay-after, rbx = PersCam): matrix +0x10..+0x4f, FOV +0x50, aspect +0x54
     -> [r12+0x18] draw camera (format B, GetDrawCamera 0x2116690: matrix, FOV +0x50, aspect +0x64)
     -> [r12+0x10] scene camera + culling copy (0x2694d40) -> passes (0x2690dd0 builds view, proj, SetCamera)
     -> context for [r12+0x330] (0x26919c0/0x2691c30/0x2684af0), RendMan projection (0x25aaa90)
     -> SFX camera matrix (0x22de3c7) and audio listener (0x9489f0) read [r12+0x28]: they follow the head
  H2 0x22de386 (replay-before, rbx = format-B local built from [SprjCamera+0x28]) -> SetCamera(r12+0x50)
```
Game logic never reads these copies: no restore. H1 samples the head once per frame, H2 reuses the sample.

## 3. Math (`src/vr_head_math.h`)

- XR → game: quaternion (x, y, z, w) → (−x, −y, z, w); vector (x, y, z) → (x, y, −z) (mirror Z).
- Reference: a yaw-only quaternion `ref` (gravity aligned). `rel = toGame(conj(ref)·head)`.
- Compose (rows): new axis j = Σ_i R(rel)[i][j]·base_i, base = the game's rows (default: the head adds to the
  game's pitch) or the yaw-only basis of the game's heading (`BB_VR_HEAD_LEVEL=1`, horizon lock of 11 §3.1:
  R = R_y(game heading)·R_head). Heading = atan2(f.x·u.y − u.x·f.y, f.z·u.y − u.z·f.y) (exact at any pitch).
- Position (`BB_VR_HEAD_POS=s`): Δ = toGame(conj(ref)·(p − p_ref))·s, clamped ±0.25 m horizontal, ±0.15 m
  vertical, added along the yaw-only basis. Off by default.
- FOV for mono (`BB_VR_HEAD_FOV=hmd`, default with `BB_XR_MODE=mono`): the symmetric frustum around the head's
  axis that contains both eyes: tan_x/tan_y = max |x/−z|, |y/−z| over each eye's corners and edge midpoints
  rotated by conj(head)·eye (canted Pimax views included); FOV = 2·atan(tan_y), aspect = tan_x/tan_y.

## 4. LOD fix (optional hook)

`VR LOD Fix (SetCamera)`: 0x2692049, 15 bytes `c5f25ec0 8b8314010000 c4e1fa2ac8` (vdivss / mov / vcvtsi2ss, no
branch target inside, no RIP operand), replay-before. There xmm0 = tan(fov/2)/tan(0.48) and rbx = the context;
when ctx+0x140 equals (bit for bit) the FOV H1 injected this frame, xmm0 ×= tan(game fov/2)/tan(injected fov/2),
so ctx+0x150 and the +0x130 vector get the game's LOD distances. SetCamera runs for every pass, maybe on other
threads: H1 publishes {FOV, factor} as one atomic 64-bit word. `tests/test_hooks.c` runs these exact bytes
through a trampoline. Runtime check pending (next flat run: `fov100` vs `fov100_nolod`, logged LOD scale).

## 5. Recentering

Forward = the yaw of the bridge's cinema screen (`BbVrViews.screen`, `screen_flags` 1 placed / 2 head-locked,
added to `vk_xr.*`, `bbgpu.*`). The bridge places it in front of the user and again on a recenter (left stick
click held 1 s, menu hold, runtime recenter, refocus), so the head reference follows; the log prints
`VR head: recentered (screen placed again): forward = the screen's yaw …`. Until the screen is placed, or with
`BB_XR_HEADLOCK=1`, forward = the head's yaw at the first tracked frame (no recenter possible there; see §7).

## 6. Environment

| Variable | Effect |
|---|---|
| `BB_HOOKS="VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context)"` (+ `;VR LOD Fix (SetCamera)`) | installs the hooks (run.py → patches.py `--hooks`) |
| `BB_VR_HEAD=1` | compose the head (otherwise the hooks only log their first call) |
| `BB_VR_HEAD_LEVEL=1` | horizon lock (recommended in the headset) |
| `BB_VR_HEAD_POS=<s>` | head position, scale s (0 = off) |
| `BB_VR_HEAD_FOV=game\|hmd\|<deg>`, `BB_VR_HEAD_ASPECT=<a>` | camera FOV |
| `BB_VR_HEAD_LOG=1` | 1 line/s per hook: game camera, head, result, previous-frame copies, main context, LOD |
| `BB_VR_FAKE_HEAD=yaw,pitch[,roll[,x,y,z]]` \| `sweep[:yaw,pitch,s]` \| `@file` | headset-free head; the file takes `yaw= pitch= roll= x= y= z= apply=1\|2\|3 fov= aspect= level= lod=` |
| `BB_EXIT_FILE=<path>`, `BB_EXIT_AFTER_SECONDS=<n>` (src/probe.c) | the game ends itself (automation) |

## 7. Mono: each image shown with the view it was rendered with (bridge ABI 5, implemented)

Before: the mono layer gave each eye `flat_views[eye].pose` and **its own** `flat_views[eye].fov`, the views
current at `ReleaseFlat`. One image over two different (asymmetric, canted) frusta lands in two directions:
double vision **[M, headset]**, plus swimming. Now:

```
H1 (game thread)    composes the camera, then bbgpu_vr_set_render_view({seq, display_time, head pose used,
                    frustum = the camera's final FOV/aspect as tan angles, camera rows right/up/forward})
                    -> Xr::SetRenderView: a ring of the last 16
PrepareFrame        (GPU thread, the flip right after the frame's draws) Frame::camera_view =
                    CameraMotion's scene camera of that frame (world-to-view rows)
Present             Xr::ReleaseFlat(camera_view): the ring entry whose rows match (max row error < 2e-3, up to
                    sign, rows or columns) = the view the frame was rendered with; else the newest;
                    api->ReleaseFlatView(&{display_time, head, fov}) when the bridge has ABI >= 5
bridge              ReleaseFlatWith(view) keeps it with the image; the mono layer gives BOTH eyes the render
                    head orientation and the one frustum, each eye at its offset from the head
                    (xrscreen::MonoEye, from the located views); no view: the old path
```
Every 10 s vk_xr logs `VR: mono images with their camera's view: N paired (L game frames after the camera),
M with the newest view`. The frustum is the symmetric union of both eyes (§3) by default in mono; with
`BB_VR_HEAD_LEVEL=1` the displayed horizon is the real one. Still open:
- **Recenter counter.** `uint32_t recenter_count` in `BbXrViews`, incremented on every manual or runtime
  recenter even when head-locked; the game re-captures its reference when it changes (today it follows the
  screen pose, which head-locked mode does not move).
- **Depth layer** (`XR_KHR_composition_layer_depth`) for positional reprojection.

## 8. Asymmetric projection (design; needed per eye in Phase 3, not for mono)

The game's projections are symmetric (0x25aa6c0 in SetCamera, 0x25aa290 for 13 passes incl. the scene view
0x2690dd0, DoF, RendMan via 0x25aaa90). Row-vector LH: off-center adds `P[2][0] += −(r+l)/(r−l)`,
`P[2][1] += −(u+d)/(u−d)` with `P00 = 2/(r−l)`, `P11 = 2/(u−d)` (tangents), which also composes with TAA jitter
(the jitter matrix only adds to P[2][0..1]). Hooks at the builders' epilogues, replay-after, before the stack is
released: 0x25aa933 (14 bytes `4c89f8 4883c468 5b 415e 415f 5d c3`; out = r15; the camera pointer is gone, so take
SetCamera's ctx from the saved rbx at [rbp−0x18] and match ctx+0x140 to the injected FOV) and 0x25aa69c (18 bytes;
out = r14, camera = rbx: match camera+0x50). Culling planes follow automatically (SetCamera extracts them after
0x25aa6c0). The tile path of 0x25aa290 (+0x70 count) is integer-only, so not a substitute.

## 9. Headset-free test (`tools/vr_head_test.py`)

Launches flat through run.py on a copy of `user/` with H1+H2+LOD hooks, `BB_VR_HEAD=1`, `BB_VR_HEAD_LOG=1`,
`BB_VR_FAKE_HEAD=@file`, presses cross twice at the title, waits for the world (draws/frame > 600), then for each
step writes the file, waits 2 s, captures through `BB_FRAME_DUMP_TRIGGER`, ends the game with `BB_EXIT_FILE` and
compares (mean difference, phase-correlation shift, per-band differences, logged context FOV/LOD).
`BB_GAME_DIR=<dump> python tools/vr_head_test.py --out <dir> --probe out/gpu/bb-probe.exe`; `--analyze-only` re-reads.

## 10. Headset checks (coordinator)

1. Direction: turn right → view right; look up; tilt; `BB_VR_HEAD_LEVEL=1` keeps the horizon level.
2. Recenter: hold L3 1 s → log line, forward = where you face.
3. **T0 (10 §2.6):** lock on, turn the head: the dot stays on the enemy → markers use a head-rotated camera.
   Repeat with H1 only (`BB_HOOKS` without H2): if the dot then drifts, H2's context drives the markers.
4. Mono (`BB_XR_MODE=mono`, with `BB_VR_HEAD_LEVEL=1`): one image, no double vision; check the pairing log line (§7).
5. Cutscenes: the head also turns the Remo camera (H1 copies `SprjCamera+8`); decide per 11 §3.2.
