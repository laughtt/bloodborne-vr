/* VR head tracking, Phase 2 (docs/vr/PLAN.md): the headset pose composed into the game's
 * render camera through the host-call hooks H1/H2 (docs/vr/findings/08 sections 3 and 7).
 *
 * Every frame the draw step 0x22ddf30 copies the active game camera into render-side copies:
 *   H1 0x22de113 (replay-after): rbx = the step's private PersCam ([r12+0x28], copied from
 *      [SprjCamera+8] just before): matrix +0x10..+0x4f, FOV +0x50, aspect +0x54. It feeds the
 *      draw camera [r12+0x18] (format B, GetDrawCamera 0x2116690), the scene camera and its
 *      culling copy ([r12+0x10], 0x2694d40), the context handed to [r12+0x330], RendMan's
 *      projection (0x25aaa90), the SFX camera matrix and the audio listener (0x9489f0).
 *   H2 0x22de386 (replay-before): rbx = a format-B camera built from [SprjCamera+0x28] for the
 *      draw context at r12+0x50 (SetCamera 0x2691c30): matrix +0x00..+0x3f, FOV +0x50, aspect
 *      +0x64. Without H2 that context keeps the game's view.
 * Game logic (movement, lock-on, the camera controller) never reads these copies, so nothing is
 * restored. H1 takes one head sample per frame and H2 reuses it (same thread, same frame); H2
 * alone samples for itself. Measured (findings/15): H1 alone moves the picture. The math is
 * src/vr_head_math.h (tests/test_vr_head.c). H1 also publishes the view it rendered (head pose,
 * frustum, camera rows: bbgpu_vr_set_render_view) so BB_XR_MODE=mono shows each image with its
 * own view (bridge ABI 5), and an optional third hook keeps the LOD distances (vh_hook_lod).
 *
 * Environment (everything off by default; the hooks are selected with BB_HOOKS, run.py):
 *   BB_VR_HEAD=1         compose the head into the camera (otherwise the hooks only log once)
 *   BB_VR_HEAD_LOG=1     once per second: the game camera, the head and the result
 *   BB_VR_HEAD_LEVEL=1   horizon lock: the headset alone sets pitch and roll, the game camera
 *                        only its heading (findings/11 3.1); default: the head rotation is
 *                        applied in the game camera's own frame (the game's pitch stays)
 *   BB_VR_HEAD_POS=<s>   head position offset, scale s (0 = off, the default; 1 = 1:1 meters),
 *                        clamped to +-0.25 m horizontally and +-0.15 m vertically
 *   BB_VR_HEAD_FOV=game|hmd|<deg>  camera FOV: "hmd" = the symmetric frustum around both eyes'
 *                        (the default with BB_XR_MODE=mono or alt), "game" (the default
 *                        otherwise), or a vertical FOV in degrees; BB_VR_HEAD_ASPECT=<w/h> with
 *                        a fixed FOV
 *   BB_VR_FAKE_HEAD=...  headset-free head: "yaw,pitch[,roll[,x,y,z]]", "sweep[:yaw,pitch,s]",
 *                        or "@file" (vr_head_math.h; re-read when it changes)
 * Reference ("forward"): the yaw of the bridge's cinema screen, which it places in front of the
 * user and places again on a recenter (left stick click held); the head's own yaw at the first
 * tracked frame when the screen is head-locked or not placed. The fake head has no reference. */
#define _GNU_SOURCE
#include "runtime.h"
#include "hooks.h"
#include "vr_head_math.h"
#include "vr_fp.h"
#include "vr_stereo.h"
#include "vr_arms.h"
#include "vr_gun.h"
#include "vr_hitbox.h"
#include "gpu/bbgpu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define VH_MAX_OFFSET_H 0.25f /* findings/11 4.6: seated 6DoF clamp */
#define VH_MAX_OFFSET_V 0.15f

enum { VH_FOV_GAME = 0, VH_FOV_HMD = 1, VH_FOV_FIXED = 2 };

