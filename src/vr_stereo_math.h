/* Pure logic for alternate-eye stereo (BB_XR_MODE=alt, docs/vr/PLAN.md Phase 3,
 * docs/vr/findings/22-phase3-alt-eye-stereo.md): the eye schedule, the per-eye camera offset, the
 * eye's layer pose and frustum, the eye tag carried by the projection, and the stereo control
 * file. No I/O, no globals: src/runtime_vr_stereo.c uses it from the H1/H2 camera hooks and
 * tests/test_vr_stereo.c checks it. Compiles as C11 and as C++.
 *
 * Conventions are those of vr_head_math.h: OpenXR right-handed (+X right, +Y up, -Z forward),
 * the game camera four float4 rows right, up, forward, position (left-handed, +Z forward).
 *
 * One game frame renders one eye, alternating L, R, L, R:
 *   - H1 composes the camera as for mono (head rotation, first person, the symmetric frustum that
 *     contains both eyes), then vrs_schedule_next picks the eye and vrs_apply_offset moves the
 *     camera to that eye along its own axes (the offset of the eye from the head that the runtime
 *     reports, xrLocateViews; never a hard-coded IPD unless there is no headset).
 *   - The right eye renders with its aspect raised by VRS_TAG (relative 4e-5: 0.04 px at the edge
 *     of a 2000 px image): the projection's x scale then tells the eyes apart in the finished
 *     frame even when the two cameras coincide (strafing at IPD per frame).
 *   - The image goes to that eye's swapchain with the pose and frustum it was rendered with
 *     (vrs_layer_pose, vrs_fov_from_camera): the runtime reprojects each eye from its own pose.
 */
#ifndef VR_STEREO_MATH_H
#define VR_STEREO_MATH_H
#include "vr_head_math.h"
#include <stdint.h>
#include <stdio.h>

enum { VRS_LEFT = 0, VRS_RIGHT = 1 };

/* Relative aspect raise of the right eye (the eye tag), see above. */
#define VRS_TAG 4e-5f
/* The presenter's starvation guard: this many frames in a row for one eye means the other eye's
 * frames are being lost; the next frame goes to the starved eye, twice (a one-frame phase shift),
 * and the guard then waits VRS_GUARD_HOLD frames for the change to reach the presenter. Two
 * shifts within VRS_PAIR_WINDOW frames mean the presenter only sees every other frame (a 2:1
 * decimation starves one eye of any strict alternation): the schedule then renders the eyes in
 * pairs, L L R R, which feeds both eyes whichever half is seen; it goes back to L R L R once the
 * presenter shows it sees whole pairs (runs of 2, VRS_PAIR_EXIT samples). */
#define VRS_STARVE_RUN 4u
#define VRS_GUARD_HOLD 8u
#define VRS_PAIR_WINDOW 64u
#define VRS_PAIR_EXIT 32u
/* An eye offset from the runtime is used when it is 15..75 mm to the side (IPD 30..150 mm) and
 * within 40 mm up/down and fore/aft; otherwise the default IPD is. */
#define VRS_MIN_HALF_IPD 0.015f
#define VRS_MAX_HALF_IPD 0.075f
#define VRS_MAX_OFF_AXIS 0.040f
#define VRS_DEFAULT_IPD 0.063f

/* ---- the eye schedule (H1) ---- */

typedef struct {
    uint64_t frames;      /* frames scheduled */
    int next;             /* the eye of the next frame */
    uint32_t hold;        /* frames the guard still waits */
    uint32_t guard_flips; /* phase shifts made by the guard */
    uint64_t last_flip;   /* frames at the latest shift */
    int pairs;            /* 1: L L R R (see VRS_PAIR_WINDOW) */
    uint32_t pair_count;  /* frames of the current eye in pair mode */
    uint32_t whole_pairs; /* pair mode: samples showing the presenter sees whole pairs */
    uint32_t pair_entries; /* times pair mode was entered */
} VrsSchedule;

