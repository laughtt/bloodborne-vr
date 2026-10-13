/* Pure math for VR gun aim (BB_VR_GUN_AIM=1, src/runtime_vr_gun.c, docs/vr/findings/27-gun-aim.md):
 * the controller's pointing ray in the game world, and the turn that moves a bullet's launch from
 * the game's direction onto it. No I/O, no globals: tests/test_vr_gun.c checks it.
 *
 * Conventions (vr_head_math.h): OpenXR poses are right-handed, +X right, +Y up, -Z forward; the
 * game camera is four float4 rows right, up, forward, position in the world (+Y up), and a vector
 * goes from the XR frame to the camera's frame by mirroring Z.
 *
 * The bullet launch (findings/27 2): SprjBulletIns's launch init 0x1df6ca0 takes a 3x4 matrix of
 * three float4 rows: row i = (axis X [i], axis Y [i], axis Z [i], position [i]). The bullet starts at
 * the position (the 4th column) and flies along axis Z (the 3rd column, normalized, times the
 * BulletParam speed). Its target (0x34 bytes) has the aim point at +0x20 when it is a plain point
 * (+0x00 handle -1, +0x30 kind 4: the point dist meters ahead of the character); a lock-on target
 * has a handle.
 */
#ifndef VR_GUN_MATH_H
#define VR_GUN_MATH_H
#include "vr_arm_math.h"

/* The pointing direction of an XR pose (its -Z) in the game world: relative to the head of the same
 * snapshot, mirrored into the camera's frame, then through the first-person camera rows (right,
 * up, forward) that show that head. Unit length; (0,0,0) for a degenerate input. */
static inline VrVec3 vg_aim_in_world(VrQuat head_xr, VrQuat pose_xr, const float rows[3][4]) {
    const VrVec3 fwd_xr = vr_qrotate(vr_qnormalize(pose_xr), vr_v3(0, 0, -1));
    const VrVec3 rel = vr_xr_to_game_vec(vr_qrotate(vr_qconj(vr_qnormalize(head_xr)), fwd_xr));
    const VrVec3 r = vr_v3(rows[0][0], rows[0][1], rows[0][2]);
    const VrVec3 u = vr_v3(rows[1][0], rows[1][1], rows[1][2]);
    const VrVec3 f = vr_v3(rows[2][0], rows[2][1], rows[2][2]);
    const VrVec3 w = vr_add(vr_add(vr_scale(r, rel.x), vr_scale(u, rel.y)), vr_scale(f, rel.z));
    return vam_norm(w, vr_v3(0, 0, 0));
}

/* A vector given in the head's frame (OpenXR axes: +X right, +Y up, -Z forward; the bridge's
 * conj(head) * v) in the game world, through the same first-person camera rows as vg_aim_in_world.
 * A direction needs no more; a position adds the camera's position (vg_rel_pos_to_world). */
static inline VrVec3 vg_rel_dir_to_world(VrVec3 rel_xr, const float rows[3][4]) {
    const VrVec3 rel = vr_xr_to_game_vec(rel_xr);
    const VrVec3 r = vr_v3(rows[0][0], rows[0][1], rows[0][2]);
    const VrVec3 u = vr_v3(rows[1][0], rows[1][1], rows[1][2]);
    const VrVec3 f = vr_v3(rows[2][0], rows[2][1], rows[2][2]);
    return vr_add(vr_add(vr_scale(r, rel.x), vr_scale(u, rel.y)), vr_scale(f, rel.z));
}
static inline VrVec3 vg_rel_pos_to_world(VrVec3 rel_xr, const float rows[3][4], VrVec3 cam_pos) {
    return vr_add(cam_pos, vg_rel_dir_to_world(rel_xr, rows));
}

/* An XR pose turned by `pitch_deg` about its own +X (positive: the ray up), for a grip used as the
 * pointer when the runtime gives no aim pose. */
