/* Pure math for the custom character model override (BB_VR_SKIN, src/runtime_vr_skin.c): where
 * another player's character is in the headset's space, how fast it moves, which of the found
 * characters to send and whether a UTF-16 name is in a memory block. No I/O, no globals:
 * tests/test_vr_skin.c checks it.
 *
 * Game space and headset space differ by a reflection (vr_xr_to_game_vec), so points are mapped
 * (feet, feet + forward, feet + up) and the directions are the normalised differences. */
#ifndef VR_SKIN_MATH_H
#define VR_SKIN_MATH_H
#include "vr_hitbox_math.h"
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define VSK_MAX 4

static inline VrVec3 vsk_unit(VrVec3 v, VrVec3 fallback) {
    const float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 1e-6f ? vr_scale(v, 1.0f / l) : fallback;
}

/* A character at world position `pos` with the physics yaw `yaw` (its face is model -Z) in the
 * headset's space: feet, unit forward, unit up. */
static inline void vsk_target_from_world(const VhbView *v, VrVec3 pos, float yaw, float out_pos[3],
                                         float out_fwd[3], float out_up[3]) {
    const VrVec3 face = vr_sub(vr_model_to_world(pos, yaw, vr_v3(0, 0, -1)), pos);
    const VrVec3 p0 = vhb_world_to_xr(v, pos);
    const VrVec3 pf = vhb_world_to_xr(v, vr_add(pos, face));
    const VrVec3 pu = vhb_world_to_xr(v, vr_add(pos, vr_v3(0, 1, 0)));
    const VrVec3 f = vsk_unit(vr_sub(pf, p0), vr_v3(0, 0, -1));
    const VrVec3 u = vsk_unit(vr_sub(pu, p0), vr_v3(0, 1, 0));
    out_pos[0] = p0.x; out_pos[1] = p0.y; out_pos[2] = p0.z;
    out_fwd[0] = f.x; out_fwd[1] = f.y; out_fwd[2] = f.z;
    out_up[0] = u.x; out_up[1] = u.y; out_up[2] = u.z;
}

/* ---- horizontal speed (smoothed) ---- */
typedef struct {
    int init;
    VrVec3 last;
    double t;
    float speed;
} VskSpeed;

#define VSK_SPEED_ALPHA 0.35f
#define VSK_SPEED_MIN_DT 0.002
#define VSK_SPEED_MAX_DT 0.25
#define VSK_TELEPORT_M 10.0f

static inline float vsk_speed_update(VskSpeed *s, VrVec3 pos, double now_s) {
    if (!s->init) {
        s->init = 1; s->last = pos; s->t = now_s; s->speed = 0.0f;
        return 0.0f;
    }
    const double dt = now_s - s->t;
    const float dx = pos.x - s->last.x, dz = pos.z - s->last.z;
    const float d = sqrtf(dx * dx + dz * dz);
    if (d > VSK_TELEPORT_M || dt < 0.0 || dt > 2.0) { /* teleport or a long gap: start over */
        s->last = pos; s->t = now_s; s->speed = 0.0f;
        return 0.0f;
    }
    if (dt < VSK_SPEED_MIN_DT) return s->speed; /* same frame: keep the old sample */
    const double cdt = dt > VSK_SPEED_MAX_DT ? VSK_SPEED_MAX_DT : dt;
    const float inst = d / (float)cdt;
    s->speed += VSK_SPEED_ALPHA * (inst - s->speed);
    s->last = pos; s->t = now_s;
    return s->speed;
}

/* ---- target selection ---- */
typedef struct {
    int slot;        /* stable discovery slot */
    float dist;      /* metres from the local player */
    int name_match;  /* its name matches the selector (name selectors only) */
} VskCand;

/* Selector: NULL / "" / "all": all, nearest first; digits: the candidate whose slot is that
 * index; anything else: a name, the candidates with name_match, nearest first. Writes up to
 * VSK_MAX indices into `cands` (nearest first) to out; returns the count. */