/* What the presenter assigned so far (bbgpu_vr_stereo_status). */
typedef struct {
    uint64_t assigned[2]; /* finished frames given to each eye */
    int last_eye;         /* the eye of the latest one, -1 none yet */
    uint32_t run;         /* how many in a row went to last_eye */
} VrsFeedback;

static inline void vrs_schedule_reset(VrsSchedule *s) {
    memset(s, 0, sizeof(*s));
    s->next = VRS_LEFT;
}

/* The eye of the frame H1 is setting up now. Strict alternation counted per rendered frame (never
 * per display frame): a late or lost frame delays its eye, it never swaps the order. A frame's eye
 * is fixed here and travels with its camera; the guard (fb: the presenter's view, may be NULL)
 * only changes which eye later frames get. */
static inline int vrs_schedule_next(VrsSchedule *s, const VrsFeedback *fb) {
    int eye = s->next;
    if (s->pairs && fb && fb->run >= 2 && ++s->whole_pairs >= VRS_PAIR_EXIT) {
        s->pairs = 0; /* the presenter sees every frame: back to L R L R */
        s->whole_pairs = 0;
    }
    if (s->hold) {
        --s->hold;
    } else if (fb && fb->last_eye >= 0 && fb->run >= VRS_STARVE_RUN) {
        eye = 1 - fb->last_eye; /* the starved eye, this frame and the next */
        if (s->guard_flips && s->frames - s->last_flip <= VRS_PAIR_WINDOW && !s->pairs) {
            s->pairs = 1;
            s->whole_pairs = 0;
            ++s->pair_entries;
        }
        s->last_flip = s->frames;
        s->next = eye;
        s->pair_count = 1;
        s->hold = VRS_GUARD_HOLD;
        ++s->guard_flips;
        ++s->frames;
        return eye;
    }
    if (s->pairs) {
        if (++s->pair_count >= 2) {
            s->pair_count = 0;
            s->next = 1 - eye;
        }
    } else {
        s->next = 1 - eye;
    }
    ++s->frames;
    return eye;
}

/* ---- the eye offset ---- */

/* The eye's offset from the head in the head's own frame (OpenXR axes, meters):
 * conj(head) . (eye - head). Canted displays (Pimax) do not matter: only positions are used. */
static inline VrVec3 vrs_eye_in_head(VrQuat head, VrVec3 head_pos, VrVec3 eye_pos) {
    return vr_qrotate(vr_qconj(head), vr_sub(eye_pos, head_pos));
}

/* 1 when `off` (vrs_eye_in_head) is a plausible offset for `eye` (left: -X). */
static inline int vrs_offset_ok(VrVec3 off, int eye) {
    const float side = eye == VRS_LEFT ? -off.x : off.x;
    return isfinite(off.x) && isfinite(off.y) && isfinite(off.z) && side >= VRS_MIN_HALF_IPD &&
           side <= VRS_MAX_HALF_IPD && fabsf(off.y) <= VRS_MAX_OFF_AXIS &&
           fabsf(off.z) <= VRS_MAX_OFF_AXIS;
}

/* Half an IPD to the eye's side (no headset, or a runtime offset that is not plausible). */
static inline VrVec3 vrs_default_offset(int eye, float ipd) {
    return vr_v3(eye == VRS_LEFT ? -0.5f * ipd : 0.5f * ipd, 0.0f, 0.0f);
}

/* BB_XR_MODE=stereo: both eyes' offsets from the head, from ONE views snapshot (the head and the
 * two eyes the runtime located for the same display time), so the pair's layer poses are
 * head . offset with the runtime's own baseline. Returns 1 when both are plausible; otherwise
 * `off` is the default IPD split (the caller then publishes no eye offsets). */