static inline VrQuat vg_pitched(VrQuat pose_xr, float pitch_deg) {
    return vr_qnormalize(vr_qmul(pose_xr, vr_qaxis(vr_v3(1, 0, 0), pitch_deg * VR_DEG)));
}

/* Heading (degrees, 0 along world +Z, +90 along +X) and pitch (degrees, up positive) of a world
 * direction, for the logs. */
static inline void vg_yaw_pitch(VrVec3 d, float *yaw_deg, float *pitch_deg) {
    const VrVec3 n = vam_norm(d, vr_v3(0, 0, 1));
    *yaw_deg = atan2f(n.x, n.z) / VR_DEG;
    *pitch_deg = asinf(vr_clampf(n.y, -1.0f, 1.0f)) / VR_DEG;
}

/* The difference of two headings in degrees, 0..180. */
static inline float vg_heading_diff_deg(float a_deg, float b_deg) {
    float d = fmodf(a_deg - b_deg, 360.0f);
    if (d < -180.0f) d += 360.0f;
    if (d > 180.0f) d -= 360.0f;
    return fabsf(d);
}

/* The physics yaw (PlayerIns physics +0x1D4) that makes the body face `heading` (radians, vg_yaw_pitch's
 * convention): the face is the model's -Z, so the body looks along yaw + pi (vr_fp_math.h vr_facing_heading).
 * The throw turns the body to its aim this way: the game launches a thrown item along the body. */
static inline float vg_body_yaw_for_heading(float heading) {
    float y = fmodf(heading - VR_PI, 2.0f * VR_PI);
    if (y < -VR_PI) y += 2.0f * VR_PI;
    if (y > VR_PI) y -= 2.0f * VR_PI;
    return y;
}

/* The angle (degrees) between two directions. */
static inline float vg_angle_deg(VrVec3 a, VrVec3 b) {
    const VrVec3 x = vam_norm(a, vr_v3(0, 0, 1)), y = vam_norm(b, vr_v3(0, 0, 1));
    return acosf(vr_clampf(vr_dot(x, y), -1.0f, 1.0f)) / VR_DEG;
}

/* The turn taking unit `from` onto unit `to` as a pitch in from's vertical plane followed by a yaw
 * about world +Y: no roll, so a spread pattern around `from` (a shotgun's pellets) stays level.
 * Falls back to the shortest rotation when either direction is (nearly) vertical. */
static inline VrQuat vg_turn(VrVec3 from, VrVec3 to) {
    const VrVec3 hf = vr_v3(from.x, 0.0f, from.z), ht = vr_v3(to.x, 0.0f, to.z);
    const float lf = vam_len(hf), lt = vam_len(ht);
    if (lf < 1e-3f || lt < 1e-3f) return vam_from_to(from, to);
    const VrVec3 hfn = vr_scale(hf, 1.0f / lf), htn = vr_scale(ht, 1.0f / lt);
    /* from's heading with to's pitch */
    const VrVec3 mid = vr_add(vr_scale(hfn, lt), vr_v3(0.0f, to.y, 0.0f));
    const VrQuat pitch = vam_from_to(from, vam_norm(mid, from));
    const VrQuat yaw = vr_dot(hfn, htn) < -0.99999f ? vr_qaxis(vr_v3(0, 1, 0), VR_PI) : vam_from_to(hfn, htn);
    return vr_qnormalize(vr_qmul(yaw, pitch));
}

/* The launch matrix (3 float4 rows) as vectors. */
static inline VrVec3 vg_launch_axis(const float m[3][4], int col) { return vr_v3(m[0][col], m[1][col], m[2][col]); }
static inline VrVec3 vg_launch_origin(const float m[3][4]) { return vg_launch_axis(m, 3); }
/* Turns the launch's axes by q about its origin (the position column is kept). */
static inline void vg_launch_turn(float m[3][4], VrQuat q) {
    for (int c = 0; c < 3; ++c) {
        const VrVec3 v = vr_qrotate(q, vg_launch_axis((const float (*)[4])m, c));
        m[0][c] = v.x;
        m[1][c] = v.y;
        m[2][c] = v.z;
    }
}

