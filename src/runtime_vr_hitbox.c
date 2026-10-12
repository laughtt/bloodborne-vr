/* The game's hitbox debug overlay, host side (docs/vr/findings/29-game-hitboxes.md). The VR UI layer
 * (bbport_xr.dll, gpu/xr_bridge/vr_game_hitbox.*) draws the VR sandbox's weapon hit capsules, the
 * player's hurtboxes and hit markers over the game; this file gives it where they are:
 *   - every pad poll (src/runtime_vr_ui.c, BB_VR_UI=1) it reads the player's model-space pose (the
 *     hkaPose the arms hook src/runtime_vr_arms.c writes: [[[P + 0x3B0] + 0x30] + 0x28] + 0x38, its
 *     modelPose array +0x18 and skeleton +0x00), read only, through its own readable-memory guard;
 *   - it takes each bone into the headset's space through the first-person camera and the head pose
 *     that camera showed (vr_arms_hitbox_view, the inverse of the arms' controller placement:
 *     src/vr_hitbox_math.h) and sends, per bone, the skinning matrix (pose . inverse bind) and, for
 *     L_Weapon / R_Weapon, the bone frame, as 3x4 matrices the bridge applies to the bbhitbox data;
 *   - the stamina hit hooks (src/runtime_vr_stamina.c, BB_VR_STAMINA) report the player's hits:
 *     their contact point (hit record +0x180, world) is kept 3 s and sent in headset space.
 * Needs first person (the H1 camera) and a headset pose; otherwise the block is zero (nothing drawn).
 * Environment: BB_VR_HITBOX=0 sends nothing (default on with BB_VR_UI=1; what is drawn is the
 * "game" object's "debug_hitboxes" in out/vr_assets/ui_layouts.json). */
#define _GNU_SOURCE
#include "runtime.h"
#include "vr_mem_cache.h"
#include "gpu/xr_bridge/bbport_xr_bridge.h"
#include "vr_hit_scale.h"
#include "vr_hitbox.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define VA_BASE 0x400000u
#define VA_WORLD_CHR_MAN 0x593E878u
#define VA_PLAYER_INS_VTABLE 0x578F310u
#define HB_MAX_BONES 256
#define HB_VIEW_STALE_US 300000u
#define HB_HIT_KEEP_S 3.0
#define HB_HIT_RANGE_M 15.0f

static double hb_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- guest memory (own guard; the hit hook and the pad poll may run on different threads) ---- */
#ifdef _WIN32
/* page-probed reads with a per-thread cache (vr_mem_cache.h): VirtualQuery on the guest mappings took ms per cold lookup (run12e/g profiles) */
static _Thread_local VmCache hb_cache;
static int hb_readable(uintptr_t a, size_t n) { return vm_readable(&hb_cache, a, n); }
#else
static int hb_readable(uintptr_t a, size_t n) {
    int fds[2];
    if (a < 0x10000 || a + n < a || pipe(fds) != 0) return 0;
    const ssize_t w = write(fds[1], (const void *)a, n > 4096 ? 4096 : n);
    close(fds[0]);
    close(fds[1]);
    return w >= 0;
}
#endif

static int hb_read(uintptr_t a, void *out, size_t n) {
    if (!hb_readable(a, n)) return 0;
    memcpy(out, (const void *)a, n);
    return 1;
}
static int hb_ptr(uintptr_t a, uintptr_t *out) {
    uint64_t v;
    if (!hb_read(a, &v, 8) || v < 0x10000 || (v & 7)) return 0;
    *out = (uintptr_t)v;
    return 1;
}
/* hkArray { T *data; int32 size; int32 capacityAndFlags; } */
static int hb_array(uintptr_t a, uintptr_t *data, int *size) {
    uint64_t d;
    int32_t n;
    if (!hb_read(a, &d, 8) || !hb_read(a + 8, &n, 4) || !d || (d & 3) || n <= 0 || n > HB_MAX_BONES) return 0;
    *data = (uintptr_t)d;
    *size = n;
    return 1;
}

/* ---- settings ---- */
static struct {
    int checked, on;
    /* per skeleton */
    uintptr_t skel;
    int n_bones, ok, weapon[2];
    int16_t parent[HB_MAX_BONES];
    uint32_t hash[HB_MAX_BONES];
    VrXform bind[HB_MAX_BONES];
    int logged_view, logged_pose;
    /* the VR hit scale (src/vr_hit_scale.h, findings/32): fp.txt hit_* keys, BB_VR_HIT_SCALE=1 */
    VhsParams hit;
    const char *control;
    double control_checked;
    long long control_mtime, control_size;
} hb;

static void hb_init(void) {
    if (hb.checked) return;
    hb.checked = 1;
    const char *e = getenv("BB_VR_HITBOX");
    hb.on = !(e && e[0] == '0');
    hb.weapon[0] = hb.weapon[1] = -1;
    hb.hit = vhs_defaults();
    const char *h = getenv("BB_VR_HIT_SCALE");
    hb.hit.enabled = h && h[0] == '1';
    hb.control = getenv("BB_VR_ARMS_CONTROL");
    if (!hb.control || !*hb.control) hb.control = getenv("BB_VR_FP_CONTROL");
}