static inline int vrs_pair_offsets(VrQuat head, VrVec3 head_pos, VrVec3 eye0, VrVec3 eye1,
                                   float default_ipd, VrVec3 off[2]) {
    const VrVec3 o0 = vrs_eye_in_head(head, head_pos, eye0);
    const VrVec3 o1 = vrs_eye_in_head(head, head_pos, eye1);
    if (vrs_offset_ok(o0, VRS_LEFT) && vrs_offset_ok(o1, VRS_RIGHT)) {
        off[0] = o0;
        off[1] = o1;
        return 1;
    }
    off[0] = vrs_default_offset(VRS_LEFT, default_ipd);
    off[1] = vrs_default_offset(VRS_RIGHT, default_ipd);
    return 0;
}

/* Moves the composed camera (rows right, up, forward, position) to the eye: the head-frame offset
 * `off` (OpenXR axes, meters) times `scale` (world scale: 1 = true size) along the camera's own
 * axes. Call after the head rotation, the first-person eye position and the head position were
 * composed. Returns the world displacement (the main context's camera gets the same). */
static inline VrVec3 vrs_apply_offset(float m[4][4], VrVec3 off, float scale) {
    const VrVec3 g = vr_scale(vr_xr_to_game_vec(off), scale);
    const VrVec3 d = vr_add(vr_add(vr_scale(vr_row(m, 0), g.x), vr_scale(vr_row(m, 1), g.y)),
                            vr_scale(vr_row(m, 2), g.z));
    vr_set_row(m, 3, vr_add(vr_row(m, 3), d));
    return d;
}

/* Which side of `center` the camera at `position` is, along the camera's right row: -1 left, +1
 * right, 0 when they are closer than 1 mm (no offset). */
static inline int vrs_side(const float rows[3][3], const float center[3], const float position[3]) {
    const float s = (position[0] - center[0]) * rows[0][0] + (position[1] - center[1]) * rows[0][1] +
                    (position[2] - center[2]) * rows[0][2];
    return s < -1e-3f ? -1 : s > 1e-3f ? 1 : 0;
}

/* ---- the eye's layer: pose and frustum ---- */

/* The pose the eye's image is rendered from, in the views' world space: the head orientation (the
 * camera turns with the head; eyes are not rotated individually) at the head position plus the
 * eye's real offset (unscaled: the runtime compares it with where the eye really is). */
static inline void vrs_layer_pose(VrQuat head, VrVec3 head_pos, VrVec3 off, VrQuat *q, VrVec3 *p) {
    *q = head;
    *p = vr_add(head_pos, vr_qrotate(head, off));
}

/* The camera's symmetric frustum (vertical FOV radians, aspect) as OpenXR angles. */
static inline VrFov vrs_fov_from_camera(float fov_y, float aspect) {
    const float ty = tanf(0.5f * fov_y), tx = ty * aspect;
    VrFov f;
    f.left = -atanf(tx);
    f.right = atanf(tx);
    f.up = atanf(ty);
    f.down = -atanf(ty);
    return f;
}

/* The aspect eye `eye` renders with: the right eye's is raised by VRS_TAG (when tag != 0). */
static inline float vrs_tag_aspect(float aspect, int eye, int tag) {
    return tag && eye == VRS_RIGHT ? aspect * (1.0f + VRS_TAG) : aspect;
}

/* ---- off-axis per-eye frustum (optional; findings/22 section 6) ----
 * The game builds symmetric projections (row-vector, left-handed: rows r0 = (sx,0,0,0), r1 =
 * (0,sy,0,0), r2 = (P20,P21,A,1), r3 = (0,0,B,0); x_ndc = x/z * sx + P20). An eye whose frustum is
 * [l, r] horizontally (tangents) gets a camera of half-width hw = (r - l) / 2 (its aspect) and the
 * shift P20 = -(r + l) / (r - l), added by the projection hooks to every projection built from that
 * camera. The vertical stays symmetric (the both-eyes ty), so the vertical FOV, the LOD scale and
 * the LOD fix do not change. */