/* Where the game aims the shot: the direction to its aim point when it has one (`target` non-NULL,
 * more than 5 cm from the muzzle) and that direction is within `max_off_deg` of the launch's Z (the
 * bullet's own direction; a pellet of a spread is off by its spread angle); otherwise the launch's
 * Z. `used_target` (may be NULL) tells which. */
enum { VG_REF_AUTO = 0, VG_REF_TARGET = 1, VG_REF_FORWARD = 2 };
static inline VrVec3 vg_game_direction(const float m[3][4], const float *target, int mode, float max_off_deg,
                                       int *used_target) {
    const VrVec3 z = vam_norm(vg_launch_axis(m, 2), vr_v3(0, 0, 1));
    if (used_target) *used_target = 0;
    if (!target || mode == VG_REF_FORWARD) return z;
    const VrVec3 t = vr_sub(vr_v3(target[0], target[1], target[2]), vg_launch_origin(m));
    if (vam_len(t) < 0.05f) return z;
    const VrVec3 tn = vam_norm(t, z);
    if (mode == VG_REF_AUTO && vg_angle_deg(tn, z) > max_off_deg) return z;
    if (used_target) *used_target = 1;
    return tn;
}

/* The redirect of one launch: turns the matrix so the game's direction (vg_game_direction) becomes
 * `aim` (unit, world), and moves the aim point (`target`, may be NULL) the same way about the
 * muzzle, keeping its distance. Returns the turn's angle in degrees; *old_dir / *new_dir (may be
 * NULL) receive the bullet's own direction (the launch's Z) before and after. */
static inline float vg_redirect(float m[3][4], float *target, VrVec3 aim, int mode, float max_off_deg, int *used_target,
                                VrVec3 *old_dir, VrVec3 *new_dir) {
    const float (*cm)[4] = (const float (*)[4])m;
    const VrVec3 game = vg_game_direction(cm, target, mode, max_off_deg, used_target);
    const VrQuat q = vg_turn(game, aim);
    if (old_dir) *old_dir = vam_norm(vg_launch_axis(cm, 2), vr_v3(0, 0, 1));
    vg_launch_turn(m, q);
    if (target) {
        const VrVec3 o = vg_launch_origin(cm);
        const VrVec3 p = vr_add(o, vr_qrotate(q, vr_sub(vr_v3(target[0], target[1], target[2]), o)));
        target[0] = p.x;
        target[1] = p.y;
        target[2] = p.z;
    }
    if (new_dir) *new_dir = vam_norm(vg_launch_axis(cm, 2), vr_v3(0, 0, 1));
    return vg_angle_deg(game, aim);
}

/* The throw mode of the launch hook (findings/39): a thrown item leaves the hand, not the character's
 * muzzle, along the release direction. Turns the launch matrix so its Z axis is `dir` (unit, world)
 * with no roll (vg_redirect's pitch-then-yaw) and moves its position to `origin`, unless that is
 * more than `max_shift` meters from the game's own muzzle (a wrong camera/world assumption then
 * keeps the game's origin: *moved = 0). The aim point (`target`, 3 floats, may be NULL: a lock-on
 * has none) goes `dir` x its old distance from the muzzle (10 m when it had none) ahead of the new
 * origin. Returns the turn's angle in degrees; *old_dir / *new_dir (may be NULL) the launch's own
 * direction before and after. */