static inline int vsk_select(const VskCand *c, int n, const char *sel, int out[VSK_MAX]) {
    int mode = 0, index = -1; /* 0 all, 1 index, 2 name */
    if (sel && *sel && strcmp(sel, "all") != 0) {
        int digits = 1;
        for (const char *p = sel; *p; ++p) if (*p < '0' || *p > '9') digits = 0;
        if (digits && strlen(sel) < 6) { mode = 1; index = atoi(sel); }
        else mode = 2;
    }
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (mode == 1 && c[i].slot != index) continue;
        if (mode == 2 && !c[i].name_match) continue;
        int pos = count < VSK_MAX ? count : VSK_MAX; /* insertion by distance */
        while (pos > 0 && c[out[pos - 1]].dist > c[i].dist) --pos;
        if (pos >= VSK_MAX) continue;
        const int last = count < VSK_MAX ? count : VSK_MAX - 1;
        for (int k = last; k > pos; --k) out[k] = out[k - 1];
        out[pos] = i;
        if (count < VSK_MAX) ++count;
    }
    return count;
}

/* ---- the players' character set (live-probed on a 3-player co-op session) ----
 * WorldChrMan+0x30 u32 = slot count (5), WorldChrMan+0x38 = entry array, 0x38 bytes per entry,
 * entry+0 = PlayerIns* (0 when free). Slot 0 is the local player; co-op guests (phantoms, the same
 * PlayerIns class) take slots 1.. in join order. [PlayerIns+0x3D8] is the player's net info
 * (vtable VA 0x56C5C00): +0x36 the online name, ASCII, NUL-terminated, at most 16 characters. */
#define VSK_WCM_SLOT_COUNT 0x30
#define VSK_WCM_SLOTS 0x38
#define VSK_SLOT_STRIDE 0x38
#define VSK_SLOT_MAX 8
#define VSK_PLAYER_NETINFO 0x3D8
#define VSK_NETINFO_NAME 0x36
#define VSK_NAME_MAX 16

/* The slot count read at WorldChrMan+0x30, clamped to [0, VSK_SLOT_MAX]; 0 when implausible. */
static inline int vsk_slot_count(uint32_t raw) { return raw > 0 && raw <= 64 ? (raw > VSK_SLOT_MAX ? VSK_SLOT_MAX : (int)raw) : 0; }

/* The online name in a net-info name field (`n` bytes from +0x36) into out (NUL-terminated).
 * Returns its length, or 0 when it is not a printable, terminated name. */
static inline int vsk_net_name(const unsigned char *b, size_t n, char out[VSK_NAME_MAX + 1]) {
    out[0] = 0;
    size_t k = 0;
    while (k < n && k <= VSK_NAME_MAX && b[k]) {
        if (b[k] < 0x20 || b[k] > 0x7E) return 0;
        ++k;
    }
    if (k == 0 || k > VSK_NAME_MAX || k >= n) return 0;
    memcpy(out, b, k);
    out[k] = 0;
    return (int)k;
}

/* Case-insensitive equality of two ASCII names. */
static inline int vsk_name_eq(const char *a, const char *b) {
    if (!a || !b || !*a) return 0;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

/* ---- UTF-16LE name ---- */
/* Byte offset of the first place in buf[0..n) (2-byte steps) where the ASCII `name` appears as
 * UTF-16LE, or -1. */
static inline long vsk_utf16_find(const unsigned char *buf, size_t n, const char *name) {
    const size_t len = name ? strlen(name) : 0;
    if (!len || n < len * 2) return -1;
    for (size_t o = 0; o + len * 2 <= n; o += 2) {
        size_t k = 0;
        while (k < len && buf[o + k * 2] == (unsigned char)name[k] && buf[o + k * 2 + 1] == 0) ++k;
        if (k == len) return (long)o;
    }
    return -1;
}
/* The same for a uint16 buffer of `words` units. */
static inline int vsk_utf16_match(const uint16_t *buf, size_t words, const char *name) {
    const size_t len = name ? strlen(name) : 0;
    if (!len || words < len) return 0;
    for (size_t o = 0; o + len <= words; ++o) {
        size_t k = 0;
        while (k < len && buf[o + k] == (uint16_t)(unsigned char)name[k]) ++k;
        if (k == len) return 1;
    }
    return 0;
}

#endif