/* Tangent bounds (left < 0 < right, down < 0 < up), around the head's axis, of an eye frustum
 * rotated by eye_in_head (conj(head) . eye; identity for parallel displays). 0 when unusable. */
typedef struct {
    float l, r, d, u;
} VrsBounds;
static inline int vrs_eye_bounds(VrQuat eye_in_head, VrFov fov, VrsBounds *out) {
    const float l = tanf(fov.left), r = tanf(fov.right), u = tanf(fov.up), d = tanf(fov.down);
    const float xs[3] = {l, 0.5f * (l + r), r}, ys[3] = {d, 0.5f * (d + u), u};
    VrsBounds b = {1e9f, -1e9f, 1e9f, -1e9f};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            if (i == 1 && j == 1) continue;
            const VrVec3 dir = vr_qrotate(eye_in_head, vr_v3(xs[i], ys[j], -1.0f));
            if (!(dir.z < -1e-3f)) return 0;
            const float x = dir.x / -dir.z, y = dir.y / -dir.z;
            b.l = x < b.l ? x : b.l;
            b.r = x > b.r ? x : b.r;
            b.d = y < b.d ? y : b.d;
            b.u = y > b.u ? y : b.u;
        }
    if (!(b.l < 0.0f && b.r > 0.0f && b.d < 0.0f && b.u > 0.0f) || !isfinite(b.l) || !isfinite(b.r)) return 0;
    *out = b;
    return 1;
}

/* The off-axis camera for horizontal tangents [l, r] at the vertical half-tangent ty: its aspect
 * and the projection shift P20. */
static inline void vrs_offaxis(float l, float r, float ty, float *aspect, float *shift) {
    *aspect = (r - l) / (2.0f * ty);
    *shift = -(r + l) / (r - l);
}

/* The frustum a camera (vertical FOV, aspect) renders with when its projection is shifted by
 * P20 = shift: half-width hw = ty * aspect around the centre -shift * hw. */
static inline VrFov vrs_fov_shifted(float fov_y, float aspect, float shift) {
    const float ty = tanf(0.5f * fov_y), hw = ty * aspect, c = -shift * hw;
    VrFov f;
    f.left = atanf(c - hw);
    f.right = atanf(c + hw);
    f.up = atanf(ty);
    f.down = -atanf(ty);
    return f;
}

/* 1 when a built projection (16 floats, rows as above) is the eye camera's: its x and y scales
 * equal 1 / hw and 1 / ty within a relative 1.5e-5 (the eye tag, 4e-5, keeps the eyes apart). */
static inline int vrs_projection_is(const float *p, float inv_hw, float inv_ty) {
    return fabsf(p[0] / inv_hw - 1.0f) < 1.5e-5f && fabsf(p[5] / inv_ty - 1.0f) < 1.5e-5f;
}

/* How much of the eye's own frustum the camera's symmetric frustum (tangents tx, ty around the
 * head's axis) covers: the smallest ratio camera/eye over the eye frustum's corners and edge
 * midpoints, rotated by the eye's orientation in the head (`eye_in_head`, conj(head) . eye).
 * >= 1: covered; < 1: the image ends inside the eye's view (black edges). */
static inline float vrs_coverage(VrQuat eye_in_head, VrFov eye, float tx, float ty) {
    const float l = tanf(eye.left), r = tanf(eye.right), u = tanf(eye.up), d = tanf(eye.down);
    const float xs[3] = {l, 0.5f * (l + r), r}, ys[3] = {d, 0.5f * (d + u), u};
    float worst = 1e9f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            if (i == 1 && j == 1) continue;
            const VrVec3 dir = vr_qrotate(eye_in_head, vr_v3(xs[i], ys[j], -1.0f));
            if (!(dir.z < -1e-3f)) return 0.0f;
            const float ax = fabsf(dir.x / -dir.z), ay = fabsf(dir.y / -dir.z);
            if (ax > 1e-6f && tx / ax < worst) worst = tx / ax;
            if (ay > 1e-6f && ty / ay < worst) worst = ty / ay;
        }
    return worst;
}