/* out/fp.txt (the arms' and first person's control file): only the hit* keys are ours. Polled ~4 Hz. */
static void hb_poll_control(double now) {
    if (!hb.control || now - hb.control_checked < 0.25) return;
    hb.control_checked = now;
    struct stat st;
    if (stat(hb.control, &st) != 0) return;
    if ((long long)st.st_mtime == hb.control_mtime && (long long)st.st_size == hb.control_size) return;
    hb.control_mtime = (long long)st.st_mtime;
    hb.control_size = (long long)st.st_size;
    FILE *f = fopen(hb.control, "rb");
    if (!f) return;
    char text[4096];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    for (char *save = NULL, *tok = strtok_r(text, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        vhs_apply_key(&hb.hit, tok, eq + 1);
    }
    fprintf(stderr, "VR hitbox: hit scale %s: radius x%.2f reach %+.3f m (short %.2f/%+.3f, medium %.2f/%+.3f, long %.2f/%+.3f; <0 = global)\n",
            hb.hit.enabled ? "on" : "off (preview with hit=1)", hb.hit.radius_scale, hb.hit.reach_m, hb.hit.class_radius_scale[0],
            hb.hit.class_reach_m[0], hb.hit.class_radius_scale[1], hb.hit.class_reach_m[1], hb.hit.class_radius_scale[2],
            hb.hit.class_reach_m[2]);
}

/* ---- the player's hits (the game thread writes, the pad poll reads) ---- */
typedef struct {
    float pos[3];
    int32_t atk_id;
    uint32_t contact;
    double t;
} HbHit;
static HbHit hb_hits[BBXR_DEBUG_MAX_HITS];
static _Atomic uint32_t hb_hit_seq; /* hits written so far; entry (seq - 1) % N is the newest */
static _Atomic uint32_t hb_hit_lock;

void vr_hitbox_hit(int contact, int32_t atk_id, uintptr_t record) {
    vr_hitscale_hit(contact, atk_id);
    hb_init();
    if (!hb.on) return;
    float p[3];
    if (!hb_read(record + 0x180, p, sizeof(p)) || !vhb_finite(p, 3)) return;
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong(&hb_hit_lock, &expected, 1)) return; /* the reader has it: drop */
    const uint32_t seq = atomic_load_explicit(&hb_hit_seq, memory_order_relaxed);
    HbHit *h = &hb_hits[seq % BBXR_DEBUG_MAX_HITS];
    memcpy(h->pos, p, sizeof(p));
    h->atk_id = atk_id;
    h->contact = (uint32_t)contact;
    h->t = hb_now();
    atomic_store_explicit(&hb_hit_seq, seq + 1, memory_order_relaxed);
    atomic_store_explicit(&hb_hit_lock, 0, memory_order_release);
}

/* ---- the skeleton (names, parents, bind pose), once per skeleton ---- */
static int hb_bone_name(uintptr_t names, int i, char *out, size_t cap) {
    uint64_t s;
    if (!hb_read(names + (uintptr_t)i * 16, &s, 8)) return 0;
    s &= ~(uint64_t)1; /* hkStringPtr: bit 0 is the "owned" flag */
    if (!s) return 0;
    size_t n = 0;
    for (; n + 1 < cap; ++n) {
        char c;
        if (!hb_read((uintptr_t)s + n, &c, 1)) return 0;
        out[n] = c;
        if (!c) return 1;
    }
    out[n] = 0;
    return 1;
}

static int hb_skeleton(uintptr_t skel) {
    if (skel == hb.skel) return hb.ok;
    hb.skel = skel;
    hb.ok = 0;
    hb.weapon[0] = hb.weapon[1] = -1;
    uintptr_t parents, names, ref;
    int n_parents, n_names, n_ref;
    if (!hb_array(skel + 0x18, &parents, &n_parents) || !hb_array(skel + 0x28, &names, &n_names) ||
        !hb_array(skel + 0x38, &ref, &n_ref) || n_parents != n_names || n_ref != n_names ||
        !hb_read(parents, hb.parent, (size_t)n_names * 2)) {
        fprintf(stderr, "VR hitbox: skeleton 0x%llx unreadable\n", (unsigned long long)skel);
        return 0;
    }
    hb.n_bones = n_names;
    for (int i = 0; i < n_names; ++i) {
        char name[96];
        hb.hash[i] = hb_bone_name(names, i, name, sizeof(name)) ? vhb_hash(name) : 0;
        if (!strcmp(name, "L_Weapon")) hb.weapon[0] = i;
        if (!strcmp(name, "R_Weapon")) hb.weapon[1] = i;
        float qs[12];
        VrXform local;
        if (hb.parent[i] >= i || !hb_read(ref + (uintptr_t)i * 48, qs, sizeof(qs)) || !vr_xf_from_qs(qs, &local)) {
            fprintf(stderr, "VR hitbox: skeleton 0x%llx: bone %d unusable\n", (unsigned long long)skel, i);
            return 0;
        }
        hb.bind[i] = hb.parent[i] >= 0 ? vr_xf_mul(hb.bind[hb.parent[i]], local) : local;
    }
    hb.ok = 1;
    fprintf(stderr, "VR hitbox: player skeleton: %d bones, L_Weapon %d, R_Weapon %d\n", n_names, hb.weapon[0], hb.weapon[1]);
    return 1;
}

