# 27: Gun aim: the player's bullets follow the left controller

Date: 2026-10-09. Branch `vr`. Status: **built and unit-tested (vr-gun-test, hooks-test). Not run in
the game yet** (the user was playing). Static analysis of `out/eboot.elf` with capstone and the
relocated image from 08 (`scratchpad/re/`). Tags as in 08: **[C]** confirmed in the disassembly,
**[L]** likely, **[S]** speculation. Image offset (logs) = VA − 0x400000.

## 1. The problem

In first person the left trigger (L2) fires the gun, but the shot goes along the character's facing
or toward the lock-on target. It ignores where the left controller points. 13 §6 #6(c) predicted
this: the shot direction comes from the character and the target, not from the gun barrel. The
direction has to be changed where the bullet is launched.

## 2. Where the shot direction is decided

```
TAE event 2/318 (bullet/gun behavior) -> RequestBehavior 0x1cc7530 -> behavior 0x1cb61c0
  -> 0x1cb6680 (bullet row): builds the request on its stack [rbp-0xc0]
       +0x00 owner handle ([chr+8]), +0x04 behavior bullet id, +0x0c BulletParam id,
       +0x30 dummy poly world 3x4 (0x1cc7800, dummy id [ev+0x20]), +0x60 character 3x4
       (transposed from [[chr+0x48]+0x20]+0x60..0x9f, or [chr+0x58]+0x90)        [C]
  -> 0x1cb7250 -> SprjBulletManager [0x593E8D0] 0x1e03620(mgr, request)          [C]
  -> 0x1dedc20 builds the emitter descriptor (0x440 bytes):                     [C]
       +0x04 BulletParam id, +0x0c owner handle ([obj+8] of 0x1946e70(request+0)),
       +0x3b0 / +0x3e0 the two 3x4 matrices,
       +0xe0..+0x113 the TARGET: lock-on (0x1df5be0, or the owner's lock target through
       [[chr+0x58]+0x280]->vf+0x88), else kind 4 = a point at muzzle + facing x BulletParam.dist
       (facing from the model quaternion [..+0x1d0], 0x1dee376..0x1dee43e)
  -> 0x1e03b50: slot (64 x 0x640, mgr+0x20), 0x1defcf0 copies the descriptor to emitter+0x1b0
  -> emitter update 0x1deffa0: per bullet (numShoot loop), picks the launch matrix (jump table on
     BulletParam +0x9a bits: identity, dummy poly 0x1df2440, character +0x590, ...), copies the
     target to [rbp-0x160] (adds the homing offset scatter, BulletParam +0x54), allocates the
     SprjBulletIns (0x1e03240) and calls
       0x1df6ca0(rdi = BulletIns, rsi = descriptor, rdx = target, rcx = launch matrix, r8d = index)
     at 0x1df0607                                                                [C]
  -> 0x1df6ca0: target -> BulletIns+0x7b0..+0x7e3, position = the matrix's 4th column
     (0x1df7067, BulletIns+0x10), orientation = the matrix (quaternion), velocity = speed x
     normalize(3rd column) (0x1df76df..0x1df77a7, BulletIns+0x40/+0x50)           [C]
```

The launch matrix is three float4 rows: row i = (axis X [i], axis Y [i], axis Z [i], position [i]).
**The bullet flies along axis Z** [C]. The target's kind-4 aim point (+0x20) is what the movement
code can steer to later: 0x1df4f00 (vtable 0x5797068) and 0x1dfe600 read BulletIns+0x7b0/+0x7d0 [C].
The direction is therefore final at the entry of 0x1df6ca0. Both the matrix and the target there are
copies on the caller's stack (`[rbp-0x120]`, `[rbp-0x160]`), so changing them affects only that bullet.