typedef struct {
    int valid;          /* a head pose is applied this frame */
    const char *source; /* "hmd" or "fake" */
    VrQuat head;        /* OpenXR orientation (logs) */
    float ref_yaw;      /* radians, the reference heading (logs) */
    VrQuat rel;         /* the head relative to the reference, game camera frame */
    VrVec3 offset;      /* game camera frame, meters, scaled and clamped */
    int apply;          /* bit 0 H1, bit 1 H2 */
    int level;
    int set_fov;        /* write fov_y (and aspect when > 0) */
    float fov_y, aspect;
    int lod;            /* the LOD fix may adjust contexts built from this camera */
    int64_t display_time; /* of the headset views the head came from (0: fake) */
    VrVec3 head_pos;    /* OpenXR, the views' world space */
} VhSample;

static struct {
    int ready, enabled, log, level, fov_mode, mono, overscan_logged;
    float pos_scale, fov_fixed, aspect_fixed, overscan_x, overscan_y;
    VrFakeHead fake;
    VrFakeFile file;
    int file_loaded;
    uint64_t file_checked, file_mtime, file_size;
    uint64_t start;
    /* reference */
    int have_ref, ref_from_screen;
    VrQuat ref;
    VrVec3 ref_pos;
    BbVrPose last_screen;
    /* per frame */
    uint64_t seq, h2_seq;
    VhSample frame;
    float h1_game[4][4];
    int h1_game_valid;
    int fp;             /* H1 put this frame's camera at the hunter's eyes (BB_VR_FP=1) */
    float h1_out[20];   /* H1's final camera: matrix, FOV, aspect, near, far */
    uint64_t next_log[2];
    int fired[2];
} vh;

/* LOD fix ("bb_hook_vr_lod" at SetCamera 0x2692049, replay-before, findings/08 section 6): the
 * context's LOD scale is tan(fov/2) / tan(0.48) / (height/720) (ctx+0x150, which also scales the
 * ctx+0x130 vector), so a wide VR FOV lowers every model's LOD (3x at 100 deg vertical). At the
 * hook xmm0 = tan(fov/2) / tan(0.48) and rbx = the context; for contexts built from the injected
 * camera (ctx+0x140 equals, bit for bit, the FOV that H1 wrote) it is multiplied by
 * tan(game fov/2) / tan(injected fov/2), so models keep the game's LOD distances. SetCamera runs
 * for every pass, possibly on other threads: H1 publishes {FOV bits, factor bits} atomically. */
static _Atomic uint64_t vh_lod;       /* injected FOV bits | factor bits << 32; 0: nothing to fix */
static _Atomic uint32_t vh_lod_hits;  /* contexts adjusted since the last log */

static ABI void vh_hook_lod(BbHookContext *ctx) {
    const uint64_t pair = atomic_load_explicit(&vh_lod, memory_order_relaxed);
    if (!pair) return;
    uint32_t fov;
    memcpy(&fov, (const unsigned char *)(uintptr_t)ctx->rbx + 0x140, 4);
    if (fov != (uint32_t)pair) return;
    const uint32_t factor_bits = (uint32_t)(pair >> 32);
    float factor, term;
    memcpy(&factor, &factor_bits, 4);
    memcpy(&term, ctx->ymm[0], 4);
    term *= factor;
    memcpy(ctx->ymm[0], &term, 4);
    atomic_fetch_add_explicit(&vh_lod_hits, 1, memory_order_relaxed);
}

static void vh_publish_lod(float game_fov, const VhSample *s) {
    uint64_t pair = 0;
    if (s->valid && (s->apply & 1) && s->set_fov && s->lod && game_fov > 0.0f) {
        const float factor = tanf(0.5f * game_fov) / tanf(0.5f * s->fov_y);
        uint32_t fov_bits, factor_bits;
        memcpy(&fov_bits, &s->fov_y, 4);
        memcpy(&factor_bits, &factor, 4);
        if (isfinite(factor) && factor > 0.0f) pair = fov_bits | (uint64_t)factor_bits << 32;
    }
    atomic_store_explicit(&vh_lod, pair, memory_order_relaxed);
}

static uint64_t vh_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static int env_on(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '1';
}