/* ---- the control file (BB_VR_STEREO_CONTROL, re-read when it changes; tools/vr_stereo_test.py)
 * Whitespace- or comma-separated key=value tokens, unknown ones ignored:
 *   alt=0|1      alternate the eyes (0: the mono camera, nothing published for the eyes)
 *   ipd=<m>      eye offset +-ipd/2 to the side instead of the runtime's (0 = the runtime's)
 *   scale=<s>    world scale of the camera offset (1 = true size; 0..8)
 *   invert=0|1   negative control: render each eye from the other eye's position, keep its label
 *   tag=0|1      the eye tag in the right eye's aspect (VRS_TAG)
 *   offaxis=0|1  each eye's own (off-axis) frustum through the projection hooks (BB_VR_STEREO_OFFAXIS)
 *   eyel=<deg> eyer=<deg>  without a headset: the left eye's left and right edge angles (left < 0;
 *                the right eye mirrored), for offaxis; 0 = none
 *   h2=0|1       the main context's camera (H2) at the eye (1) or at the center (0, BB_VR_STEREO_H2)
 * A missing key keeps its default (alt 1, ipd 0, scale 1, invert 0, tag 1, offaxis/h2: -1 = the
 * environment's, eyel/eyer 0). */
typedef struct {
    int alt;
    float ipd;
    float scale;
    int invert;
    int tag;
    int offaxis;
    float eye_l_deg, eye_r_deg;
    int h2;
} VrsControl;

static inline void vrs_control_default(VrsControl *c) {
    c->alt = 1;
    c->ipd = 0.0f;
    c->scale = 1.0f;
    c->invert = 0;
    c->tag = 1;
    c->offaxis = -1;
    c->eye_l_deg = c->eye_r_deg = 0.0f;
    c->h2 = -1;
}

static inline void vrs_control_parse(const char *text, VrsControl *c) {
    vrs_control_default(c);
    while (text && *text) {
        while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' || *text == ',') ++text;
        if (!*text) break;
        const char *end = text;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r' && *end != ',') ++end;
        char token[64];
        const size_t n = (size_t)(end - text) < sizeof(token) - 1 ? (size_t)(end - text) : sizeof(token) - 1;
        memcpy(token, text, n);
        token[n] = 0;
        text = end;
        const char *eq = strchr(token, '=');
        if (!eq) continue;
        char *stop;
        const float value = strtof(eq + 1, &stop);
        if (stop == eq + 1 || !isfinite(value)) continue;
        const size_t key = (size_t)(eq - token);
        if (key == 3 && !strncmp(token, "alt", 3)) c->alt = value != 0.0f;
        else if (key == 3 && !strncmp(token, "ipd", 3)) c->ipd = value >= 0.0f && value <= 1.0f ? value : 0.0f;
        else if (key == 5 && !strncmp(token, "scale", 5)) c->scale = value >= 0.0f && value <= 8.0f ? value : 1.0f;
        else if (key == 6 && !strncmp(token, "invert", 6)) c->invert = value != 0.0f;
        else if (key == 3 && !strncmp(token, "tag", 3)) c->tag = value != 0.0f;
        else if (key == 7 && !strncmp(token, "offaxis", 7)) c->offaxis = value != 0.0f;
        else if (key == 4 && !strncmp(token, "eyel", 4)) c->eye_l_deg = value < 0.0f && value > -89.0f ? value : 0.0f;
        else if (key == 4 && !strncmp(token, "eyer", 4)) c->eye_r_deg = value > 0.0f && value < 89.0f ? value : 0.0f;
        else if (key == 2 && !strncmp(token, "h2", 2)) c->h2 = value != 0.0f;
    }
}

#endif
