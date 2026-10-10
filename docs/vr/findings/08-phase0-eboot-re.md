# 08: Phase 0 static RE of the real eboot (camera seam, culling, effect sites)

Date: 2026-10-09. Input: the user's own `C:/games/CUSA03173\eboot.bin` (APP_VER 01.09, plaintext SELF).
Static analysis only (capstone 5 + eh_frame function table + relocation-applied image); the game was not run.
Scripts and the extracted image live in the session scratch dir `scratchpad/re/` (not in the repo):
`extract.py` (SELF → ELF via `prepare.parse_self`), `reloc.py` (applies the 231k R_X86_64_RELATIVE relocs at base 0x400000),
`index.py` (call/RIP-ref index over all 162,959 eh_frame functions), `ripscan.py` (brute-force rel32 search, also finds code with no FDE),
`steps.py` (maps `X::STEP_*` UTF-16 names to their functions).

Legend: **[C]** confirmed from the disassembly. **[L]** likely (strong circumstantial evidence). **[S]** speculation.
All addresses are PS4 VAs, the same space as `patches/Bloodborne.xml`.

---

## 1. ELF map and address translation

| PH | type | flags | file off | vaddr (ELF) | PS4 VA (+0x400000) | filesz / memsz |
|---|---|---|---|---|---|---|
| 0 | LOAD | R-X | 0x4000 | 0x0 | 0x400000 | 0x50d96dc (code 0x4000a0..~0x2fc0000, then rodata) |
| 1 | LOAD | RW- | 0x50e0000 | 0x50dc000 | 0x54dc000 | 0x309b38 / 0x5f70f4 (data + bss) |
| 2 | SCE_RELRO | R | 0x50e0000 | 0x50dc000 | | 0x40 |
| 3 | DYNAMIC | | 0x5952c38 | | | 0xc80 |
| 5 | TLS | | | 0x50dc040 | | memsz 0x750 |
| 6 | GNU_EH_FRAME | R | 0x4f45510 | 0x4f41510 | 0x5341510 | 0x1981cc |
| 7 | SCE_DYNLIBDATA | | 0x53e9b40 | | | 0x569d78 |

- **Patch VA = ELF vaddr + 0x400000** **[C]**. This is what `scripts/patches.py` (`EBOOT_BASE=0x400000`, writes at `addr-0x400000`)
  and `probe.c apply_patches` already assume; the loader maps ELF vaddr 0 at image offset 0. Proof: Lance's
  "Restore Debug Camera" lines at 0x01972998.. are the original bytes shifted by one (`74 17` → `74 16`, `48 39 4a 20` kept, etc.),
  and every effect site decodes to a sensible instruction (section 5).
- PLT stubs sit at 0x2fbe4f8.. (`__stack_chk_fail` 0x2fbe4f8, `memcpy` 0x2fbe578, `tanf` 0x2fbe648, `atan2f` 0x2fbe698, `memset` 0x2fbe688;
  0x2fbe518 is an unresolved NID `ZtjspkJQ+vw` used as sin(edi=0)/cos(edi=1) of xmm0). `0x24b5470` is a `jmp tanf` thunk. **[C]**
- Debug-build leftovers make naming easy: `FD4Singleton.h` asserts carry singleton names, and static initializers register
  step functions with UTF-16 names (`SprjCameraStep::STEP_Update` etc.). **[C]**

## 2. Pre-patched bytes in this eboot (the "1080p_60fps" package)

GoldHEN XML lines carry no "original bytes", so I compared each line's value with the eboot.

- **The eboot is NOT retail 1.09 in the frame-pacing code.** **[C]** 61 lines that only "Uncap FPS++" writes are already present
  (code caves at 0x13d2e16..0x13d3562, 0x1bf9b71, 0x21bc181..0x21bc30d, 0x2377xxx, 0x2483xxx, 0x2715xxx), and 11+6 lines common
  to all FPS++ variants are present (0xfbc40f `eb1d`, 0x2434831.. `mov dword [r12+0x10],1` …). Other FPS++ lines are not
  (e.g. 0x243480f still `...02`, 0x24347f3 `8b11`). So it's a mixed baked-in frame-rate mod. Applying an FPS++ patch on top
  overwrites the same addresses with consistent values, but **do not assume retail bytes when writing new patches in the
  frame-pacing code** (0x13d2xxx, 0x1bf9xxx, 0x21bcxxx, 0x2418xxx, 0x2434xxx, 0x2377xxx).
