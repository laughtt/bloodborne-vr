/* VR alternate-eye stereo, PLAN.md Phase 3 (BB_XR_MODE=alt; docs/vr/findings/22): each game
 * frame renders one eye, L, R, L, R. The H1 hook (src/runtime_vr_head.c) composes the camera as
 * for mono (head rotation, first person, head position, the symmetric frustum that contains both
 * eyes); then vr_stereo_h1 picks the eye (vrs_schedule_next), moves the camera to that eye along
 * its own axes (the eye's offset from the head as the runtime reports it, xrLocateViews),
 * tags the right eye in the projection (VRS_TAG) and publishes the eye's view for the presenter:
 * seq, eye, the camera (the finished frame is recognized by it), and the pose and frustum it is
 * rendered with (bbgpu_vr_set_eye_view). H2 (the main context, whose camera position the LOD
 * selection uses: findings/22 section 5) stays at the center unless BB_VR_STEREO_H2=eye. The math is
 * src/vr_stereo_math.h (tests/test_vr_stereo.c); the presenter side is
 * gpu/shadps4/video_core/renderer_vulkan/vk_xr_stereo.cpp.
 *
 * Optional off-axis frusta (BB_VR_STEREO_OFFAXIS=1 with the two "VR Stereo Off-Axis" hooks): each
 * eye's camera gets the width of its own frustum and the projection builders (0x25aa6c0's epilogue
 * for SetCamera, 0x25aa290's for the passes) shift every projection built from it, so the image
 * covers exactly the eye's view instead of the symmetric frustum around both eyes.
 *
 * Environment:
 *   BB_XR_MODE=alt            on (with BB_VR=1 the frames go to the headset's eyes; without it the
 *                             pairing still runs and logs: flat tests, tools/vr_stereo_test.py)
 *   BB_VR_STEREO_IPD=<m>      the IPD without a headset, or when the runtime's eye offsets are not
 *                             plausible (default 0.063)
 *   BB_VR_STEREO_SCALE=<s>    world scale of the camera offset (1 = true size, the default)
 *   BB_VR_STEREO_H2=eye       the main context's camera at the eye too (default: the center)
 *   BB_VR_STEREO_OFFAXIS=1    off-axis per-eye frusta (needs the projection hooks)
 *   BB_VR_STEREO_LOG=1        1 line per second (also with BB_VR_HEAD_LOG=1)
 *   BB_VR_STEREO_CONTROL=<f>  live control file, re-read when it changes (vr_stereo_math.h:
 *                             alt= ipd= scale= invert= tag= offaxis= eyel= eyer= h2=)
 */
#define _GNU_SOURCE
#include "runtime.h"
#include "hooks.h"
#include "vr_stereo.h"
#include "vr_stereo_math.h"
#include "gpu/bbgpu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

enum { VS_OFF_DEFAULT = 0, VS_OFF_RUNTIME = 1, VS_OFF_CONTROL = 2 };

static struct {
    int ready, enabled, log, h2_eye_env, offaxis_env;
    float ipd_default, scale_env;
    VrsSchedule sched;
    VrsControl control;
    const char *control_path;
    int control_loaded;
    uint64_t control_checked, control_mtime, control_size;
    uint64_t seq;
    /* H1 -> H2 of the same frame */
    int h2_pending;
    uint64_t h2_frame;
    VrVec3 h2_delta;
    float h2_aspect;
    /* logs */
    uint64_t next_log;
    uint64_t applied[2];
    int warned_offset, warned_cover, logged_first, last_source, last_offaxis;
} vs;

/* The off-axis projection table the projection hooks read (any thread): per eye the 1/half-width
 * the eye's projection has (its x scale) and its shift P20, as float bits (0: no off-axis for that
 * eye), and the eye camera's FOV and aspect bits (the passes' builder recognizes the camera itself);
 * the shared 1/ty (y scale). Written by H1 every frame it renders that eye. */
static _Atomic uint64_t vs_proj_eye[2], vs_proj_cam[2];
static _Atomic uint32_t vs_proj_inv_ty;
static _Atomic uint32_t vs_proj_calls, vs_proj_hits, vs_proj_tiled;
static _Atomic int vs_proj_logged[2];

static uint64_t vs_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static int vs_env_on(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '1';
}