static void vh_init(void) {
    if (vh.ready) return;
    vh.ready = 1;
    vh.start = vh_now_us();
    vh.enabled = env_on("BB_VR_HEAD");
    vh.log = env_on("BB_VR_HEAD_LOG");
    vh.level = env_on("BB_VR_HEAD_LEVEL");
    const char *mode = getenv("BB_XR_MODE");
    /* mono and alt (alternate-eye stereo): the frustum that contains both eyes by default */
    vh.mono = mode && (!strcmp(mode, "mono") || !strcmp(mode, "alt"));
    const char *pos = getenv("BB_VR_HEAD_POS");
    vh.pos_scale = pos ? strtof(pos, NULL) : 0.0f;
    if (!isfinite(vh.pos_scale) || vh.pos_scale < 0.0f || vh.pos_scale > 4.0f) vh.pos_scale = 0.0f;
    const char *fov = getenv("BB_VR_HEAD_FOV");
    vh.fov_mode = vh.mono ? VH_FOV_HMD : VH_FOV_GAME;
    if (fov && !strcmp(fov, "game")) vh.fov_mode = VH_FOV_GAME;
    else if (fov && !strcmp(fov, "hmd")) vh.fov_mode = VH_FOV_HMD;
    else if (fov && *fov) {
        vh.fov_fixed = strtof(fov, NULL);
        if (isfinite(vh.fov_fixed) && vh.fov_fixed >= 10.0f && vh.fov_fixed <= 170.0f) vh.fov_mode = VH_FOV_FIXED;
        else fprintf(stderr, "VR head: BB_VR_HEAD_FOV=%s ignored (game, hmd or 10..170 degrees)\n", fov);
    }
    /* BB_XR_OVERSCAN=<v>[,<h>]: extra frustum (fraction of the tangents) beyond the union of the
     * eyes' views, so the compositor's reprojection of an older image has no black border.
     * Default 0.15 vertical, 0.10 horizontal; 0 turns it off. */
    vh.overscan_y = 0.15f;
    vh.overscan_x = 0.10f;
    const char *ov = getenv("BB_XR_OVERSCAN");
    if (ov && *ov) {
        char *end = NULL;
        const float v = strtof(ov, &end);
        if (end != ov && isfinite(v) && v >= 0.0f && v <= 1.0f) {
            vh.overscan_y = v;
            vh.overscan_x = v * (2.0f / 3.0f);
            if (*end == ',') {
                const float h = strtof(end + 1, NULL);
                if (isfinite(h) && h >= 0.0f && h <= 1.0f) vh.overscan_x = h;
            }
        } else {
            fprintf(stderr, "VR head: BB_XR_OVERSCAN=%s ignored (0..1 or vertical,horizontal)\n", ov);
        }
    }
    const char *aspect = getenv("BB_VR_HEAD_ASPECT");
    vh.aspect_fixed = aspect ? strtof(aspect, NULL) : 0.0f;
    if (!isfinite(vh.aspect_fixed) || vh.aspect_fixed < 0.2f || vh.aspect_fixed > 5.0f) vh.aspect_fixed = 0.0f;
    const char *fake = getenv("BB_VR_FAKE_HEAD");
    if (fake && *fake && !vr_fake_parse(fake, &vh.fake))
        fprintf(stderr, "VR head: BB_VR_FAKE_HEAD=%s not understood (yaw,pitch | sweep[:yaw,pitch,s] | @file)\n", fake);
    if (vh.enabled)
        fprintf(stderr, "VR head: on (source %s%s, fov %s, %s, position %s)\n",
                vh.fake.kind ? "fake " : "headset", vh.fake.kind ? fake : "",
                vh.fov_mode == VH_FOV_HMD ? "hmd" : vh.fov_mode == VH_FOV_FIXED ? "fixed" : "game",
                vh.level ? "horizon level" : "head in the camera frame",
                vh.pos_scale > 0.0f ? "on" : "off");
}

/* "@file" fake head: re-read (at most every 100 ms) when its time stamp or size changes. */
static void vh_poll_file(uint64_t now) {
    if (vh.file_loaded && now - vh.file_checked < 100000) return;
    vh.file_checked = now;
    struct stat st;
    if (stat(vh.fake.path, &st) != 0) {
        if (!vh.file_loaded) { vr_fake_file_parse("", &vh.file); vh.file_loaded = 1; }
        return;
    }
    if (vh.file_loaded && (uint64_t)st.st_mtime == vh.file_mtime && (uint64_t)st.st_size == vh.file_size) return;
    FILE *f = fopen(vh.fake.path, "rb");
    if (!f) return;
    char text[1024];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    vh.file_mtime = (uint64_t)st.st_mtime;
    vh.file_size = (uint64_t)st.st_size;
    vr_fake_file_parse(text, &vh.file);
    vh.file_loaded = 1;
    fprintf(stderr, "VR head: fake head file: yaw %.1f pitch %.1f roll %.1f pos (%.2f %.2f %.2f) apply %d fov %.1f aspect %.3f level %d lod %d\n",
            vh.file.v[0], vh.file.v[1], vh.file.v[2], vh.file.v[3], vh.file.v[4], vh.file.v[5],
            vh.file.apply, vh.file.fov_deg, vh.file.aspect, vh.file.level, vh.file.lod);
}