static inline float vg_throw_launch(float m[3][4], float *target, VrVec3 origin, VrVec3 dir, float max_shift, int *moved,
                                    VrVec3 *old_dir, VrVec3 *new_dir) {
    const VrVec3 old_o = vg_launch_origin((const float (*)[4])m);
    float dist = 10.0f;
    if (target) {
        const float d = vam_len(vr_sub(vr_v3(target[0], target[1], target[2]), old_o));
        if (d >= 0.05f && d <= 100.0f) dist = d;
    }
    const float turn = vg_redirect(m, NULL, dir, VG_REF_FORWARD, 0.0f, NULL, old_dir, new_dir);
    const int ok = vam_len(vr_sub(origin, old_o)) <= max_shift;
    if (moved) *moved = ok;
    const VrVec3 o = ok ? origin : old_o;
    m[0][3] = o.x;
    m[1][3] = o.y;
    m[2][3] = o.z;
    if (target) {
        const VrVec3 p = vr_add(o, vr_scale(dir, dist));
        target[0] = p.x;
        target[1] = p.y;
        target[2] = p.z;
    }
    return turn;
}

/* A throw that spawns several bullets (the rope molotov's chain 110/111/112, the gloves' fan, a knife
 * pair): the first bullet's turn (its Z onto the release direction) and origin shift are kept and applied
 * to every later bullet of the same throw, so their spread relative to each other is unchanged. */
typedef struct {
    VrQuat q;        /* the first bullet's turn */
    VrVec3 old_o;    /* the first bullet's launch origin before the move */
    VrVec3 shift;    /* hand - old_o (zero when the hand was too far: origin kept) */
    int moved;
} VgThrowGroup;
/* Built from the first bullet's launch matrix BEFORE vg_throw_launch changes it (same turn and reach rule). */
static inline VgThrowGroup vg_throw_group_make(const float m[3][4], VrVec3 origin, VrVec3 dir, float max_shift) {
    VgThrowGroup g;
    const VrVec3 z = vam_norm(vg_launch_axis(m, 2), vr_v3(0, 0, 1));
    g.q = vg_turn(z, dir);
    g.old_o = vg_launch_origin(m);
    g.moved = vam_len(vr_sub(origin, g.old_o)) <= max_shift;
    g.shift = g.moved ? vr_sub(origin, g.old_o) : vr_v3(0, 0, 0);
    return g;
}
/* A later bullet of the group: its axes turned by the group's turn; its origin shifted by the group's
 * shift only when it spawns within `near_m` of the first bullet's old origin (a bullet born elsewhere,
 * e.g. at the impact, keeps its place). The aim point (may be NULL) is turned about the old origin and
 * shifted the same way. Returns the turn in degrees; *old_dir / *new_dir (may be NULL) the Z before / after. */
static inline float vg_throw_group_apply(float m[3][4], float *target, const VgThrowGroup *g, float near_m, int *moved,
                                         VrVec3 *old_dir, VrVec3 *new_dir) {
    const VrVec3 o = vg_launch_origin((const float (*)[4])m);
    const VrVec3 z0 = vam_norm(vg_launch_axis((const float (*)[4])m, 2), vr_v3(0, 0, 1));
    if (old_dir) *old_dir = z0;
    vg_launch_turn(m, g->q);
    const int shift = g->moved && vam_len(vr_sub(o, g->old_o)) <= near_m;
    const VrVec3 no = shift ? vr_add(o, g->shift) : o;
    m[0][3] = no.x;
    m[1][3] = no.y;
    m[2][3] = no.z;
    if (target) {
        const VrVec3 p = vr_add(no, vr_qrotate(g->q, vr_sub(vr_v3(target[0], target[1], target[2]), o)));
        target[0] = p.x;
        target[1] = p.y;
        target[2] = p.z;
    }
    const VrVec3 z1 = vam_norm(vg_launch_axis((const float (*)[4])m, 2), vr_v3(0, 0, 1));
    if (new_dir) *new_dir = z1;
    if (moved) *moved = shift;
    return vg_angle_deg(z0, z1);
}