static float vs_env_float(const char *name, float fallback, float lo, float hi) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    const float f = strtof(v, NULL);
    return isfinite(f) && f >= lo && f <= hi ? f : fallback;
}

static void vs_init(void) {
    if (vs.ready) return;
    vs.ready = 1;
    const char *mode = getenv("BB_XR_MODE");
    vs.enabled = mode && !strcmp(mode, "alt");
    vs.log = vs_env_on("BB_VR_STEREO_LOG") || vs_env_on("BB_VR_HEAD_LOG");
    vs.ipd_default = vs_env_float("BB_VR_STEREO_IPD", VRS_DEFAULT_IPD, 0.0f, 1.0f);
    vs.scale_env = vs_env_float("BB_VR_STEREO_SCALE", 1.0f, 0.0f, 8.0f);
    const char *h2 = getenv("BB_VR_STEREO_H2");
    vs.h2_eye_env = h2 && !strcmp(h2, "eye");
    vs.offaxis_env = vs_env_on("BB_VR_STEREO_OFFAXIS");
    vrs_schedule_reset(&vs.sched);
    vrs_control_default(&vs.control);
    const char *control = getenv("BB_VR_STEREO_CONTROL");
    vs.control_path = control && *control ? control : NULL;
    vs.last_source = vs.last_offaxis = -1;
    if (vs.enabled)
        fprintf(stderr, "VR stereo: alternate eyes on (BB_XR_MODE=alt): one eye per game frame, L R L R; "
                        "default IPD %.1f mm, world scale %.2f, main context (H2) at the %s, off-axis %s%s%s\n",
                vs.ipd_default * 1000.0f, vs.scale_env, vs.h2_eye_env ? "eye" : "center",
                vs.offaxis_env ? "on (needs the VR Stereo Off-Axis hooks)" : "off",
                vs.control_path ? ", control file " : "", vs.control_path ? vs.control_path : "");
}

int vr_stereo_enabled(void) {
    vs_init();
    return vs.enabled;
}

/* The control file: re-read (at most every 100 ms) when its time stamp or size changes. */
static void vs_poll_control(uint64_t now) {
    if (!vs.control_path || (vs.control_loaded && now - vs.control_checked < 100000)) return;
    vs.control_checked = now;
    struct stat st;
    if (stat(vs.control_path, &st) != 0) return;
    if (vs.control_loaded && (uint64_t)st.st_mtime == vs.control_mtime && (uint64_t)st.st_size == vs.control_size)
        return;
    FILE *f = fopen(vs.control_path, "rb");
    if (!f) return;
    char text[512];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    vs.control_mtime = (uint64_t)st.st_mtime;
    vs.control_size = (uint64_t)st.st_size;
    vrs_control_parse(text, &vs.control);
    vs.control_loaded = 1;
    fprintf(stderr, "VR stereo: control file: alt %d ipd %.4f scale %.3f invert %d tag %d offaxis %d eye edges %.1f/%.1f h2 %d\n",
            vs.control.alt, vs.control.ipd, vs.control.scale, vs.control.invert, vs.control.tag, vs.control.offaxis,
            vs.control.eye_l_deg, vs.control.eye_r_deg, vs.control.h2);
}

static int vs_h2_eye(void) { return vs.control.h2 >= 0 ? vs.control.h2 : vs.h2_eye_env; }
static int vs_offaxis_wanted(void) { return vs.control.offaxis >= 0 ? vs.control.offaxis : vs.offaxis_env; }

static VrQuat vs_q(const BbVrPose *p) {
    return vr_q(p->orientation[0], p->orientation[1], p->orientation[2], p->orientation[3]);
}
static VrVec3 vs_p(const BbVrPose *p) { return vr_v3(p->position[0], p->position[1], p->position[2]); }

/* The eyes' offsets from the head (head frame, OpenXR axes): the control file's IPD, else the
 * runtime's from one GetViews snapshot, else the default IPD. From the same snapshot, each eye's
 * frustum bounds around the head's axis (*have_bounds; the control file's eyel/eyer without a
 * headset). Warns once about black edges when the camera's frustum (tangents tx, ty) does not
 * cover an eye. Returns VS_OFF_*. */