static VrQuat pose_q(const BbVrPose *p) {
    return vr_q(p->orientation[0], p->orientation[1], p->orientation[2], p->orientation[3]);
}
static VrVec3 pose_v(const BbVrPose *p) { return vr_v3(p->position[0], p->position[1], p->position[2]); }
static int same_pose(const BbVrPose *a, const BbVrPose *b) {
    for (int i = 0; i < 3; ++i) if (fabsf(a->position[i] - b->position[i]) > 1e-4f) return 0;
    for (int i = 0; i < 4; ++i) if (fabsf(a->orientation[i] - b->orientation[i]) > 1e-5f) return 0;
    return 1;
}

static void vh_sample(VhSample *s, uint64_t now) {
    memset(s, 0, sizeof(*s));
    s->apply = 3;
    s->level = vh.level;
    s->lod = 1;
    float fov_deg = vh.fov_mode == VH_FOV_FIXED ? vh.fov_fixed : 0.0f;
    float aspect = vh.aspect_fixed;
    if (vh.fake.kind) {
        VrVec3 pos = vr_v3(0, 0, 0);
        VrQuat q;
        if (vh.fake.kind == VR_FAKE_FILE) {
            vh_poll_file(now);
            const float *v = vh.file.v;
            q = vr_xr_ypr(v[0] * VR_DEG, v[1] * VR_DEG, v[2] * VR_DEG);
            pos = vr_v3(v[3], v[4], v[5]);
            s->apply = vh.file.apply;
            if (vh.file.level >= 0) s->level = vh.file.level;
            if (vh.file.fov_deg > 0.0f) fov_deg = vh.file.fov_deg;
            if (vh.file.aspect > 0.0f) aspect = vh.file.aspect;
            s->lod = vh.file.lod;
        } else {
            q = vr_fake_pose(&vh.fake, (double)(now - vh.start) * 1e-6, &pos);
        }
        s->valid = 1;
        s->source = "fake";
        s->head_pos = pos;
        s->head = q;
        s->rel = vr_head_rel_game(q, vr_qidentity());
        s->offset = vr_head_offset_game(pos, vr_v3(0, 0, 0), vr_qidentity(), vh.pos_scale,
                                        VH_MAX_OFFSET_H, VH_MAX_OFFSET_V);
    } else {
        BbVrViews v;
        if (!bbgpu_vr_views(&v)) return;
        const VrQuat e0 = pose_q(&v.eye[0].pose), e1 = pose_q(&v.eye[1].pose);
        const int eyes = vr_qvalid(e0) && vr_qvalid(e1);
        VrQuat q = pose_q(&v.head);
        VrVec3 pos = pose_v(&v.head);
        if (!vr_qvalid(q)) {
            if (!eyes) return;
            q = vr_qnlerp(e0, e1, 0.5f); /* the middle of the eyes (the VIEW space) */
            pos = vr_scale(vr_add(pose_v(&v.eye[0].pose), pose_v(&v.eye[1].pose)), 0.5f);
        }
        q = vr_qnormalize(q);
        /* Reference: the cinema screen's yaw while it is placed in the world. */
        const VrQuat screen = pose_q(&v.screen);
        if ((v.screen_flags & 1) && !(v.screen_flags & 2) && vr_qvalid(screen)) {
            if (!vh.ref_from_screen || !same_pose(&v.screen, &vh.last_screen)) {
                vh.ref = vr_xr_yaw_only(screen);
                vh.ref_pos = pos;
                fprintf(stderr, "VR head: %s: forward = the screen's yaw %.1f deg\n",
                        vh.ref_from_screen ? "recentered (screen placed again)" : "reference set",
                        vr_xr_yaw(vh.ref) / VR_DEG);
            }
            vh.ref_from_screen = vh.have_ref = 1;
            vh.last_screen = v.screen;
        } else if (!vh.have_ref || vh.ref_from_screen) {
            vh.ref = vr_xr_yaw_only(q);
            vh.ref_pos = pos;
            vh.have_ref = 1;
            vh.ref_from_screen = 0;
            fprintf(stderr, "VR head: reference set from the head: forward yaw %.1f deg\n",
                    vr_xr_yaw(vh.ref) / VR_DEG);
        }
        s->valid = 1;
        s->source = "hmd";
        s->display_time = v.display_time_ns;
        s->head_pos = pos;
        s->head = q;
        s->ref_yaw = vr_xr_yaw(vh.ref);
        s->rel = vr_head_rel_game(q, vh.ref);
        if (vh.pos_scale > 0.0f && (v.flags & 2))
            s->offset = vr_head_offset_game(pos, vh.ref_pos, vh.ref, vh.pos_scale, VH_MAX_OFFSET_H,
                                            VH_MAX_OFFSET_V);
        if (vh.fov_mode == VH_FOV_HMD && eyes) {
            const VrQuat to_head = vr_qconj(q);
            const VrQuat in_head[2] = {vr_qmul(to_head, e0), vr_qmul(to_head, e1)};
            const VrFov fov[2] = {
                {v.eye[0].fov_left, v.eye[0].fov_right, v.eye[0].fov_up, v.eye[0].fov_down},
                {v.eye[1].fov_left, v.eye[1].fov_right, v.eye[1].fov_up, v.eye[1].fov_down}};
            float tx, ty;
            if (vr_union_tangents(in_head, fov, &tx, &ty)) {
                const float dx = tx, dy = ty;
                if (vh.mono) vr_overscan_tangents(&tx, &ty, vh.overscan_x, vh.overscan_y);
                vr_fov_from_tangents(tx, ty, &s->fov_y, &s->aspect);
                s->set_fov = 1;
                if (!vh.overscan_logged) {
                    vh.overscan_logged = 1;
                    fprintf(stderr, "VR head: render fov %.1f x %.1f deg, display fov %.1f x %.1f deg (overscan %.0f%% x, %.0f%% y%s)\n",
                            2.0f * atanf(tx) / VR_DEG, 2.0f * atanf(ty) / VR_DEG, 2.0f * atanf(dx) / VR_DEG,
                            2.0f * atanf(dy) / VR_DEG, vh.overscan_x * 100.0f, vh.overscan_y * 100.0f,
                            vh.mono ? "" : ", not applied: stereo");
                }
            }
        }
    }
    if (fov_deg > 0.0f) {
        s->set_fov = 1;
        s->fov_y = fov_deg * VR_DEG;
        s->aspect = aspect;
    }
    if (s->set_fov) {
        s->fov_y = vr_clampf(s->fov_y, 10.0f * VR_DEG, 170.0f * VR_DEG);
        if (s->aspect != 0.0f) s->aspect = vr_clampf(s->aspect, 0.2f, 5.0f);
    }
}