/* The throw speed from the hand (findings/39 1A option ii): the factor on the game's projectile speed
 * for a release at `speed_mps` (the hand's speed relative to the head), clamp(speed / ref, min, max).
 * No speed (<= 0) or a bad reference leaves the game's speed (1). */
static inline float vg_throw_speed_factor(float speed_mps, float ref, float lo, float hi) {
    if (!(speed_mps > 0.0f) || !(ref > 0.0f) || !isfinite(speed_mps)) return 1.0f;
    return vr_clampf(speed_mps / ref, lo, hi);
}

/* Gun parry aim assist (docs/vr/findings/41-parry.md). The game's lock-on candidate list gives enemy
 * points; the one the ray passes closest to (in angle, within `range` m of the muzzle) pulls the ray
 * toward itself. The cone grows as the enemy gets close, where a 5 degree miss is more than the body:
 *   cone(d) = clamp(atan(radius / d), cone_min, cone_max)             degrees
 *   w = strength * (1 - smoothstep(inner * cone, cone, angle))        0 outside the cone
 *   dir' = normalize(lerp(ray, to_target, w))
 * so a ray inside the inner part of the cone (inner = 0.4) is pulled strength of the way, fading to
 * nothing at the cone's edge. strength 0 turns it off; 1 snaps onto the point. */
typedef struct {
    float radius, cone_min, cone_max, strength, inner, range;
} VgAssist;
static inline VgAssist vg_assist_defaults(void) {
    VgAssist a = {0.6f, 4.0f, 18.0f, 0.8f, 0.4f, 12.0f};
    return a;
}
static inline float vg_assist_cone(const VgAssist *a, float dist) {
    const float c = atan2f(a->radius, dist > 0.1f ? dist : 0.1f) / VR_DEG;
    return vr_clampf(c, a->cone_min, a->cone_max);
}
/* Returns the new unit direction. *picked = the index of the pulling target (-1: none), *angle = the
 * ray's angle to it before the pull (degrees; may be NULL). */
static inline VrVec3 vg_assist_dir(VrVec3 ray, VrVec3 muzzle, const VrVec3 *targets, int n, const VgAssist *a, int *picked,
                                   float *angle) {
    int best = -1;
    float best_angle = 1e9f, best_w = 0.0f;
    VrVec3 best_dir = ray;
    ray = vam_norm(ray, ray);
    for (int i = 0; i < n; ++i) {
        const VrVec3 d = vr_sub(targets[i], muzzle);
        const float dist = vam_len(d);
        if (!(dist > 0.3f) || dist > a->range) continue;
        const VrVec3 dn = vr_scale(d, 1.0f / dist);
        const float ang = vg_angle_deg(ray, dn);
        const float cone = vg_assist_cone(a, dist);
        if (ang >= cone || ang >= best_angle) continue;
        const float lo = a->inner * cone;
        float t = vr_clampf((ang - lo) / (cone - lo), 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);
        best = i;
        best_angle = ang;
        best_w = a->strength * (1.0f - t);
        best_dir = dn;
    }
    if (picked) *picked = best;
    if (angle) *angle = best < 0 ? 0.0f : best_angle;
    if (best < 0 || best_w <= 0.0f) return ray;
    return vam_norm(vam_lerp(ray, best_dir, vr_clampf(best_w, 0.0f, 1.0f)), ray);
}

/* ---- the barrel source and the live trims (findings/27 section 11) ----
 * The visible gun hangs off the L_Weapon bone, which the arms IK places at grip * offset * trim
 * (runtime_vr_arms.c arms_target: arm_offset, arm_rot_l). The controller's aim ray ignores that
 * offset and trim, so with arm_rot_l=30,25 the bullets left about 30 deg left / 25 deg below the
 * barrel the player sees. The barrel source takes the direction from the weapon bone after the IK:
 * its rotation relative to the H1 camera (vg_weapon_in_cam, published by the arms module) and an
 * axis of the bone (-Z: the weapon models' barrel, the pistol's muzzle dummy at z -0.44, findings/13
 * 7.3; the sandbox's guns aim the model's -Z), through the same camera rows as the aim ray. */