- `0x04D25844` holds **1/30 (`8988083d`)**, the retail value; the FPS++ patches write 1/60, 1/120.
- Resolution lines: "Optimal 1080p"/"Resolution Patch" share 18–28 equal lines (`b8800700` = mov eax,1920, single `00` bytes);
  these look like retail 1080p values, not a modification. **[L]**
- **All camera/effect sites used by the VR plan are unmodified original code** **[C]**: auto-rotate (4 sites), camera distance,
  motion blur/AA (`01` flags), DoF (`0f84 de000000`), CA (`8b8590f5ffff 8983ac000000`). The Lance debug-camera hunk matches
  only on the 5 bytes it re-writes unchanged.
- **Caution [C]:** Lance's debug-camera cave at 0x1de3328..0x1de368b overwrites a *live* function (0x1de3330, called from
  0x1d62765 and 0x1dbe4f2). Do not use it as a cave for our own code.

## 3. The camera pipeline (who writes what, per frame)

```
ChrExFollowCam (=[ChrCam+0x60])  ── update 0x183ac60 (yaw +0x144, pitch +0x140)
        │ ChrCam director 0x18368b0 copies the active sub-camera into ChrCam+0x10..+0x5c
        ▼
in-game step 0x1d3ac10 (step-table 0x57be1a0 slot 9; r13 = this):
   0x1d3be42 call 0x18368b0(ChrCam=[r13+0x128], chr, ...)          ; camera controller update
   0x1d3be5a..0x1d3beb3 copy ChrCam → r15 = [[0x594F880]+0x10]      ; "render camera" (game side)
   0x1d3bec0 call 0x1972900(dbg=[r13+0x110])                        ; debug-cam step (Lance's 0x01972998 lives here)
   0x1d3bec5..0x1d3bf33 if dbg->+0xc4 && dbg->+0xd0: copy debug cam → r15
   0x1d3d434 call 0x1972710(dbg, r15, ChrCam)                       ; snapshot r15 → [dbg+0x10], cull cam → [dbg+0x18]
   0x1d3d989 call 0x19715f0(dbg)                                    ; applies camera effects to [dbg+0x10] and
        │                                                           ;   SetFieldCam: copy → SprjCamera+0x10 and +0x28, flags |= 1|8
        ▼
SprjCamera singleton (global 0x593E8F8), 5 PersCams: +8 active, +0x10 field, +0x18 event/Remo, +0x20 debug, +0x28 last
   SprjCameraStep::STEP_Update 0x1e09cd0 → jmp 0x1e08e00: picks +0x10/+0x18/+0x20 by flags into +8, mirrors +8 → +0x28,
   Remo (cutscene) camera written into +0x18 here (from [[0x5940058]+8]+0x120.., fov +0x160)
        ▼
SprjDrawStep::STEP_Update 0x21924c0 → 0x22ddf30(r12 = draw-camera manager):
   0x22de0bf..0x22de10e copy [SprjCamera+8] → private PersCam [r12+0x28]
   0x22de113..0x22de19b prev draw cam [r12+0x20] ← [r12+0x18]
   0x22de1ae [r12+0x28]->vfunc+0x28 = 0x2116690 "GetDrawCamera" → [r12+0x18] (format B, below)
   0x22de1bb 0x2694d40([r12+0x10], drawcam): scene camera + culling-camera copy at +0x90 (skipped if debug "freeze" byte 0x5940DD4)
   0x22de1c0.. RendMan-side copy of the PREVIOUS draw cam at +0x2980.. (motion vectors / TAA history)
   0x22de397 [SprjCamera+0x28]->vfunc+0x28 → local [rbp-0x340]; 0x22de3a2 SetCamera 0x2691c30(ctx=r12+0x50, local)
   0x22de3c7 SFX system gets the camera matrix (global 0x5940370 → +0x70 → +0x260 / +0x5f0)
```