/* The composed camera, or 0 when it would not be finite (then the game's stays). */
static int vh_compose(float out[4][4], const float in[4][4], const VhSample *s) {
    memcpy(out, in, 16 * sizeof(float));
    vr_cam_compose(out, s->rel, s->offset, s->level);
    for (int i = 0; i < 16; ++i) if (!isfinite(out[i / 4][i % 4])) return 0;
    return 1;
}

static void vh_first_fire(int hook, const BbHookContext *ctx) {
    if (vh.fired[hook]) return;
    vh.fired[hook] = 1;
    fprintf(stderr, "VR head: %s hook fired at guest rip=0x%llx rbx=0x%llx r12=0x%llx%s\n",
            hook ? "H2" : "H1", (unsigned long long)ctx->rip, (unsigned long long)ctx->rbx,
            (unsigned long long)ctx->r12, vh.enabled ? "" : " (BB_VR_HEAD is not 1: camera unchanged)");
}

static int vh_log_due(int hook, uint64_t now) {
    if (!vh.log || now < vh.next_log[hook]) return 0;
    vh.next_log[hook] = now + 1000000;
    return 1;
}

static void log_rows(const char *what, const float m[4][4]) {
    fprintf(stderr, "VR head:   %s right (%.4f %.4f %.4f | %.3g) up (%.4f %.4f %.4f | %.3g) fwd (%.4f %.4f %.4f | %.3g) pos (%.3f %.3f %.3f | %.3g)\n",
            what, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
            m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
}

static void log_sample(const VhSample *s) {
    float yaw, pitch, roll;
    vr_xr_angles(s->head, &yaw, &pitch, &roll);
    fprintf(stderr, "VR head:   head (%s) yaw %.1f pitch %.1f roll %.1f deg, forward yaw %.1f deg, offset (%.3f %.3f %.3f), apply %s%s, %s, fov %s %.4f rad aspect %.4f\n",
            s->source, yaw / VR_DEG, pitch / VR_DEG, roll / VR_DEG, s->ref_yaw / VR_DEG,
            s->offset.x, s->offset.y, s->offset.z, s->apply & 1 ? "H1" : "", s->apply & 2 ? "+H2" : "",
            s->level ? "level" : "camera frame", s->set_fov ? "set" : "game", s->fov_y, s->aspect);
}

/* The view the camera was just turned to, for BB_XR_MODE=mono (bridge ABI 5): the head pose
 * used, the camera's final symmetric frustum (fov_y, aspect as tangents) and its rotation rows,
 * which the presenter matches against the presented frame's scene camera. */
static void vh_publish_render_view(const float m[4][4], float fov_y, float aspect) {
    const VhSample *s = &vh.frame;
    const float ty = tanf(0.5f * fov_y), tx = ty * aspect;
    if (!s->valid || !(ty > 0.0f) || !(tx > 0.0f) || !isfinite(tx) || !isfinite(ty)) return;
    BbVrRenderView view;
    memset(&view, 0, sizeof(view));
    view.seq = vh.seq;
    view.display_time = s->display_time;
    view.head.position[0] = s->head_pos.x;
    view.head.position[1] = s->head_pos.y;
    view.head.position[2] = s->head_pos.z;
    view.head.orientation[0] = s->head.x;
    view.head.orientation[1] = s->head.y;
    view.head.orientation[2] = s->head.z;
    view.head.orientation[3] = s->head.w;
    view.fov_left = -atanf(tx);
    view.fov_right = atanf(tx);
    view.fov_up = atanf(ty);
    view.fov_down = -atanf(ty);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) view.rows[i][j] = m[i][j];
    bbgpu_vr_set_render_view(&view);
}

