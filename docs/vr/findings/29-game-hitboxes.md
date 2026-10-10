# 29 - The sandbox's hitboxes over the game (debug overlay)

The VR sandbox shows weapon hit capsules, hurtboxes and hits (`sandbox3d/scene.cpp`, config `"show"`:
`hitboxes`, `hurtboxes`, `hurtboxes_self`). This brings the same view into the game. It is drawn in the
VR UI layer (`BB_VR_UI=1`, findings/26) over the game image, in stereo and anchored to the world.

## 1. What is drawn

| Item | Data | Placement | Look |
|---|---|---|---|
| Weapon hit capsules (`weapon`) | `out/vr_assets/hitbox/wp_<id>.json` (the weapon's own shapes from every attack, each once; kicks left out) | the game's `L_Weapon` / `R_Weapon` bone, model-space pose | sandbox `HitColor` (tip red, middle orange, root yellow), alpha `idle_alpha` |
| Current attack (`attack`) | the attack whose `ref_id` matches the stamina ticket's AtkParam id (`BBXR_EV_SWING_OPEN`.b); body points (kicks) skinned on their bone | same | full alpha, double width; **white, triple width for 0.25 s after `BBXR_EV_SWING_HIT`** |
| Own hurtboxes (`self`, off by default) | `chr_c0000.json` ragdoll capsules | skinned on the game's bones (pose x inverse bind) | sandbox part-group colours |
| Hit markers (`hits`) | stamina hit hooks: hit record +0x180 (world point) | world -> player model -> headset | sphere + cross; red = character, white = wall bounce, yellow = object; fades over 3 s |
| Enemy hurtboxes (`enemies`) | **not drawn yet** (section 5) | | |

Lines are the sandbox's wireframe (`sandbox3d/wire_shapes.h`, now shared by `scene.cpp` and the overlay).
In the UI layer they are thin quads that face the viewer. Their width is `line_mrad` x distance (1.5 mrad,
at least 0.8 mm). The UI layer has no depth buffer, so the shapes show through walls.

## 2. Data flow

```
pad poll (runtime_vr_ui.c, BB_VR_UI=1)
  vr_hitbox_fill (src/runtime_vr_hitbox.c)            read only, own readable-memory guard
    player pose  [[[[[0x593E878]+0x60]+0x3B0]+0x30]+0x28]+0x38   (the hkaPose the arms hook writes)
      skeleton +0x00: names +0x28, parents +0x18, reference pose +0x38 (bind, once per skeleton)
      modelPose +0x18 (hkQsTransform x N)
    camera: vr_arms_hitbox_view (runtime_vr_arms.c, the H1 snapshot the arms use; < 300 ms old)
    hits:   vr_hitbox_hit ring (runtime_vr_stamina.c vs_hit, contacts that pay: character/wall/object)
  -> BbXrGameState.debug (BbXrGameDebug, appended to the struct; ABI unchanged at 8)
bbport_xr.dll: GameUi::Build -> GameHitboxes::Draw (gpu/xr_bridge/vr_game_hitbox.*)
```

`BbXrGameDebug` holds `weapon[2][12]` (the L/R weapon bone frames), `bones[128]` with
`{FNV-1a(name), skin[12]}` and `hits[8]`. Each matrix is 3x4 and column-major. It maps a bbhitbox data point,
as stored, straight into the headset's space. An older host sends a shorter state, so the block stays zero
and nothing is drawn. An older bridge reads only its own size.

Weapon ids: the active right slot `weapons[weapon_active[0]]` and the active left slot
`weapons[2 + weapon_active[1]]` are matched to `hitbox/index.json`. The lookup tries the exact id first,
then `id/10000*10000`, then `id/100000*100000`, so upgrades fall back to the base row. Example: Saw Cleaver
7000000 (0x6ACFC0) is `wp_a_0800`.

## 3. The math (src/vr_hitbox_math.h, tests/test_vr_hitbox.c)

The math is the exact inverse of the arms' controller placement (`vam_grip_to_model`):

    xr = head_pos + head_rot . Z( conj(cam_rot) . (p_model - cam_pos) )      Z = (x, y, -z)

Here `cam_pos/cam_rot` is the first-person camera in the character's model space
(`vam_camera_in_model`), and `head_*` is the OpenXR head pose that camera showed. A world point first goes
to model space with `vr_world_to_model(body_pos, yaw)`. The bbhitbox data is in FromSoftware model space
with X mirrored, so a data point `j` is `X(j) = (-x, y, z)` in the game. `vhb_affine` folds X, the bone
transform and the way to the headset into one matrix. Because the two mirrors cancel, `det = +1`.

- Weapons: the bone frame `now(R_Weapon)`. The weapon's model space is assumed to be the `R_Weapon` frame,
  the same assumption the sandbox makes with a zero `weapon_grip`. Body dummy 300, where the weapon hangs,
  sits on `R_Weapon` with pos_local ~0.
- Body points and hurtboxes: the skinning transform `now(bone) . inverse(bind(bone))`, applied to the
  data's bind-pose model-space points (`a`/`b`/`pos`). This avoids the pos_local mirroring convention.

The tests check:

- a model point sent to the headset comes back through `vam_xr_in_head_game` and the camera;
- camera forward/up become head -Z/+Y;
- the affine matrix equals the point-by-point path, is rigid and has det 1;
- skinning at bind is the identity, and a moved bone moves the point;
- world and model paths agree;
- the FNV hash values are right.

`bbport_xr_game_ui_test` (TestHitboxes) runs the bridge side on the real data. It checks the id mapping,
the idle, attack and flash looks, the hit marker and the c0000 hurtboxes forming a standing body.

## 4. Enable / configure

- Requirements: `BB_VR=1`, `BB_VR_UI=1`, first person with head tracking (the H1 camera). `BB_VR_ARMS` is
  not needed, but with it the weapon bone is the IK-posed one you see.
- The attack highlight and flash need the stamina tickets: `BB_VR_STAMINA=defer|refund` and the hooks
  "VR Stamina Cost (behavior)" + "VR Stamina Hits" in `BB_HOOKS`. Hit markers need the same hooks.
- `BB_VR_HITBOX=0` sends nothing from the host. The overlay is on by default.
- `out/vr_assets/ui_layouts.json`, `"game"` object (hot reload; the defaults apply when the key is absent):
  `"debug_hitboxes": {"weapon": true, "attack": true, "enemies": true, "self": false, "hits": true, "line_mrad": 1.5, "idle_alpha": 0.6}`
- Log lines: `VR hitbox: player skeleton: N bones, L_Weapon a, R_Weapon b` and
  `game hitboxes: weapon <id> -> wp_a_XXXX: hitboxes ...: N attacks`.

## 5. Not done: enemy hurtboxes

No list of enemy characters is known yet. Nothing in src/ or docs/ walks WorldChrMan's character array.
The lock-on manager `[0x593E860]` (+8 linked list, node +0x90 flag 0x20, node +0x00 point) gives only a
target point, not a ChrIns. There is also no known way from a ChrIns to its `cXXXX` model id, which picks
`chr_cXXXX.json`. Possible ways in:

1. Reverse WorldChrMan's character list. The player is at +0x60; the list layout is unknown.
2. Collect enemy ChrIns from the hit hooks: the attacker `rdx` when it is not the player, and the victim
   (likely `rsi` at 0x1cb79e0, unverified). Then read the same pose chain (+0x3B0 ...) and physics position
   (`[bag+0x68]+0x1E0`, yaw +0x1D4).
3. Find the chr id from the ChrIns (possibly from the model resource at +0x48) or match its skeleton's bone
   count/names against `chr_*.json`.

Once the ChrIns, chr id and pose are known, the host can send another bone block per enemy (the same skin
matrices; `vhb_world_to_xr` for the enemy's own model -> world), and `GameHitboxes` can draw
`LoadHurtboxes(chr)` exactly like `self`. The config key is already there.

## 6. Risks / to verify in the headset

- **Weapon frame:** the capsules might be offset or rotated from the visible blade if the game hangs the
  weapon at dummy 300's orientation rather than the bone's. If so, add a fixed per-hand correction.
- **Hit point:** +0x180 is verified only for floor contacts (findings/20). For character hits it is
  unverified, so markers may appear at the wrong place. Points more than 15 m from the player are dropped.
- **Timing:** the pose is read on the pad thread while animation may write it, which can give a torn bone
  for a frame. The overlay updates at the game's frame rate and the UI layer at the headset's, so it lags
  slightly when the head moves fast.
- **Spaces:** the head pose given to the H1 camera (`bbgpu_vr_views` space) is assumed to be the UI layer's
  space. That is true for the arms, which line up.
- **Size:** the state grew by ~7 KB. It is copied each pad poll; this is negligible.
- **Data:** the radii are the extractor's. The Saw Cleaver's 0.4 m looks large; the sandbox shows the same.