Confidence: each arrow is **[C]** from disassembly except "[dbg+0x10] is the camera published by 0x19715f0 in the normal
(non-debug) path" which is **[L]** (the branch at 0x1971bc4 loads `rbx=[r14]`, `r14=&dbg+0x10`; the copy to
SprjCamera+0x10/+0x28 with `or [..+0x30],1` / `,8` is at 0x1971dda..0x1971eb2; the loop before it multiplies the matrix by
per-effect matrices built by 0x1be05d0, most likely camera shake). Nothing else in the binary writes SprjCamera+0x10.

### 3.1 Layouts

**PersCam** (game camera; class vtable 0x576BA10 for the 0x594F880 holder camera, alloc size 0x60) **[C]**
| off | meaning | default (ctor 0x1832bc0) |
|---|---|---|
| +0x00 | vtable | |
| +0x10/+0x20/+0x30 | rotation rows right/up/forward (+0x30 = view axis) | |
| +0x40 | position (w=1) | |
| +0x50 | **FOV, vertical, radians** | 0x3f402037 = 0.7505 (43.0°) |
| +0x54 | aspect | 1.7778 |
| +0x58 | near | 0.0501 |
| +0x5c | far | 3000.0 |

Hypothesis "matrix at +0x10, FOV at +0x50": **confirmed** for every camera in the chain (ChrCam, follow cam, holder cam,
debug cam, SprjCamera slots).

**Draw camera "format B"** (output of 0x2116690, input of the projection builders) **[C]**
| off | meaning |
|---|---|
| +0x00..+0x3f | camera world matrix (PersCam +0x10..+0x4f) |
| +0x50 | FOV (vertical) |
| +0x54 / +0x58 | near / far |
| +0x5c / +0x60 | width / height (u32) — **hard-coded 1920 / 1080 in 0x2116690** (`movabs 0x43800000780` at 0x21166d5) |
| +0x64 | aspect (PersCam +0x54); if 0, width/height is used |
| +0x68 | byte: apply sub-pixel jitter (TAA) |
| +0x6c..+0x80 | tile / off-center sub-rect (only used by 0x25aa290) |

**Draw context** (SetCamera 0x2691c30, `this`=rdi; vtable at ~0x5760f40) **[C]**
+0x10..+0x40 view (inverse world), +0x50..+0x80 view·proj, +0x90..+0xc0 projection, +0x110/+0x114 w/h, +0x140 fov,
+0x144 near, +0x148 far, +0x14c aspect, +0x130/+0x120 LOD vector, **+0x150 LOD scale = tan(fov/2)/tan(0.48)/(h/720)**,
**+0x160..+0x1b0 six world-space frustum planes**, **+0x1c0..+0x210 six view-space planes**, +0x220/+0x230 frustum AABB,
+0x240..+0x2c0 copy of the format-B camera.

### 3.2 Projection builders **[C]**
- `0x25aa6c0(out, cam)`: symmetric D3D-style LH [0,1]: `sx = cot(fov/2)/aspect`, `sy = cot(fov/2)`, `A = f/(f-n)`,
  `B = -n·f/(f-n)`; cot via sin/cos (0x2fbe518). +0x68 multiplies in a jitter matrix. Called only from SetCamera.
- `0x25aa290(out, cam)`: same but with the tile/off-center path (+0x70 tile count, +0x74 index, +0x78/+0x7c/+0x80);
  uses `tanf(0.5·fov)`. 13 callers (render passes incl. DoF 0x25d969a, 0x2592f7a, 0x2691004, 0x12645d0 …).
  **This off-center support is a ready-made hook for asymmetric per-eye frusta later [L].**
- `0x25aaa90`: wraps 0x25aa290 (15 callers); used at 0x22de59f for RendMan's per-frame projection.

## 4. Hypotheses: confirmed / refuted

