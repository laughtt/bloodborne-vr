/* Pure math for VR head tracking on Bloodborne's render camera (docs/vr/PLAN.md Phase 2).
 * No I/O, no globals: src/runtime_vr_head.c uses it from the H1/H2 camera hooks and
 * tests/test_vr_head.c checks it.
 *
 * Conventions (docs/vr/findings/08 section 3, confirmed from the disassembly):
 *  - OpenXR poses (bbgpu_vr_views): right-handed, +X right, +Y up, -Z forward, meters,
 *    quaternions stored x, y, z, w.
 *  - The game camera (PersCam +0x10..+0x4f; the draw camera "format B" +0x00..+0x3f) is a
 *    camera-to-world matrix of four float4 rows: right, up, forward (the view axis), position
 *    (w = 1). Row-vector convention (world = local . M), left-handed like D3D: in the camera's
 *    frame +X is right, +Y up, +Z forward; the world is +Y up. The projection the game builds from
 *    it (0x25aa6c0) is LH with depth 0..1: clip = (x.sx, y.sy, z.A + B, z), sy = cot(fov_y / 2),
 *    sx = sy / aspect.
 *  - A rotation moves from the XR frame to the camera's frame by mirroring Z: the quaternion
 *    (x, y, z, w) becomes (-x, -y, z, w), a vector (x, y, z) becomes (x, y, -z).
 *  - Head yaw is positive to the left (counterclockwise seen from above, about +Y), pitch
 *    positive up (about +X), roll positive with the head tilted left (about +Z), as in OpenXR.
 */
#ifndef VR_HEAD_MATH_H
#define VR_HEAD_MATH_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VR_PI 3.14159265358979f
#define VR_DEG (VR_PI / 180.0f)

typedef struct { float x, y, z, w; } VrQuat;
typedef struct { float x, y, z; } VrVec3;
/* Tangent-free FOV angles in radians from the view axis, as OpenXR reports them: left and down
 * are negative. */
typedef struct { float left, right, up, down; } VrFov;