Player firearms (BehaviorParam_PC refType 1 → Bullet; the game's params): pistols 500/510/520/530,
blunderbuss 600–631 (5 pellets, shootAngle −12), cannon/gatling 700–750, rifle spear 800, Reiterpallasch
900, flamesprayer 910–929, bowblade 940–944. Most fire a short "close range" bullet that spawns its child
(HitBulletID = id+1) when it expires (launchConditionType 254). Thrown items are bullets 100–370.

## 3. The hook

| | |
|---|---|
| Patch | `"VR Gun Aim (bullet spawn)"`, disabled by default |
| Site | **0x1df6ca0** (image 0x19f6ca0), the entry of the launch init. 20 bytes `554889e54157415641554154534881ec48010000` (push rbp … sub rsp,0x148), no RIP-relative operand, no branch into the window (rel32 scan: only the three calls to the entry). Absolute-jump form. Replay-after |
| Handler | `bb_hook_vr_gun_aim` → `src/runtime_vr_gun.c` |
| Filter | `[rsp]` = 0x1df060c (the emitter's launch call; the other callers 0x1df6c7d / 0x1df7a3d are not local shots) and descriptor +0x0c = `[player+8]` (player = `[[0x593E878]+0x60]`, vtable 0x578F310) and BulletParam id (descriptor +0x04) in `BB_VR_GUN_AIM_IDS` (500–999) |
| Lock-on | locked = target handle ≠ −1 or kind ≠ 4. Default: the game's aim is kept. `BB_VR_GUN_AIM_LOCK=aim`: the ray is used and the target becomes a kind-4 point 10 m along it |
| Edit | `vg_redirect` (src/vr_gun_math.h): the game's direction is the aim point's direction from the muzzle (`ref=auto`: when it is within 30° of the launch Z, otherwise Z). The turn onto the ray is a pitch then a yaw about world +Y, with no roll. It is applied to the matrix axes and to the aim point about the muzzle. The muzzle and the aim distance are kept. A blunderbuss keeps its spread around the ray |
| Ray | `bbgpu_vr_hands` now also carries the **aim** poses (valid bits 3/4; vk_xr Hands, bbgpu.cpp). Pointing = the aim pose's −Z, relative to the head of the same snapshot, mirrored into the camera frame, then through the first-person camera rows H1 just showed (`vr_gun_view`, called next to `vr_arms_view`). The grip is the fallback (`BB_VR_GUN_AIM_GRIP_PITCH`) |
| Log | every redirected shot (at most 20 lines/s): `VR gun aim: shot N bullet ID #i: yaw a pitch b -> yaw c pitch d (turn e deg from the aim point/launch Z, aim ray; launch Z vs aim point f deg) muzzle x y z`, plus the reason when a player shot is left alone (lock-on, no first-person camera, no controller) |

## 4. How to enable

```
BB_VR_GUN_AIM=1  BB_HOOKS="<the usual VR hooks>;VR Gun Aim (bullet spawn)"
optional: BB_VR_GUN_AIM_LOCK=aim  BB_VR_GUN_AIM_HAND=right  BB_VR_GUN_AIM_IDS=all
          BB_VR_GUN_AIM_REF=auto|target|forward  BB_VR_GUN_AIM_POSE=grip  BB_VR_GUN_AIM_LOG=1
```

It needs first person (`BB_VR_FP=1`, H1). Without first person the shots are left alone and logged.

## 5. What to test

1. Pistol without lock-on: point the left controller 45° left, then up and down, and fire. The log
   shows `yaw … -> yaw …`. The tracer and the hit go where the controller points.
2. The log's "launch Z vs aim point" angle should be about 0 for a pistol. If it is large, the game
   aims along the launch matrix and not at the point: try `BB_VR_GUN_AIM_REF=forward`.
3. Blunderbuss: the pellets spread around the ray. Cannon: the shell follows the ray.
4. Lock on and fire: the log says "lock-on … kept" and the shot goes to the target. Then try
   `BB_VR_GUN_AIM_LOCK=aim`.
5. A thrown Molotov or pebble is not touched (ids 100–370).
6. Child bullets: check that the long-range part of a pistol shot (bullet id+1) also follows the ray.
   It shows as a second log line when it goes through the same launch.

## 6. Risks and open points

- **[L] Child bullets.** Most guns launch a 0.03–0.16 s "close range" bullet whose child (id+1) is the
  real shot. If the child is launched through another path (not the emitter call at 0x1df0607), it may
  go back to the character's facing after a few meters. The log shows whether 501 / 601 … appear.
- **[L] The owner of a child bullet** may be the parent bullet, not the player. Then it is skipped as
  "not the player's" (visible with `BB_VR_GUN_AIM_LOG=1`).
- Gun parry and enemy hit reactions use the bullet's real path, so aiming away from the character's
  facing is a balance change (13 §6). Animations still face forward.
- Aim pose ≠ grip: older bridges without a valid aim pose fall back to the grip's −Z.
- The ray uses the head of the controller snapshot and the camera of the last frame. Latency is one
  frame.
- Online: other players see the shot through the game's own bullet sync. Not checked.

## 7. Files

`src/vr_gun_math.h` (pure math), `src/runtime_vr_gun.c` (handler), `src/vr_gun.h`,
`src/runtime_hooks.c` + `src/hooks.h` (`bb_hook_vr_gun_aim`, `bb_hook_set_gun_impl`),
`src/runtime_vr_head.c` (calls `vr_gun_view` / `vr_gun_register`), `gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`,
`gpu/shadps4/video_core/renderer_vulkan/vk_xr.{h,cpp}` (aim poses), `patches/Bloodborne.xml`,
`tests/test_vr_gun.c` (`vr-gun-test`, 115 checks), `tests/test_hooks.c` (the real window).