/* Alternate-eye stereo (BB_XR_MODE=alt, src/runtime_vr_stereo.c, findings/22): the last step of
 * H1's camera, after the head, first person and the FOV: this frame's eye offset and eye tag. */
static void vh_stereo(float *cam) {
    const VhSample *s = &vh.frame;
    VrStereoHead head;
    memset(&head, 0, sizeof(head));
    head.valid = s->valid;
    head.from_headset = s->source && !strcmp(s->source, "hmd");
    head.head = s->head;
    head.head_pos = s->head_pos;
    head.display_time = s->display_time;
    head.frame = vh.seq;
    vr_stereo_h1(cam, &head);
}

/* First person (BB_VR_FP=1, src/runtime_vr_fp.c, findings/16): the camera at the hunter's eyes,
 * heading from the game camera or snap turns, the headset's rotation with the horizon level, and
 * a near plane that hides the own head. Returns 1 when it set this frame's camera. */
static int vh_first_person(const BbHookContext *ctx, float *cam, int logging) {
    float (*m)[4] = (float (*)[4])cam;
    const int head_ok = vh.frame.valid && (vh.frame.apply & 1);
    VrFpView fpv;
    /* ref_yaw: the reference's heading (OpenXR yaw is left positive); it moves on a recenter */
    if (!vr_fp_frame(ctx, (const float (*)[4])vh.h1_game, head_ok, vh.frame.rel, -vh.frame.ref_yaw, &fpv)) return 0;
    float out[4][4];
    if (fpv.keep_game_rotation) { /* flat: the game's own rotation, only the position moves */
        memcpy(out, vh.h1_game, sizeof(out));
        for (int j = 0; j < 3; ++j) out[3][j] = fpv.rows[3][j];
    } else {
        memcpy(out, fpv.rows, sizeof(out));
        if (head_ok) vr_cam_compose(out, vh.frame.rel, vh.frame.offset, 1);
    }
    int finite = 1;
    for (int i = 0; i < 16; ++i) finite &= isfinite(out[i / 4][i % 4]) != 0;
    if (!finite) return 0;
    { /* VR arms (BB_VR_ARMS=1, findings/24): the camera and the head it shows */
        const float hp[3] = {vh.frame.head_pos.x, vh.frame.head_pos.y, vh.frame.head_pos.z};
        const float hq[4] = {vh.frame.head.x, vh.frame.head.y, vh.frame.head.z, vh.frame.head.w};
        vr_arms_view((const float (*)[4])out, head_ok && !fpv.keep_game_rotation, hp, hq);
        vr_gun_view((const float (*)[4])out); /* VR gun aim (BB_VR_GUN_AIM=1, findings/27) */
    }
    memcpy(m, out, sizeof(out));
    if (fpv.near_plane > 0.0f) cam[18] = fpv.near_plane;
    if (vh.frame.valid && vh.frame.set_fov) {
        cam[16] = vh.frame.fov_y;
        if (vh.frame.aspect > 0.0f) cam[17] = vh.frame.aspect;
    }
    memcpy(vh.h1_out, cam, sizeof(vh.h1_out)); /* H2's camera: before the stereo eye offset */
    vh_stereo(cam);
    vh_publish_render_view((const float (*)[4])m, cam[16], cam[17]);
    if (logging) {
        log_sample(&vh.frame);
        log_rows("fp  ", (const float (*)[4])m);
    }
    return 1;
}