| Hypothesis (findings 01–06) | Verdict |
|---|---|
| 0x01972998 is the per-frame camera seam | **Refuted as stated [C].** It is inside `0x1972900`, the *debug-camera step* (FD4PadManager lookup; if dbg+0xc4==1 it calls the free-cam mover 0x21145b0 with dt=1/30). Lance hooks it because it runs once per frame next to the real seam. The real seam is its caller, the in-game step 0x1d3ac10 at 0x1d3be5a..0x1d3bf33, and the publish to rendering is 0x19715f0 → SprjCamera. |
| Camera struct: view/camera matrix at +0x10, FOV at +0x50 | **Confirmed [C]** (+0x54 aspect, +0x58 near, +0x5c far). The matrix is camera-to-world (rows right/up/fwd/pos), not a view matrix; the view is built in SetCamera. |
| FOV is vertical | **Confirmed [C]** (`sy = cot(fov/2)`). Unlike Sekiro, aspect (+0x54 → format B +0x64) **is** used by the projection. |
| Camera-controller cluster 0x01836000–0x01842000 | **Confirmed [C]**: ChrCam director 0x18368b0, ChrExFollowCam update 0x183ac60 (0x4e6d bytes), aim cam 0x1838320/0x1839430, ctors 0x183a0e0/0x183fb60, collision/near-plane 0x18414f0, camera-holder ctor 0x1832bc0. |
| Auto-rotation at 0x0183c6e8 | **Confirmed [C]**: `vmovss [r13+0x144], xmm0` storing `atan2f(...)` (0x2fbe698) = follow-cam **yaw**. 0x183c870 = wrapped-yaw store (mod 2π), 0x183c984 = auto-rotate timer `[+0x130]`, 0x183dde6 = `[+0x294]` (auto pitch/height blend). |
| Camera yaw/pitch storage | **ChrExFollowCam +0x144 = yaw [L], +0x140 = pitch [L]** (+0x148 = default pitch: `[+0x140]=[+0x148]` on reset at 0x183bcea). ChrCam: +0x60 follow cam, +0x78 active sub-cam, +0x84 mode (switch 0..4) **[C]**. |
| CPU frustum/culling builder exists | **Found [C]**: planes are extracted (Gribb–Hartmann) from the projection inside SetCamera 0x2691c30 at 0x26923b4..0x26925f0; tests are virtual methods of the draw-context class (0x2692b80 point/AABB vs the 6 world planes, 0x26928a0/0x26929b0 vs frustum AABB). |
| 0x04D25844 = 1/120 movement constant | **Refuted [C]**: the eboot holds **1/30**; it is read by exactly one function (0x16ab4c0, 8×`vmulss`, WorldChrMan/SprjWorldAiManager code) to turn u16 frame counts (record +0x44/+0x46) into seconds. Not a global timestep. The 120 FPS limit has to be found elsewhere (the FPS++ code caves, section 2). |

## 5. Effect patch sites **[C]**

| site | function | original | meaning |
|---|---|---|---|
| 0x026C2549 (MB) | 0x26c1050 (render-settings ctor) | imm byte 2 of `c7 83 68290000 01010101` | `[obj+0x2969]=1` motion blur on → patch 0 |
| 0x026C254A (AA) | same | imm byte 3 | `[obj+0x296a]` AA on |
| 0x026C2538 (dyn shadows) | same | `c6 83 6e290000 01` | `[obj+0x296e]` |
| 0x025D7BBC (DoF) | 0x25d5f80 (DoF pass; also builds a projection via 0x25aa290) | `0f84 de000000` je | patch `e9 df000000 90` = always skip DoF block (same target 0x25d7ca0) |
| 0x0269FAA8 (CA) | 0x269e990 (post-process param setup, calls SetCamera) | `mov eax,[rbp-0xa70]; mov [rbx+0xac],eax` | patch stores 0 to CA intensity `[rbx+0xac]` |

The settings object with +0x2960/+0x2968 is probably the one at global 0x593E860 (the in-game step writes `[+0x2960]` and
toggles `[+0x2821]` from it) **[L]**.

## 6. Culling: how to widen it