static int vs_offsets(const VrStereoHead *head, float tx, float ty, VrVec3 off[2], VrsBounds bounds[2],
                      int *have_bounds) {
    *have_bounds = 0;
    BbVrViews v;
    const int views = head->from_headset && bbgpu_vr_views(&v);
    int source = VS_OFF_DEFAULT;
    if (views) {
        const VrQuat e0 = vs_q(&v.eye[0].pose), e1 = vs_q(&v.eye[1].pose);
        VrQuat hq = vs_q(&v.head);
        VrVec3 hp = vs_p(&v.head);
        if (!vr_qvalid(hq) && vr_qvalid(e0) && vr_qvalid(e1)) {
            hq = vr_qnlerp(e0, e1, 0.5f); /* the middle of the eyes (the VIEW space) */
            hp = vr_scale(vr_add(vs_p(&v.eye[0].pose), vs_p(&v.eye[1].pose)), 0.5f);
        }
        if (vr_qvalid(hq) && vr_qvalid(e0) && vr_qvalid(e1)) {
            hq = vr_qnormalize(hq);
            const VrVec3 o0 = vrs_eye_in_head(hq, hp, vs_p(&v.eye[0].pose));
            const VrVec3 o1 = vrs_eye_in_head(hq, hp, vs_p(&v.eye[1].pose));
            const VrFov f0 = {v.eye[0].fov_left, v.eye[0].fov_right, v.eye[0].fov_up, v.eye[0].fov_down};
            const VrFov f1 = {v.eye[1].fov_left, v.eye[1].fov_right, v.eye[1].fov_up, v.eye[1].fov_down};
            const VrQuat in0 = vr_qmul(vr_qconj(hq), e0), in1 = vr_qmul(vr_qconj(hq), e1);
            *have_bounds = vrs_eye_bounds(in0, f0, &bounds[0]) && vrs_eye_bounds(in1, f1, &bounds[1]);
            if (!vs.warned_cover && tx > 0.0f && ty > 0.0f) {
                const float c0 = vrs_coverage(in0, f0, tx, ty), c1 = vrs_coverage(in1, f1, tx, ty);
                if (c0 < 0.99f || c1 < 0.99f) {
                    vs.warned_cover = 1;
                    fprintf(stderr, "VR stereo: warning: the camera's frustum (%.1f x %.1f deg) covers only %.0f%%/%.0f%% of "
                                    "the eyes' views: black edges (BB_VR_HEAD_FOV=hmd covers both)\n",
                            2.0f * atanf(tx) / VR_DEG, 2.0f * atanf(ty) / VR_DEG, c0 * 100.0f, c1 * 100.0f);
                }
            }
            if (vrs_offset_ok(o0, VRS_LEFT) && vrs_offset_ok(o1, VRS_RIGHT)) {
                off[0] = o0;
                off[1] = o1;
                source = VS_OFF_RUNTIME;
            } else if (!vs.warned_offset) {
                vs.warned_offset = 1;
                fprintf(stderr, "VR stereo: warning: the runtime's eye offsets L (%.4f %.4f %.4f) R (%.4f %.4f %.4f) m "
                                "are not plausible: IPD %.1f mm (BB_VR_STEREO_IPD) instead\n",
                        o0.x, o0.y, o0.z, o1.x, o1.y, o1.z, vs.ipd_default * 1000.0f);
            }
        }
    }
    if (!*have_bounds && vs.control.eye_l_deg < 0.0f && vs.control.eye_r_deg > 0.0f && ty > 0.0f) {
        /* No headset: the control file's left eye, the right eye mirrored, the camera's vertical. */
        bounds[0].l = tanf(vs.control.eye_l_deg * VR_DEG);
        bounds[0].r = tanf(vs.control.eye_r_deg * VR_DEG);
        bounds[1].l = -bounds[0].r;
        bounds[1].r = -bounds[0].l;
        bounds[0].u = bounds[1].u = ty;
        bounds[0].d = bounds[1].d = -ty;
        *have_bounds = 1;
    }
    if (vs.control.ipd > 0.0f) {
        off[0] = vrs_default_offset(VRS_LEFT, vs.control.ipd);
        off[1] = vrs_default_offset(VRS_RIGHT, vs.control.ipd);
        return VS_OFF_CONTROL;
    }
    if (source == VS_OFF_RUNTIME) return source;
    off[0] = vrs_default_offset(VRS_LEFT, vs.ipd_default);
    off[1] = vrs_default_offset(VRS_RIGHT, vs.ipd_default);
    return VS_OFF_DEFAULT;
}

