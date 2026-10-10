# 32 - Bigger weapon hit shapes for VR (hit radius and reach)

In VR you stand much closer to the enemy than the flat camera puts you, and a swing that ends a few
centimetres short is a miss the animation never had. The game's attack capsules (AtkParam, findings/14)
are tuned for the flat game. This adds two knobs, tested in the sandbox first and drawn in the game's
hitbox overlay. The game's real hit test uses them with the hook "VR Hitbox Scale (attack shape)" (section 4,
implemented, not yet run in the game).

## 1. The shared maths: `src/vr_hit_scale.h`

C11 and C++, header only, like `vr_arm_math.h`. Tests: `tests/test_vr_hit_scale.c` (built as C and as
C++: `ninja -C out/gpu vr-hit-scale-test vr-hit-scale-test-cpp`) and the sandbox collision tests.

| Parameter | Default | Meaning |
|---|---|---|
| `radius_scale` | **1.5** | multiplies every capsule/sphere radius |
| `reach_m` | **+0.15 m** | extends the shapes that reach the weapon's tip along their own axis, away from the grip |
| per class `radius_scale` / `reach_m` (short < 0.7 m, medium < 1.1 m, long) | inherit | override either for one weapon class; the class is how far the weapon's shapes reach from the grip (Saw Cleaver form B 1.21 m: long, form A 0.5 m shapes: the class follows the weapon's farthest shape over all its attacks) |
| `enabled` | on in the sandbox, off in the game until the hook exists | `hit=0` = identity |

Rules: a shape gets the reach only when its far end is at least 60% as far from the grip as the weapon's
farthest shape (the blade/tip shapes, not a root shape); a sphere (a hammer head) becomes a capsule going
outward from its centre; body-sourced shapes (kicks) only get the radius; the final radius is capped at
1.2 m; values are clamped (scale 0.25..4, reach -0.3..1 m); a negative reach never inverts a capsule.

Why these defaults: the extractor's radii are the flat game's (median 0.40 m, findings/14) but most hit
shapes sit on the blade, and the sandbox already finds a 0.6x radius right for a *tracked* blade. In the
game the animation, not your hand, places the capsule, so the miss in VR is distance: x1.5 is the
"feels fair" step before a thick capsule starts hitting what is clearly beside the blade, and 0.15 m is
about a hand's width, the typical gap between where the blade ends and where the enemy is when you stand
close. Tune from there; both are live.

## 2. Sandbox (done)

`out/vr_assets/sandbox3d.json`, hot reloaded (twice a second), under `"physics"`:

```json
"physics": { "hit": { "enabled": true, "radius_scale": 1.5, "reach_m": 0.15,
    "short": {"radius_scale": null, "reach_m": null},
    "medium": {"radius_scale": null, "reach_m": null},
    "long": {"radius_scale": 1.2, "reach_m": 0.05} } }
```

`null` or a missing key means the global value. The wireframe (`show.hitboxes`) draws the scaled shapes and
the weapon physics hits with the same ones (then `melee.attack_radius_scale`, 0.6, as before), so what you
see is what hits. The sandbox's glue is `sandbox3d/hit_scale.h`; `scene.cpp` applies it in the draw and in
`held.attack`.

## 3. Game: the overlay and the keys (done)

`out/fp.txt` (the file `BB_VR_FP_CONTROL` names; the arms and first person read it too, unknown keys are
ignored by each):

```
hit=1 hit_scale=1.5 hit_reach=0.15
hit_scale_s/m/l=<x|-1>  hit_reach_s/m/l=<m|-2>      (per class; negative = global)
```

`src/runtime_vr_hitbox.c` polls the file (4 Hz), logs `VR hitbox: hit scale on: radius x1.50 reach +0.150 m ...`
and sends the values in `BbXrGameDebug.hit_scale` (appended; an older host or bridge ignores it).
`vr_game_hitbox.cpp` draws the weapon shapes scaled when `enabled`. Default in the game: off (`hit=0`);
`BB_VR_HIT_SCALE=1` starts it on. Without the hook (section 4) `hit=1` is only a **preview**: the wireframe
grows, the game's hit test does not.

## 4. Game: the hit test (implemented: "VR Hitbox Scale (attack shape)")

### 4.1 Where the game builds the shape (static, out/eboot.elf) [C]

```
attack creation 0x1c63970 (behavior row in hand), per hit 0..3:
  0x23142d0(info, row, hit): AtkParam Radius (+0x00/+0x04/+0x08/+0x0C) -> info+0x4C,
      DmyPoly1 (+0x2C..) -> info+0x134, DmyPoly2 (+0x34..) -> info+0x138, shape kind (+0x6A..) -> info+0x50
  0x23137a0 fills the rest (damage, AtkParam id: info+0x40); radius is NOT touched there
  0x1c63f00 -> 0x1c5de90: a 0x300-byte attack entry, the info copied at entry+0x54:
      radius +0xA0, DmyPoly1 +0x188, DmyPoly2 +0x18C, owner handle +0x50 (= [ChrIns+8]), AtkParam id +0x94;
      first capsule (hkpCapsuleShape ctor 0xf86da0) in place at [entry+0x30]
attack manager 0x1c63540, every frame: all entries' UPDATE 0x1c5ed50, then all entries' TEST 0x1c5fbe0
  update 0x1c5ed50: both dummies' world matrices (0x19a7610 / model vf+0x58), p1 = DmyPoly1 position
      ([rbp-0x280]), p2 = DmyPoly2 (the matrices at [rbp-0x1d0 + 0x30*[entry+0x258]]); REBUILDS the capsule
      every frame: radius [entry+0xA0] -> shape+0x14 (0x1c5fa50), local ends A = [rbp-0x210] = 0,
      B = [rbp-0x220] = (0,0,max(1e-5,|p2-p1|)), ctor 0xf86da0 at 0x1c5faaa; transform +0x1E0..+0x210
      (origin p1, +Z toward p2)
  test 0x1c5fbe0: hkpWorld linear cast (0xf21340) of that shape from the last position to this one,
      the contacts added to entry+0x2E0 (0x1c60540), then the hit processor 0x1c60fb0 (too late for shapes)
other readers of [entry+0xA0]: the attack debug draw 0x1c61a30 only (stays unscaled)
```

