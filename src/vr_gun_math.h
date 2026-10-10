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