static uint32_t vs_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof(b));
    return b;
}
static float vs_float(uint32_t b) {
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}

static void vs_proj_log(int e, const float *p, float shift, const char *where) {
    if (!atomic_exchange_explicit(&vs_proj_logged[e], 1, memory_order_relaxed))
        fprintf(stderr, "VR stereo: off-axis: first %s eye projection (%s): r0 (%.5f %g %g %g) r1 (%g %.5f %g %g) "
                        "r2 (%.5f %.5f %.6f %g) r3 (%g %g %.6f %g), P20 += %.5f\n",
                e ? "right" : "left", where, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
                p[10], p[11], p[12], p[13], p[14], p[15], shift);
}

/* 0x25aa933 (0x25aa6c0's epilogue, SetCamera's builder; replay-after): out = r15 (rows r0..r3,
 * row-vector). The camera pointer is gone here: a projection whose x and y scales are an eye
 * camera's gets that eye's shift. */
static ABI void vs_hook_proj_setcamera(BbHookContext *ctx) {
    float *p = (float *)(uintptr_t)ctx->r15;
    atomic_fetch_add_explicit(&vs_proj_calls, 1, memory_order_relaxed);
    const uint32_t ty_bits = atomic_load_explicit(&vs_proj_inv_ty, memory_order_acquire);
    if (!ty_bits) return;
    const float inv_ty = vs_float(ty_bits);
    for (int e = 0; e < 2; ++e) {
        const uint64_t w = atomic_load_explicit(&vs_proj_eye[e], memory_order_acquire);
        if (!w) continue;
        const float inv_hw = vs_float((uint32_t)w), shift = vs_float((uint32_t)(w >> 32));
        if (!vrs_projection_is(p, inv_hw, inv_ty)) continue;
        vs_proj_log(e, p, shift, "SetCamera");
        p[8] += shift;
        atomic_fetch_add_explicit(&vs_proj_hits, 1, memory_order_relaxed);
        return;
    }
}

/* 0x25aa69c (0x25aa290's epilogue, the passes' builder; replay-after): out = r14, camera = rbx
 * (format B: FOV +0x50, aspect +0x64). The main view goes through its tile path with one tile (+0x70
 * = 1, the whole frame; the frame's depth slices too), so the camera itself is matched: its FOV and
 * aspect bits are an eye camera's. A tile's projection maps its part of the frame, so the shift
 * scales with its x scale against the whole frame's (1 for one tile). */
static ABI void vs_hook_proj_pass(BbHookContext *ctx) {
    float *p = (float *)(uintptr_t)ctx->r14;
    const unsigned char *cam = (const unsigned char *)(uintptr_t)ctx->rbx;
    atomic_fetch_add_explicit(&vs_proj_calls, 1, memory_order_relaxed);
    uint32_t fov_bits, aspect_bits, tiles;
    memcpy(&fov_bits, cam + 0x50, sizeof(fov_bits));
    memcpy(&aspect_bits, cam + 0x64, sizeof(aspect_bits));
    memcpy(&tiles, cam + 0x70, sizeof(tiles));
    const uint64_t key = (uint64_t)fov_bits | (uint64_t)aspect_bits << 32;
    for (int e = 0; e < 2; ++e) {
        if (atomic_load_explicit(&vs_proj_cam[e], memory_order_acquire) != key) continue;
        const uint64_t w = atomic_load_explicit(&vs_proj_eye[e], memory_order_acquire);
        if (!w) return;
        const float inv_hw = vs_float((uint32_t)w), shift = vs_float((uint32_t)(w >> 32));
        const float scale = tiles > 1 ? p[0] / inv_hw : 1.0f;
        vs_proj_log(e, p, shift * scale, tiles ? "passes, tile path" : "passes");
        p[8] += shift * scale;
        atomic_fetch_add_explicit(&vs_proj_hits, 1, memory_order_relaxed);
        if (tiles) atomic_fetch_add_explicit(&vs_proj_tiled, 1, memory_order_relaxed);
        return;
    }
}

void vr_stereo_register(void) {
    bb_hook_set_stereo_impl(vs_hook_proj_setcamera, vs_hook_proj_pass);
}