Choice: the hook goes where the per-frame capsule is built (approach 2), not the param row (approach 1): it
is before the test by construction, it gets both radius and reach (the ends are in hand), it is per
attack entry so the owner check is exact (enemies untouched), and since the shape is rebuilt from
`[entry+0xA0]` every frame nothing compounds (no originals to keep, `hit=0` is the game again next frame).
The row approach would change radius only and every character that shares the row.

### 4.2 The hook

`patches/Bloodborne.xml` **"VR Hitbox Scale (attack shape)"**, 0x1c5fa55, 18 bytes
`66c7432a3800 66c743280800 66c743100301` (three `mov word [rbx+..], imm`, no RIP-relative operand, no
branch into the window, absolute 14-byte jump), replay-after, handler `bb_hook_vr_hit_scale` ->
`src/runtime_vr_hitscale.c`. At the site rbx = the shape (radius already at +0x14), r12 = the entry.
For `[entry+0x50] == [PlayerIns+8]` it: checks the layout (A = 0, B.z = |p2-p1|, else radius only);
finds the grip = the L_Weapon/R_Weapon bone (player pose, model -> world through physics +0x1E0 / yaw
+0x1D4) nearer to the capsule (farther than 0.5 m: a body shape, radius only; no pose: radius only); the
weapon reach = running max of `vhs_far_end` over the attack's shapes (`vhs_track_reach`, new AtkParam id
or 0.5 s gap restarts); then `vhs_apply_axis` (vr_hit_scale.h): shape+0x14 = r x scale, and the end
farther from the grip moves outward along the axis (B.z += reach, or A.z -= reach when DmyPoly1 is the tip).
Not covered: sphere attacks (one dummy: the sphere at entry+0x20 is built once at creation, not per
frame) and the creation frame's capsule (replaced by the update before the first test, same manager pass).
Settings: `BB_VR_HIT_SCALE=1` (the gate) and the `hit*` keys of out/fp.txt (polled 4 Hz; the user's setting
`hit=1 hit_scale=1.5 hit_reach=0.50`; the clamp allows reach up to 1.0 m). Tests: tests/test_vr_hit_scale.c
(`vhs_apply_axis`, `vhs_track_reach`, `vhs_seg_dist`, `vhs_apply_text`), tests/test_hooks.c (the window).

### 4.3 Run and what proves it

`out/vr_all.sh`: add `;VR Hitbox Scale (attack shape)` to `BB_HOOKS`, `BB_VR_HIT_SCALE=1`, and
`BB_PROBE=out/gpu/bb-probe-hitscale.exe`. Log lines:

```
VR hit scale: on for the game's hit test (...)                     start (BB_VR_HIT_SCALE=1)
VR hit scale: game hit test on: radius x1.50 reach +0.500 m ...    fp.txt read
VR hit scale: player skeleton 0x...: L_Weapon n, R_Weapon m        the grip bones found
VR hit scale: attack 700000 r 0.400->0.600 reach +0.500 (len .., right hand, grip .. m, weapon reach .. m long; +0x3c ..)
VR hit scale: attack 700000 done: HIT (cast contacts in 3 frames, max 1; character hits 1, ...)
```

No "attack" line on a swing = the hook is not installed or the owner check fails; "reach +0.000" with
"no pose" / "body shape" = the grip; "capsule layout unexpected" = the stack slots differ (radius still
applies). Character hits are counted only with "VR Stamina Hits" installed (the cast contacts always).
Open [S]: entry+0x94 as the AtkParam id (the line also prints +0x3c to settle it); whether the cast's
collector also reports walls (more contacts with a fatter blade in corridors, 4.4 b).

### 4.4 Verify by feel

(a) `hit=1 hit_scale=2 hit_reach=0` on a lone enemy: the swing that used to stop short now connects;
(b) a bigger radius also makes the blade clang walls earlier (`mapHitType`), watch tight corridors, the
class overrides or a lower `hit_scale` fix it; (c) the same hit count per swing (the per-attack hit list is
unchanged); (d) the stamina tickets still pair hits and swings (findings/20).

## 5. Try it

Sandbox: `scripts/vr_sandbox3d.sh` (MSYS2 CLANG64) and edit `out/vr_assets/sandbox3d.json` ->
`physics.hit`; the Y button cycles the attack's shapes, swing at the post/enemy: with `enabled: false` the
sandbox is as before, with the defaults the blade's shapes are fatter and 15 cm longer and the hits show up
earlier. Game: `out/fp.txt` add `hit=1 hit_scale=1.5 hit_reach=0.15`; the overlay (BB_VR_UI=1) shows the
scaled shapes.