enum { VG_SRC_BARREL = 0, VG_SRC_AIM = 1, VG_SRC_GRIP = 2 };
static inline const char *vg_src_name(int s) { return s == VG_SRC_AIM ? "aim" : s == VG_SRC_GRIP ? "grip" : "barrel"; }
/* "barrel" | "aim" | "grip" (case sensitive); -1 for anything else. */
static inline int vg_src_parse(const char *s) {
    if (!s) return -1;
    return !strcmp(s, "barrel") ? VG_SRC_BARREL : !strcmp(s, "aim") ? VG_SRC_AIM : !strcmp(s, "grip") ? VG_SRC_GRIP : -1;
}
/* A bone axis "+x" "-x" "x" "+y" "-y" "y" "+z" "-z" "z" (no sign = +); 1 when it parsed. */
static inline int vg_axis_parse(const char *s, VrVec3 *out) {
    if (!s) return 0;
    if (!strcmp(s, "auto")) { *out = vr_v3(0, 0, 0); return 1; } /* gun_axis=auto: picked per lock (vg_best_axis) */
    float sign = 1.0f;
    if (*s == '-' || *s == '+') sign = *s++ == '-' ? -1.0f : 1.0f;
    if (!s[0] || s[1]) return 0;
    switch (s[0]) {
    case 'x': case 'X': *out = vr_v3(sign, 0, 0); return 1;
    case 'y': case 'Y': *out = vr_v3(0, sign, 0); return 1;
    case 'z': case 'Z': *out = vr_v3(0, 0, sign); return 1;
    default: return 0;
    }
}
/* A direction in the game camera's frame (+x right, +y up, +z forward; vr_xr_to_game_vec of an XR
 * vector) in the world, through the camera rows (right, up, forward). */
static inline VrVec3 vg_cam_dir_to_world(VrVec3 d, const float rows[3][4]) {
    const VrVec3 r = vr_v3(rows[0][0], rows[0][1], rows[0][2]);
    const VrVec3 u = vr_v3(rows[1][0], rows[1][1], rows[1][2]);
    const VrVec3 f = vr_v3(rows[2][0], rows[2][1], rows[2][2]);
    return vr_add(vr_add(vr_scale(r, d.x), vr_scale(u, d.y)), vr_scale(f, d.z));
}
/* A bone's model rotation relative to the camera: cam_rot is the camera in model space
 * (vam_camera_in_model: camera frame -> model), so this is camera frame <- bone frame. */
static inline VrQuat vg_weapon_in_cam(VrQuat cam_rot, VrQuat bone_model) {
    return vr_qnormalize(vr_qmul(vr_qconj(cam_rot), bone_model));
}
/* The barrel in the world: the bone's `axis` (bone frame) through its camera-relative rotation and
 * the camera rows. Unit; (0,0,0) for a degenerate input. */
static inline VrVec3 vg_barrel_in_world(VrQuat weapon_cam, VrVec3 axis, const float rows[3][4]) {
    return vam_norm(vg_cam_dir_to_world(vr_qrotate(vr_qnormalize(weapon_cam), axis), rows), vr_v3(0, 0, 0));
}
/* gun_axis=auto: the six bone axes (+x -x +y -y +z -z) through vg_barrel_in_world; returns the index of the one
 * closest to the controller ray `ray` (unit) and fills angles[6] (deg, -1 for a degenerate axis); -1 if none. */