/* The player's hkaPose (holder + 0x38), 0 when there is none. */
static uintptr_t hb_player_pose(unsigned char *image) {
    uintptr_t wcm, player, bag, beh, holder;
    uint64_t vt;
    if (!image) return 0;
    const uintptr_t base = (uintptr_t)image - VA_BASE;
    if (!hb_ptr(base + VA_WORLD_CHR_MAN, &wcm) || !hb_ptr(wcm + 0x60, &player) || !hb_read(player, &vt, 8) ||
        vt != (uint64_t)(base + VA_PLAYER_INS_VTABLE) || !hb_ptr(player + 0x3B0, &bag) || !hb_ptr(bag + 0x30, &beh) ||
        !hb_ptr(beh + 0x28, &holder))
        return 0;
    return holder + 0x38;
}

void vr_hitbox_fill(struct BbXrGameDebug *d, unsigned char *image) {
    hb_init();
    memset(d, 0, sizeof(*d));
    if (!hb.on) return;
    hb_poll_control(hb_now());
    d->hit_scale.valid = 1;
    d->hit_scale.enabled = (uint32_t)(hb.hit.enabled != 0);
    d->hit_scale.radius_scale = hb.hit.radius_scale;
    d->hit_scale.reach_m = hb.hit.reach_m;
    for (int i = 0; i < 3; ++i) {
        d->hit_scale.class_radius_scale[i] = hb.hit.class_radius_scale[i];
        d->hit_scale.class_reach_m[i] = hb.hit.class_reach_m[i];
    }
    VhbView view;
    if (!vr_arms_hitbox_view(&view, HB_VIEW_STALE_US)) {
        if (!hb.logged_view) {
            hb.logged_view = 1;
            fprintf(stderr, "VR hitbox: waiting for a first-person frame with a headset pose (nothing drawn until then)\n");
        }
        return;
    }
    const double now = hb_now();
    /* hits (newest first), kept HB_HIT_KEEP_S near the player */
    {
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong(&hb_hit_lock, &expected, 1)) {
            const uint32_t seq = atomic_load_explicit(&hb_hit_seq, memory_order_relaxed);
            for (uint32_t k = 0; k < BBXR_DEBUG_MAX_HITS && k < seq; ++k) {
                const HbHit *h = &hb_hits[(seq - 1 - k) % BBXR_DEBUG_MAX_HITS];
                const double age = now - h->t;
                const VrVec3 w = vr_v3(h->pos[0], h->pos[1], h->pos[2]);
                if (age < 0.0 || age > HB_HIT_KEEP_S || vr_dot(vr_sub(w, view.body_pos), vr_sub(w, view.body_pos)) > HB_HIT_RANGE_M * HB_HIT_RANGE_M) continue;
                const VrVec3 x = vhb_world_to_xr(&view, w);
                BbXrDebugHit *o = &d->hits[d->hit_count++];
                o->pos[0] = x.x;
                o->pos[1] = x.y;
                o->pos[2] = x.z;
                o->age_s = (float)age;
                o->atk_id = h->atk_id;
                o->contact = h->contact;
            }
            atomic_store_explicit(&hb_hit_lock, 0, memory_order_release);
        }
    }
    /* the pose */
    const uintptr_t pose = hb_player_pose(image);
    uintptr_t skel, model;
    int n;
    if (!pose || !hb_ptr(pose, &skel) || !hb_skeleton(skel) || !hb_array(pose + 0x18, &model, &n) || n != hb.n_bones ||
        !hb_readable(model, (size_t)n * 48)) {
        if (!hb.logged_pose) {
            hb.logged_pose = 1;
            fprintf(stderr, "VR hitbox: no player pose yet (pose 0x%llx)\n", (unsigned long long)pose);
        }
        return;
    }
    uint32_t count = 0;
    for (int i = 0; i < n && count < BBXR_DEBUG_MAX_BONES; ++i) {
        float qs[12];
        VrXform now_x;
        memcpy(qs, (const void *)(model + (uintptr_t)i * 48), sizeof(qs));
        if (!vr_xf_from_qs(qs, &now_x)) continue;
        for (int side = 0; side < 2; ++side) {
            if (i != hb.weapon[side]) continue;
            vhb_affine(&view, now_x, d->weapon[side]);
            if (vhb_finite(d->weapon[side], 12)) d->weapon_valid |= 1u << side;
        }
        if (!hb.hash[i]) continue;
        BbXrDebugBone *b = &d->bones[count];
        b->name_hash = hb.hash[i];
        vhb_affine(&view, vhb_skin(now_x, hb.bind[i]), b->skin);
        if (vhb_finite(b->skin, 12)) ++count;
    }
    d->bone_count = count;
    if (count || d->weapon_valid) d->flags |= BBXR_DEBUG_POSE;
}