static inline VrVec3 vr_v3(float x, float y, float z) { VrVec3 v = {x, y, z}; return v; }
static inline VrVec3 vr_add(VrVec3 a, VrVec3 b) { return vr_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline VrVec3 vr_sub(VrVec3 a, VrVec3 b) { return vr_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline VrVec3 vr_scale(VrVec3 a, float s) { return vr_v3(a.x * s, a.y * s, a.z * s); }
static inline float vr_dot(VrVec3 a, VrVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline VrVec3 vr_cross(VrVec3 a, VrVec3 b) {
    return vr_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float vr_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline VrQuat vr_q(float x, float y, float z, float w) { VrQuat q = {x, y, z, w}; return q; }
static inline VrQuat vr_qidentity(void) { return vr_q(0.0f, 0.0f, 0.0f, 1.0f); }
static inline VrQuat vr_qconj(VrQuat q) { return vr_q(-q.x, -q.y, -q.z, q.w); }
static inline float vr_qnorm2(VrQuat q) { return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w; }
static inline VrQuat vr_qmul(VrQuat a, VrQuat b) {
    return vr_q(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
/* Finite and close to unit length (OpenXR hands out unit quaternions; zeros mean "no pose"). */
static inline int vr_qvalid(VrQuat q) {
    const float n = vr_qnorm2(q);
    return isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w) && fabsf(n - 1.0f) < 0.02f;
}
static inline VrQuat vr_qnormalize(VrQuat q) {
    const float n = sqrtf(vr_qnorm2(q));
    return n > 0.0f ? vr_q(q.x / n, q.y / n, q.z / n, q.w / n) : vr_qidentity();
}
/* v rotated by the unit quaternion q (q v q*). */
static inline VrVec3 vr_qrotate(VrQuat q, VrVec3 v) {
    const VrVec3 u = vr_v3(q.x, q.y, q.z);
    const VrVec3 t = vr_scale(vr_cross(u, v), 2.0f);
    return vr_add(vr_add(v, vr_scale(t, q.w)), vr_cross(u, t));
}
static inline VrQuat vr_qaxis(VrVec3 unit_axis, float angle) {
    const float s = sinf(0.5f * angle);
    return vr_q(unit_axis.x * s, unit_axis.y * s, unit_axis.z * s, cosf(0.5f * angle));
}
/* Shortest-arc normalized lerp (two eye orientations -> the head's). */
static inline VrQuat vr_qnlerp(VrQuat a, VrQuat b, float t) {
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float s = d < 0.0f ? -1.0f : 1.0f;
    return vr_qnormalize(vr_q(a.x + (s * b.x - a.x) * t, a.y + (s * b.y - a.y) * t,
                              a.z + (s * b.z - a.z) * t, a.w + (s * b.w - a.w) * t));
}
/* Rotation matrix of a unit quaternion, column-vector convention: v' = m . v. Column j is the
 * rotated frame's axis j in the original frame. */
static inline void vr_qmat3(VrQuat q, float m[3][3]) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0][0] = 1.0f - 2.0f * (yy + zz); m[0][1] = 2.0f * (xy - wz);        m[0][2] = 2.0f * (xz + wy);
    m[1][0] = 2.0f * (xy + wz);        m[1][1] = 1.0f - 2.0f * (xx + zz); m[1][2] = 2.0f * (yz - wx);
    m[2][0] = 2.0f * (xz - wy);        m[2][1] = 2.0f * (yz + wx);        m[2][2] = 1.0f - 2.0f * (xx + yy);
}

/* ---- OpenXR side ---- */

/* Head orientation from yaw (left +), pitch (up +), roll (tilt left +), radians: yaw about +Y,
 * then pitch about the turned +X, then roll about the view axis. */
static inline VrQuat vr_xr_ypr(float yaw, float pitch, float roll) {
    return vr_qmul(vr_qmul(vr_qaxis(vr_v3(0, 1, 0), yaw), vr_qaxis(vr_v3(1, 0, 0), pitch)),
                   vr_qaxis(vr_v3(0, 0, 1), roll));
}
/* Yaw, pitch, roll (radians) of an OpenXR orientation (inverse of vr_xr_ypr away from +-90 deg
 * pitch). For logs and tests. */
static inline void vr_xr_angles(VrQuat q, float *yaw, float *pitch, float *roll) {
    const VrVec3 f = vr_qrotate(q, vr_v3(0, 0, -1));
    const VrVec3 r = vr_qrotate(q, vr_v3(1, 0, 0));
    const VrVec3 u = vr_qrotate(q, vr_v3(0, 1, 0));
    *pitch = asinf(vr_clampf(f.y, -1.0f, 1.0f));
    *yaw = atan2f(-f.x, -f.z);
    /* roll: the right vector's rise against the level right vector of this yaw */
    *roll = atan2f(r.y, u.y);
}
/* Heading of an OpenXR orientation: rotation about +Y (left +) of its view axis flattened onto
 * the floor. Stable for any pitch short of straight up or down; at the poles the up vector
 * gives the heading. */
static inline float vr_xr_yaw(VrQuat q) {
    const VrVec3 f = vr_qrotate(q, vr_v3(0, 0, -1));
    if (f.x * f.x + f.z * f.z > 1e-8f) return atan2f(-f.x, -f.z);
    const VrVec3 u = vr_qrotate(q, vr_v3(0, 1, 0));
    return f.y < 0.0f ? atan2f(-u.x, -u.z) : atan2f(u.x, u.z);
}
/* The yaw-only part of an orientation: a gravity-aligned reference ("forward" for recentering). */
static inline VrQuat vr_xr_yaw_only(VrQuat q) { return vr_qaxis(vr_v3(0, 1, 0), vr_xr_yaw(q)); }

/* ---- XR -> game camera ---- */

static inline VrQuat vr_xr_to_game_rot(VrQuat q) { return vr_q(-q.x, -q.y, q.z, q.w); }
static inline VrVec3 vr_xr_to_game_vec(VrVec3 v) { return vr_v3(v.x, v.y, -v.z); }

/* The head's rotation relative to the yaw-only reference `ref` (both OpenXR, same space),
 * expressed for the game camera's local frame. */
static inline VrQuat vr_head_rel_game(VrQuat head, VrQuat ref) {
    return vr_xr_to_game_rot(vr_qnormalize(vr_qmul(vr_qconj(ref), head)));
}
/* The head's displacement from `ref_pos` in the frame of the yaw-only reference `ref`, game axes
 * (+X right, +Y up, +Z forward), multiplied by `scale` and clamped to +-`max_h` meters
 * horizontally and +-`max_v` vertically. */
static inline VrVec3 vr_head_offset_game(VrVec3 head_pos, VrVec3 ref_pos, VrQuat ref, float scale,
                                         float max_h, float max_v) {
    VrVec3 d = vr_xr_to_game_vec(vr_qrotate(vr_qconj(ref), vr_sub(head_pos, ref_pos)));
    d = vr_scale(d, scale);
    const float h = sqrtf(d.x * d.x + d.z * d.z);
    if (h > max_h && h > 0.0f) { d.x *= max_h / h; d.z *= max_h / h; }
    d.y = vr_clampf(d.y, -max_v, max_v);
    return d;
}

/* ---- the game camera matrix (4 float4 rows: right, up, forward, position) ---- */

static inline VrVec3 vr_row(const float m[4][4], int i) { return vr_v3(m[i][0], m[i][1], m[i][2]); }
static inline void vr_set_row(float m[4][4], int i, VrVec3 v) { m[i][0] = v.x; m[i][1] = v.y; m[i][2] = v.z; }

/* Heading of the camera in the world (+Y up), radians about +Y with yaw 0 looking along world +Z
 * and positive toward +X (the game's own atan2(x, z) sense: a right turn in a left-handed,
 * +Y up world). Uses forward.y and up.y so it is exact for any pitch without roll. */
static inline float vr_cam_heading(const float m[4][4]) {
    const VrVec3 u = vr_row(m, 1), f = vr_row(m, 2);
    float hx = f.x * u.y - u.x * f.y, hz = f.z * u.y - u.z * f.y;
    if (hx * hx + hz * hz < 1e-12f) { hx = f.x; hz = f.z; }
    return atan2f(hx, hz);
}
/* The camera's yaw-only basis (level right, world up, level forward) for its heading, with the
 * camera's own handedness (right = up x forward for the game's det +1 cameras). */
static inline void vr_cam_level_basis(const float m[4][4], VrVec3 *right, VrVec3 *up, VrVec3 *fwd) {
    const float h = vr_cam_heading(m), s = sinf(h), c = cosf(h);
    const float det = vr_dot(vr_cross(vr_row(m, 0), vr_row(m, 1)), vr_row(m, 2));
    *right = vr_scale(vr_v3(c, 0.0f, -s), det < 0.0f ? -1.0f : 1.0f);
    *up = vr_v3(0.0f, 1.0f, 0.0f);
    *fwd = vr_v3(s, 0.0f, c);
}

/* Composes the head into the camera matrix in place (rows 0..3; the w lanes are kept):
 *  - rotation: `rel` (vr_head_rel_game) applied in the camera's own frame (level == 0: the game's
 *    pitch and roll stay, the head adds to them), or in the camera's yaw-only frame (level != 0:
 *    the horizon lock of findings/11 section 3.1, R = R_y(game heading) . R_head, so the
 *    headset alone sets pitch and roll);
 *  - position: `offset` (vr_head_offset_game, meters = game units) along the yaw-only basis. */
static inline void vr_cam_compose(float m[4][4], VrQuat rel, VrVec3 offset, int level) {
    VrVec3 lr, lu, lf;
    vr_cam_level_basis(m, &lr, &lu, &lf);
    VrVec3 base[3];
    if (level) { base[0] = lr; base[1] = lu; base[2] = lf; }
    else { base[0] = vr_row(m, 0); base[1] = vr_row(m, 1); base[2] = vr_row(m, 2); }
    float r[3][3];
    vr_qmat3(rel, r);
    for (int j = 0; j < 3; ++j)
        vr_set_row(m, j, vr_add(vr_add(vr_scale(base[0], r[0][j]), vr_scale(base[1], r[1][j])),
                                vr_scale(base[2], r[2][j])));
    const VrVec3 p = vr_row(m, 3);
    vr_set_row(m, 3, vr_add(p, vr_add(vr_add(vr_scale(lr, offset.x), vr_scale(lu, offset.y)),
                                      vr_scale(lf, offset.z))));
}

/* Largest deviation of the camera's rotation rows from an orthonormal basis, and its handedness
 * (det of right/up/forward: +1 for the game's cameras). For the log that verifies the layout. */
static inline float vr_cam_ortho_error(const float m[4][4], float *det) {
    const VrVec3 r = vr_row(m, 0), u = vr_row(m, 1), f = vr_row(m, 2);
    if (det) *det = vr_dot(vr_cross(r, u), f);
    const float e[6] = {vr_dot(r, r) - 1.0f, vr_dot(u, u) - 1.0f, vr_dot(f, f) - 1.0f,
                        vr_dot(r, u), vr_dot(r, f), vr_dot(u, f)};
    float worst = 0.0f;
    for (int i = 0; i < 6; ++i) worst = fabsf(e[i]) > worst ? fabsf(e[i]) : worst;
    return worst;
}

/* ---- FOV ---- */

/* The symmetric frustum around the head's view axis that contains both eyes' frusta: tangent
 * half-extents *tan_x, *tan_y. eye_in_head[i] is eye i's orientation relative to the head
 * (conj(head) . eye, OpenXR); canted views (Pimax) are rotated accordingly. The bound is taken
 * over each frustum's corners and edge midpoints. Returns 0 when no direction is usable. */
static inline int vr_union_tangents(const VrQuat eye_in_head[2], const VrFov fov[2], float *tan_x,
                                    float *tan_y) {
    float tx = 0.0f, ty = 0.0f;
    int used = 0;
    for (int e = 0; e < 2; ++e) {
        const float l = tanf(fov[e].left), r = tanf(fov[e].right);
        const float u = tanf(fov[e].up), d = tanf(fov[e].down);
        const float xs[3] = {l, 0.5f * (l + r), r}, ys[3] = {d, 0.5f * (d + u), u};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                if (i == 1 && j == 1) continue;
                const VrVec3 dir = vr_qrotate(eye_in_head[e], vr_v3(xs[i], ys[j], -1.0f));
                if (!(dir.z < -1e-3f)) continue;
                const float ax = fabsf(dir.x / -dir.z), ay = fabsf(dir.y / -dir.z);
                tx = ax > tx ? ax : tx;
                ty = ay > ty ? ay : ty;
                used = 1;
            }
    }
    if (!used || !(tx > 0.0f) || !(ty > 0.0f) || !isfinite(tx) || !isfinite(ty)) return 0;
    *tan_x = tx;
    *tan_y = ty;
    return 1;
}
/* Overscan: grow the tangent half-extents by the fractions ov_x / ov_y (0.15 = 15%, clamped to 0..1)
 * so a reprojected old image still covers the display when the head turns. */
static inline void vr_overscan_tangents(float *tan_x, float *tan_y, float ov_x, float ov_y) {
    ov_x = ov_x < 0.0f ? 0.0f : ov_x > 1.0f ? 1.0f : ov_x;
    ov_y = ov_y < 0.0f ? 0.0f : ov_y > 1.0f ? 1.0f : ov_y;
    *tan_x *= 1.0f + ov_x;
    *tan_y *= 1.0f + ov_y;
}
/* The game's camera parameters for a symmetric frustum: vertical FOV (radians) and aspect. */
static inline void vr_fov_from_tangents(float tan_x, float tan_y, float *fov_y, float *aspect) {
    *fov_y = 2.0f * atanf(tan_y);
    *aspect = tan_x / tan_y;
}

/* ---- the fake head (BB_VR_FAKE_HEAD) for headset-free tests ---- */

enum { VR_FAKE_OFF = 0, VR_FAKE_FIXED = 1, VR_FAKE_SWEEP = 2, VR_FAKE_FILE = 3 };
typedef struct {
    int kind;
    float yaw, pitch, roll;  /* degrees (fixed, and the file's last values) */
    float x, y, z;           /* meters, OpenXR axes (fixed and file) */
    float amp_yaw, amp_pitch, period; /* sweep: degrees, degrees, seconds */
    char path[260];          /* file */
} VrFakeHead;

/* Up to `max` comma- or space-separated floats from s; returns how many, -1 on garbage. */
static inline int vr_parse_floats(const char *s, float *out, int max) {
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
        if (!*s) break;
        char *end;
        const float v = strtof(s, &end);
        if (end == s || n >= max || !isfinite(v)) return -1;
        out[n++] = v;
        s = end;
    }
    return n;
}
/* BB_VR_FAKE_HEAD: "yaw,pitch[,roll[,x,y,z]]" (degrees, meters), "sweep[:yaw_amp,pitch_amp,
 * period_s]" (a slow sinusoid, default 30,10,8), or "@file" (re-read when it changes; see
 * runtime_vr_head.c). Returns 1 when understood. */
static inline int vr_fake_parse(const char *s, VrFakeHead *out) {
    memset(out, 0, sizeof(*out));
    if (!s || !*s) return 0;
    if (s[0] == '@') {
        const size_t n = strlen(s + 1);
        if (!n || n >= sizeof(out->path)) return 0;
        memcpy(out->path, s + 1, n + 1);
        out->kind = VR_FAKE_FILE;
        return 1;
    }
    if (!strncmp(s, "sweep", 5)) {
        float v[3] = {30.0f, 10.0f, 8.0f};
        if (s[5] == ':') {
            const int n = vr_parse_floats(s + 6, v, 3);
            if (n < 1) return 0;
        } else if (s[5]) {
            return 0;
        }
        if (!(v[2] > 0.1f)) return 0;
        out->kind = VR_FAKE_SWEEP;
        out->amp_yaw = v[0]; out->amp_pitch = v[1]; out->period = v[2];
        return 1;
    }
    float v[6] = {0};
    const int n = vr_parse_floats(s, v, 6);
    if (n < 1) return 0;
    out->kind = VR_FAKE_FIXED;
    out->yaw = v[0]; out->pitch = v[1]; out->roll = v[2];
    out->x = v[3]; out->y = v[4]; out->z = v[5];
    return 1;
}
/* The "@file" fake head: whitespace-separated tokens, re-read by the hook when the file changes,
 * so one game launch can run several experiments (tools/vr_head_test.py). Bare numbers fill yaw,
 * pitch, roll (degrees), x, y, z (meters) in order; key=value tokens set: yaw= pitch= roll= x= y=
 * z=, apply= (1 H1 only, 2 H2 only, 3 both, 0 none), fov= (vertical degrees, 0 = no override),
 * aspect= (0 = keep), level= (0/1, -1 = BB_VR_HEAD_LEVEL), lod= (0: no LOD fix). Unknown tokens
 * are ignored. */
typedef struct {
    float v[6]; /* yaw, pitch, roll, x, y, z */
    int apply;
    float fov_deg, aspect;
    int level;
    int lod;    /* 0: the LOD fix hook leaves this camera alone */
} VrFakeFile;
static inline void vr_fake_file_parse(const char *text, VrFakeFile *out) {
    static const char *const keys[6] = {"yaw=", "pitch=", "roll=", "x=", "y=", "z="};
    memset(out, 0, sizeof(*out));
    out->apply = 3;
    out->level = -1;
    out->lod = 1;
    int next = 0;
    while (*text) {
        while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' || *text == ',') ++text;
        if (!*text) break;
        const char *end = text;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r' && *end != ',') ++end;
        char token[64];
        const size_t n = (size_t)(end - text) < sizeof(token) - 1 ? (size_t)(end - text) : sizeof(token) - 1;
        memcpy(token, text, n);
        token[n] = 0;
        text = end;
        char *stop;
        const float bare = strtof(token, &stop);
        if (stop != token && !*stop && isfinite(bare)) {
            if (next < 6) out->v[next++] = bare;
            continue;
        }
        const char *eq = strchr(token, '=');
        if (!eq) continue;
        const float value = strtof(eq + 1, &stop);
        if (stop == eq + 1 || !isfinite(value)) continue;
        for (int k = 0; k < 6; ++k)
            if (!strncmp(token, keys[k], strlen(keys[k]))) out->v[k] = value;
        if (!strncmp(token, "apply=", 6)) out->apply = (int)value & 3;
        if (!strncmp(token, "fov=", 4)) out->fov_deg = value;
        if (!strncmp(token, "aspect=", 7)) out->aspect = value;
        if (!strncmp(token, "level=", 6)) out->level = value < 0.0f ? -1 : value > 0.0f ? 1 : 0;
        if (!strncmp(token, "lod=", 4)) out->lod = value != 0.0f;
    }
}

/* The fake head's OpenXR orientation (and position) at time t (seconds). */
static inline VrQuat vr_fake_pose(const VrFakeHead *f, double t, VrVec3 *position) {
    if (position) *position = vr_v3(f->x, f->y, f->z);
    if (f->kind == VR_FAKE_SWEEP) {
        const double w = 2.0 * 3.14159265358979 * t / f->period;
        return vr_xr_ypr(f->amp_yaw * VR_DEG * (float)sin(w),
                         f->amp_pitch * VR_DEG * (float)sin(w / 1.7), 0.0f);
    }
    return vr_xr_ypr(f->yaw * VR_DEG, f->pitch * VR_DEG, f->roll * VR_DEG);
}

#endif