static const char *const vg_axis_names[6] = {"+x", "-x", "+y", "-y", "+z", "-z"};
static inline int vg_best_axis(VrQuat weapon_cam, const float rows[3][4], VrVec3 ray, float angles[6]) {
    static const float ax[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int best = -1;
    for (int i = 0; i < 6; ++i) {
        const VrVec3 b = vg_barrel_in_world(weapon_cam, vr_v3(ax[i][0], ax[i][1], ax[i][2]), rows);
        angles[i] = vam_len(b) < 0.5f ? -1.0f : vg_angle_deg(b, ray);
        if (angles[i] >= 0.0f && (best < 0 || angles[i] < angles[best])) best = i;
    }
    return best;
}
/* The live trim: the heading turned by yaw_deg (+ = right: vg_yaw_pitch's heading grows toward
 * world +X, which is the camera's right when it looks along +Z) and the pitch raised by pitch_deg
 * (clamped to +-89 deg). Zero trims return the direction unchanged. */
static inline VrVec3 vg_trim(VrVec3 dir, float yaw_deg, float pitch_deg) {
    if (yaw_deg == 0.0f && pitch_deg == 0.0f) return dir;
    float h, p;
    vg_yaw_pitch(dir, &h, &p);
    h = (h + yaw_deg) * VR_DEG;
    p = vr_clampf(p + pitch_deg, -89.0f, 89.0f) * VR_DEG;
    return vr_v3(cosf(p) * sinf(h), sinf(p), cosf(p) * cosf(h));
}
/* The arms IK's per-hand trim (arm_rot_l / arm_rot_r) as a rotation in the controller frame
 * (OpenXR: yaw right = about -Y, pitch up = about +X), the same product arms_target applies. */
static inline VrQuat vg_hand_trim(float yaw_deg, float pitch_deg) {
    return vr_qnormalize(vr_qmul(vr_qaxis(vr_v3(0, 1, 0), -yaw_deg * VR_DEG), vr_qaxis(vr_v3(1, 0, 0), pitch_deg * VR_DEG)));
}
/* `key=<float>` as a whole token anywhere in a one-line control text (fp.txt: space separated
 * key=value tokens); 1 and the value when present and finite. */
static inline int vg_key_float(const char *text, const char *key, float *out) {
    const size_t n = strlen(key);
    for (const char *p = text; p && (p = strstr(p, key)) != NULL; p += n) {
        if ((p != text && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r') || p[n] != '=') continue;
        const char c = p[n + 1];
        if (!c || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue; /* "key=" with no value */
        char *end;
        const float x = strtof(p + n + 1, &end);
        if (end != p + n + 1 && isfinite(x)) { *out = x; return 1; }
    }
    return 0;
}
/* `key=<word>` likewise; the word (up to the next blank, at most cap - 1 chars) into out. */
static inline int vg_key_word(const char *text, const char *key, char *out, size_t cap) {
    const size_t n = strlen(key);
    for (const char *p = text; p && (p = strstr(p, key)) != NULL; p += n) {
        if ((p != text && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r') || p[n] != '=') continue;
        const char *v = p + n + 1;
        size_t k = 0;
        while (v[k] && v[k] != ' ' && v[k] != '\t' && v[k] != '\r' && v[k] != '\n') ++k;
        if (!k || k >= cap) return 0;
        memcpy(out, v, k);
        out[k] = 0;
        return 1;
    }
    return 0;
}

/* Bullet id filter: "all", or comma separated ids and ranges ("500-999,1100"). Returns 1 when `id`
 * is in the list; an empty or unparsable list matches nothing. */
static inline int vg_id_in_list(const char *list, int id) {
    if (!list) return 0;
    if (!strcmp(list, "all")) return 1;
    const char *s = list;
    while (*s) {
        char *end;
        const long lo = strtol(s, &end, 10);
        if (end == s) return 0;
        long hi = lo;
        s = end;
        if (*s == '-') {
            hi = strtol(s + 1, &end, 10);
            if (end == s + 1) return 0;
            s = end;
        }
        if (id >= lo && id <= hi) return 1;
        while (*s == ',' || *s == ' ') ++s;
    }
    return 0;
}

#endif