/* Publishes (or clears) eye `eye`'s off-axis projection for the hooks: the camera's FOV and aspect
 * (as H1 leaves them), its projection's x and y scales, and the shift. */
static void vs_proj_publish(int eye, float fov, float aspect, float inv_hw, float inv_ty, float shift, int on) {
    const uint64_t w = on ? (uint64_t)vs_bits(inv_hw) | (uint64_t)vs_bits(shift) << 32 : 0;
    const uint64_t key = on ? (uint64_t)vs_bits(fov) | (uint64_t)vs_bits(aspect) << 32 : 0;
    if (on) atomic_store_explicit(&vs_proj_inv_ty, vs_bits(inv_ty), memory_order_release);
    atomic_store_explicit(&vs_proj_eye[eye], w, memory_order_release);
    atomic_store_explicit(&vs_proj_cam[eye], key, memory_order_release);
}

int vr_stereo_h1(float *cam, const VrStereoHead *head) {
    vs_init();
    vs.h2_pending = 0;
    if (!vs.enabled) return -1;
    const uint64_t now = vs_now_us();
    vs_poll_control(now);
    if (!vs.control.alt || !head || !head->valid) {
        vs_proj_publish(0, 0, 0, 0, 0, 0, 0);
        vs_proj_publish(1, 0, 0, 0, 0, 0, 0);
        return -1;
    }
    float (*m)[4] = (float (*)[4])cam;
    const float ty = tanf(0.5f * cam[16]), tx = ty * cam[17];
    VrVec3 off[2];
    VrsBounds bounds[2];
    int have_bounds;
    const int source = vs_offsets(head, tx, ty, off, bounds, &have_bounds);
    if (source != vs.last_source) {
        vs.last_source = source;
        const VrVec3 apart = vr_sub(off[1], off[0]);
        fprintf(stderr, "VR stereo: eye offsets from the %s: L (%.4f %.4f %.4f) R (%.4f %.4f %.4f) m, IPD %.1f mm\n",
                source == VS_OFF_RUNTIME ? "runtime (xrLocateViews)" : source == VS_OFF_CONTROL ? "control file" : "default IPD",
                off[0].x, off[0].y, off[0].z, off[1].x, off[1].y, off[1].z, sqrtf(vr_dot(apart, apart)) * 1000.0f);
    }
    /* The eye: strict alternation, with the presenter's view for the starvation guard. */
    VrsFeedback fb;
    memset(&fb, 0, sizeof(fb));
    fb.last_eye = -1;
    BbVrStereoStatus status;
    if (bbgpu_vr_stereo_status(&status)) {
        fb.assigned[0] = status.assigned[0];
        fb.assigned[1] = status.assigned[1];
        fb.last_eye = status.last_eye;
        fb.run = status.run;
    }
    const int eye = vrs_schedule_next(&vs.sched, &fb);
    const float center[3] = {m[3][0], m[3][1], m[3][2]};
    const VrVec3 d = vrs_apply_offset(m, vs.control.invert ? off[1 - eye] : off[eye], vs.scale_env * vs.control.scale);
    if (!isfinite(m[3][0]) || !isfinite(m[3][1]) || !isfinite(m[3][2])) {
        m[3][0] = center[0];
        m[3][1] = center[1];
        m[3][2] = center[2];
        return -1;
    }
    /* The frustum: the symmetric one around both eyes, or (off-axis, once the projection hooks are
     * seen to run) the eye's own horizontal extent with the shift the hooks add. */
    const int offaxis = vs_offaxis_wanted() && have_bounds && ty > 0.0f &&
                        atomic_load_explicit(&vs_proj_calls, memory_order_relaxed) > 0;
    if (offaxis != vs.last_offaxis) {
        vs.last_offaxis = offaxis;
        if (offaxis)
            fprintf(stderr, "VR stereo: off-axis frusta: left eye %.1f..%.1f deg, right eye %.1f..%.1f deg (symmetric "
                            "%.1f..%.1f)\n",
                    atanf(bounds[0].l) / VR_DEG, atanf(bounds[0].r) / VR_DEG, atanf(bounds[1].l) / VR_DEG,
                    atanf(bounds[1].r) / VR_DEG, -atanf(tx) / VR_DEG, atanf(tx) / VR_DEG);
        else if (vs_offaxis_wanted())
            fprintf(stderr, "VR stereo: off-axis frusta wanted but %s: symmetric frustum\n",
                    !have_bounds ? "no eye frustum (headset views or eyel/eyer)"
                                 : "the VR Stereo Off-Axis hooks have not run (BB_HOOKS)");
    }
    float shift = 0.0f;
    if (offaxis) {
        float aspect;
        vrs_offaxis(bounds[eye].l, bounds[eye].r, ty, &aspect, &shift);
        cam[17] = vrs_tag_aspect(aspect, eye, vs.control.tag);
        vs_proj_publish(eye, cam[16], cam[17], 1.0f / (ty * cam[17]), 1.0f / ty, shift, 1);
    } else {
        vs_proj_publish(eye, 0, 0, 0, 0, 0, 0);
        if (cam[17] > 0.0f && isfinite(cam[17])) cam[17] = vrs_tag_aspect(cam[17], eye, vs.control.tag);
    }
    /* What the presenter pairs the finished frame with, and the layer's pose and frustum. */
    BbVrEyeView view;
    memset(&view, 0, sizeof(view));
    view.seq = ++vs.seq;
    view.eye = (uint32_t)eye;
    view.has_pose = head->from_headset ? 1u : 0u;
    view.display_time = head->display_time;
    VrQuat lq;
    VrVec3 lp;
    vrs_layer_pose(head->head, head->head_pos, off[eye], &lq, &lp);
    view.pose.position[0] = lp.x;
    view.pose.position[1] = lp.y;
    view.pose.position[2] = lp.z;
    view.pose.orientation[0] = lq.x;
    view.pose.orientation[1] = lq.y;
    view.pose.orientation[2] = lq.z;
    view.pose.orientation[3] = lq.w;
    const VrFov f = offaxis ? vrs_fov_shifted(cam[16], cam[17], shift) : vrs_fov_from_camera(cam[16], cam[17]);
    view.fov_left = f.left;
    view.fov_right = f.right;
    view.fov_up = f.up;
    view.fov_down = f.down;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) view.rows[i][j] = m[i][j];
        view.position[i] = m[3][i];
        view.center[i] = center[i];
    }
    bbgpu_vr_set_eye_view(&view);
    vs.h2_pending = 1;
    vs.h2_frame = head->frame;
    vs.h2_delta = d;
    vs.h2_aspect = cam[17];
    ++vs.applied[eye];
    if (!vs.logged_first) {
        vs.logged_first = 1;
        fprintf(stderr, "VR stereo: first eye camera: %s, moved (%.4f %.4f %.4f), fov %.4f rad, aspect %.6f\n",
                eye ? "right" : "left", d.x, d.y, d.z, cam[16], cam[17]);
    }
    if (vs.log && now >= vs.next_log) {
        vs.next_log = now + 1000000;
        fprintf(stderr, "VR stereo: eye %c seq %llu moved (%.4f %.4f %.4f) aspect %.6f fov %.4f | set up L %llu R %llu, "
                        "guard shifts %u | presenter: L %llu R %llu, last %d run %u | off-axis %d: builder calls %u, "
                        "shifted %u, tiled %u\n",
                eye ? 'R' : 'L', (unsigned long long)view.seq, d.x, d.y, d.z, cam[17], cam[16],
                (unsigned long long)vs.applied[0], (unsigned long long)vs.applied[1], vs.sched.guard_flips,
                (unsigned long long)fb.assigned[0], (unsigned long long)fb.assigned[1], fb.last_eye, fb.run, offaxis,
                atomic_load_explicit(&vs_proj_calls, memory_order_relaxed),
                atomic_load_explicit(&vs_proj_hits, memory_order_relaxed),
                atomic_load_explicit(&vs_proj_tiled, memory_order_relaxed));
    }
    return eye;
}

void vr_stereo_h2(float *cam, uint64_t frame) {
    if (!vs.h2_pending || vs.h2_frame != frame) return;
    vs.h2_pending = 0;
    if (!vs_h2_eye()) return; /* the main context stays at the center: LOD from the center */
    cam[12] += vs.h2_delta.x;
    cam[13] += vs.h2_delta.y;
    cam[14] += vs.h2_delta.z;
    if (vs.h2_aspect > 0.0f) cam[25] = vs.h2_aspect;
}