- The CPU frustum is **the same projection** the context renders with (planes from `0x25aa6c0` output, built from the
  draw camera's fov/aspect). So **setting the camera FOV/aspect to the HMD FOV widens culling automatically** **[C]**.
- To widen culling *only* (margin for reprojection, or a union frustum for two eyes), scale `P00`/`P11` of the stack copy of
  the projection **after** it has been stored to `ctx+0x90` (0x2691fad) and after view·proj (+0x50, 0x26923ac), right before
  plane extraction: hook window **0x26923b4..0x26923d4** (four `vmovaps xmmN,[rbp-0xe0/-0xd0/-0xc0/-0xb0]`, 32 bytes, no RIP/branches).
  Multiply xmm0 by (k,1,1,1) and xmm1 by (1,k,1,1) with k<1 (k = tan(game half-fov)/tan(wanted half-fov)). This changes
  only +0x160..+0x210 planes and the +0x220/+0x230 frustum AABB. **[L]** (the AABB is derived from the planes at 0x2692606).
- SetCamera is used by every pass (main view, shadows, reflections: callers 0x2690dd0, 0x269e990, 0x26af630, 0x26b5b10,
  0x26d1990, 0x26d4ad0, 0x26d5770, 0x22ddf30×2). Filter in the host on the main context (`rdi == drawmgr+0x50`).
- **LOD side effect [C]:** ctx+0x150 scales LOD by `tan(fov/2)`; a 100°+ VR FOV pushes LODs ~2–3× lower. Compensate by
  rewriting +0x150 after 0x269207d (or use the existing "Model LOD -2" patch).
- Second culling camera: 0x2694d40 keeps a copy at scene-object+0x90 that the debug "freeze culling camera" byte
  (0x5940DD4) can pin; debug menu items "UseCullCam" (dbg+0xc2) / "DispCullCamFrustum" (dbg+0xc3) exist in 0x196fec0.
  Which culler reads +0x90 is not traced **[open]**.

## 7. Proposed Phase 2 hooks (head tracking, mono)

Design: inject only into **render-side private copies**, so game logic (movement direction, lock-on, AI, camera
controller) never sees the head pose and **no restore step is needed**. This is the Sekiro inject pattern minus the restore,
possible because the draw step copies the camera before using it.

All windows below contain only position-independent instructions; the trampoline replays them verbatim, then jumps back.
Each is ≥14 bytes, so the patch can be a 14-byte absolute `jmp [rip+0]; dq trampoline` (FF 25 00000000 + imm64), padded
with NOPs; no in-image cave needed. Stack is 16-byte aligned at every site (checked from the prologues).

| id | site | bytes to replace | regs at the site | host action |
|---|---|---|---|---|
| **H1** | **0x22de113** (in 0x22ddf30, after the private PersCam copy) | 18 bytes `498b442418 498b4c2420 c5f82800 c5f82901` (resume 0x22de125) | **rbx = PersCam\*** (`[r12+0x28]`), r12 = draw-camera manager; rax/rcx/rdx/rsi/rdi/r8–r11/xmm0–5 dead here | `M' = M_game · T(head pos) · R(head rot)` on +0x10..+0x40; set +0x50 = HMD vertical FOV, +0x54 = HMD aspect; remember r12 |
| **H2** | **0x22de386** (same fn, main-context camera) | 20 bytes `488b7828 488b07 488d9dc0fcffff 4889de ff5028` (resume 0x22de39a) | after replay: rbx = format-B local camera (`rbp-0x340`) | same pose/FOV on the format-B layout (+0x00..+0x3f, +0x50, +0x64). The replay includes `call [rax+0x28]` (relocatable, indirect). |
| H2-alt | 0x2691c30 entry (SetCamera) | 14 bytes `55 4889e5 4156 53 4881ec30010000` (resume 0x2691c3e) | rdi = ctx, rsi = format-B cam | apply only if `rdi == r12+0x50` saved by H1. Simpler window, but sees every pass. |
| **H3** | 0x26923b4 (SetCamera, before plane extraction) | 32 bytes (4× vmovaps from rbp-0xe0..-0xb0) | xmm0–3 about to be loaded | optional culling widening (section 6); also fix ctx+0x150 LOD here |
| G1 (alt, game side) | 0x1d3bec5 (in-game step, after ChrCam→r15 copy) | 14 bytes `498b8510010000 83b8c400000000` (flags from the `cmp` must survive: resume at 0x1d3bed3 `je`) | r15 = holder render camera, r13 = step | only if the render-side hooks prove insufficient; then game logic *does* read the camera afterwards (0x1972710/0x19715f0 snapshot, SFX), so a restore at the start of the next frame's 0x1d3be5a copy is implicit (r15 is fully rewritten every frame). |

What the **host-call patch type** in `apply_patches` (probe.c) must do:
1. New XML/`patches.bin` record: `{site VA, window length, resume VA, handler id}`. Validate that the window bytes equal the
   expected originals (store them in the record; refuse on mismatch, since this eboot is not pristine).
2. At load, per hook emit a host trampoline: save all SysV-volatile GPRs (rax, rcx, rdx, rsi, rdi, r8–r11) and xmm0–xmm15
   (or at least xmm0–5; the guest is SysV, the host is Windows-x64 clang64, so declare the handler `__attribute__((sysv_abi))`
   or switch ABI in the stub: rcx=arg, 32-byte shadow space), keep 16-byte alignment, call `handler(regs*)`, restore,
   **replay the displaced bytes verbatim**, `jmp` (absolute) to the resume VA. For H1/H2 the replay must come *before* the
   handler call in H2 (it produces the camera) and *after* in H1 (H1's displaced instructions don't touch the camera).
   Simplest generic form: two flags per hook, `replay_before` / `replay_after`.
3. Overwrite the window with `FF 25 00 00 00 00 <imm64 trampoline>` + `90` padding (needs the existing W^X dance in
   `protect()`).
4. Pose source: sample the OpenXR predicted pose in the handler (H1 runs once per rendered frame in SprjDrawStep), tag it
   with a frame id, and hand the same pose to `xrEndFrame` for that image (02 §4 pose bookkeeping).

Effect patches to enable for Phase 2 (existing XML, sites verified): Disable Motion Blur, Disable DoF, Disable Chromatic
Aberration, Disable AA (if the port's TAA replaces it), Disable Camera Auto Rotation (4 NOPs at 0x183c6e8/0x183c870/
0x183c984/0x183dde6; NOPs over full 9-byte `vmovss` stores, safe).

## 8. Open questions / Phase 0 runtime checks still needed

1. **One-frame check of the chain** (can be a log line in the H1 handler): confirm `[[0x593E8F8]+8]` matrix == ChrCam matrix
   × shake during gameplay, and that H1 alone moves the picture (expect yes; if the GPU picture follows only H2, the main
   pass uses the r12+0x50 context and both hooks are needed — the plan already applies both).
2. Which consumer uses scene-object+0x90 (culling-camera copy) vs ctx+0x160 planes (0x2692b80 family)? Grep at runtime by
   breaking on 0x2692b80 or by widening only H3 and watching pop-in.
3. The PS4-side width/height in format B is hard-coded 1920×1080 at 0x21166d5; the resolution patches don't touch it.
   Matters only if we change the guest render aspect.
4. Thread: SprjDrawStep and the in-game step run as FD4 step tasks; verify on which thread H1 runs relative to
   `VideoOutDriver::PresentThread` (affects pose latency, 02 §4).
5. The real "120 FPS movement limit": not 0x04D25844. Candidates are in the frame-pacing code of section 2: at 0x2418e3d
   the "30 FPS++" patch re-inserts a clamp of the frame delta `[rbx+0x264]` between `[rbx+0x18]` and 1/30, while this
   eboot has that spot as `mov eax,[rbx+0x264]; mov [rbp-0x38],eax` + NOPs (clamp already removed by the baked-in mod);
   the vsync/interval switches are around 0x2434800–0x2434950. Needs its own pass.
6. Cutscenes: Remo writes SprjCamera+0x18 (0x1e08e13); H1/H2 will apply head pose in cutscenes too. Decide per mode.
7. Name of the in-game step class (step table 0x57be1a0: 0x1d39350, 0x1d393e0, … 0x1d3ac10, 0x1d41c20) not resolved;
   `InGameStep` UTF-16 string exists at 0x4d86f60 but isn't linked to the table statically.