/* H1: rbx = the draw step's private PersCam. */
static ABI void vh_hook_h1(BbHookContext *ctx) {
    vh_init();
    vh_first_fire(0, ctx);
    const int fp_on = vr_fp_enabled();
    vh.fp = 0;
    if (!vh.enabled && !fp_on) return;
    const uint64_t now = vh_now_us();
    ++vh.seq;
    if (vh.enabled) vh_sample(&vh.frame, now);
    else memset(&vh.frame, 0, sizeof(vh.frame));
    float *cam = (float *)(uintptr_t)(ctx->rbx + 0x10); /* +0x10 matrix, then fov aspect near far */
    float (*m)[4] = (float (*)[4])cam;
    memcpy(vh.h1_game, m, sizeof(vh.h1_game));
    vh.h1_game_valid = 1;
    const int logging = vh_log_due(0, now);
    if (logging) {
        float det;
        const float ortho = vr_cam_ortho_error((const float (*)[4])m, &det);
        fprintf(stderr, "VR head: H1 PersCam 0x%llx fov %.4f aspect %.4f near %.4f far %.1f, ortho error %.2g det %+.4f heading %.1f deg\n",
                (unsigned long long)ctx->rbx, cam[16], cam[17], cam[18], cam[19], ortho, det,
                vr_cam_heading((const float (*)[4])m) / VR_DEG);
        log_rows("game", (const float (*)[4])m);
        /* The copies built from the previous frame's H1 camera (findings/08 section 3). */
        const unsigned char *mgr = (const unsigned char *)(uintptr_t)ctx->r12;
        const float *draw = *(float *const *)(mgr + 0x18), *scene = *(float *const *)(mgr + 0x10);
        if (draw && scene)
            fprintf(stderr, "VR head:   previous frame: draw camera fov %.4f aspect %.4f, scene camera fov %.4f, culling copy fov %.4f\n",
                    draw[20], draw[25], scene[20], scene[(0x90 + 0x50) / 4]);
    }
    vh_publish_lod(cam[16], &vh.frame);
    if (logging) {
        const uint32_t hits = atomic_exchange_explicit(&vh_lod_hits, 0, memory_order_relaxed);
        if (hits) fprintf(stderr, "VR head:   LOD fix: %u contexts kept at the game's LOD distances in the last second\n", hits);
    }
    if (fp_on && (vh.fp = vh_first_person(ctx, cam, logging))) return;
    if (!vh.enabled) return;
    if (!vh.frame.valid) {
        if (logging) fprintf(stderr, "VR head:   no head pose: camera unchanged\n");
        return;
    }
    if (vh.frame.apply & 1) {
        float out[4][4];
        if (vh_compose(out, (const float (*)[4])m, &vh.frame)) memcpy(m, out, sizeof(out));
        if (vh.frame.set_fov) {
            cam[16] = vh.frame.fov_y;
            if (vh.frame.aspect > 0.0f) cam[17] = vh.frame.aspect;
        }
        vh_stereo(cam);
        vh_publish_render_view((const float (*)[4])m, cam[16], cam[17]);
    }
    if (logging) {
        log_sample(&vh.frame);
        log_rows("out ", (const float (*)[4])m);
    }
}

/* H2 (after the displaced bytes built it): rbx = the format-B camera for the r12+0x50 context. */
static ABI void vh_hook_h2(BbHookContext *ctx) {
    vh_init();
    vh_first_fire(1, ctx);
    if (vh.fp && vr_fp_active()) { /* first person: the same camera as H1's */
        float *cam = (float *)(uintptr_t)ctx->rbx;
        memcpy(cam, vh.h1_out, 16 * sizeof(float));
        cam[20] = vh.h1_out[16]; /* FOV */
        cam[21] = vh.h1_out[18]; /* near */
        cam[25] = vh.h1_out[17]; /* aspect */
        vr_stereo_h2(cam, vh.seq); /* alternate-eye stereo: the eye offset if H2 follows the eye (findings/22) */
        return;
    }
    if (!vh.enabled) return;
    const uint64_t now = vh_now_us();
    if (vh.h2_seq == vh.seq) { /* no H1 since the last H2: H1 is not installed */
        ++vh.seq;
        vh_sample(&vh.frame, now);
        vh.h1_game_valid = 0;
    }
    vh.h2_seq = vh.seq;
    float *cam = (float *)(uintptr_t)ctx->rbx; /* +0x00 matrix, +0x50 fov, +0x64 aspect */
    float (*m)[4] = (float (*)[4])cam;
    const int logging = vh_log_due(1, now);
    if (logging) {
        float diff = -1.0f;
        if (vh.h1_game_valid) {
            diff = 0.0f;
            for (int i = 0; i < 16; ++i) {
                const float d = fabsf(m[i / 4][i % 4] - vh.h1_game[i / 4][i % 4]);
                diff = d > diff ? d : diff;
            }
        }
        const uint32_t *size = (const uint32_t *)(cam + 23);
        const unsigned char *ctx50 = (const unsigned char *)(uintptr_t)ctx->r12 + 0x50;
        const float *c = (const float *)ctx50;
        fprintf(stderr, "VR head: H2 format-B fov %.4f near %.4f far %.1f size %ux%u aspect %.4f jitter %u; game matrix vs H1's: %s %.3g\n",
                cam[20], cam[21], cam[22], size[0], size[1], cam[25], ((const unsigned char *)cam)[0x68],
                diff < 0.0f ? "no H1" : "max diff", diff < 0.0f ? 0.0f : diff);
        fprintf(stderr, "VR head:   previous frame's main context: fov %.4f aspect %.4f P00 %.4f P11 %.4f LOD scale %.4f\n",
                c[0x140 / 4], c[0x14c / 4], c[0x90 / 4], c[0x90 / 4 + 5], c[0x150 / 4]);
    }
    if (!vh.frame.valid || !(vh.frame.apply & 2)) return;
    float out[4][4];
    if (vh_compose(out, (const float (*)[4])m, &vh.frame)) memcpy(m, out, sizeof(out));
    if (vh.frame.set_fov) {
        cam[20] = vh.frame.fov_y;
        if (vh.frame.aspect > 0.0f) cam[25] = vh.frame.aspect;
    }
    vr_stereo_h2(cam, vh.seq); /* alternate-eye stereo: the eye offset if H2 follows the eye (findings/22) */
    if (logging && !vh.h1_game_valid) {
        log_sample(&vh.frame);
        log_rows("out ", (const float (*)[4])m);
    }
}

void runtime_vr_head_register(void) {
    bb_hook_set_camera_impl(vh_hook_h1, vh_hook_h2, vh_hook_lod);
    vr_fp_register();
    vr_stereo_register();
    vr_arms_register();
    vr_gun_register();
    vr_hitscale_register();
    vr_atkspeed_register();
}
