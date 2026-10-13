/* Instant items (BB_VR_INSTANT_ITEMS=1 / instant_items=1 in out/fp.txt, default off; docs/vr/findings/44-instant-items.md).
 * Pure logic, no I/O, no game memory: unit tested in tests/test_vr_instant_items.c. The game side is
 * src/runtime_vr_items.c.
 *
 * With the setting on, a held item's gesture (findings/39) no longer starts the game's use animation:
 *   - SpEffect goods (refCategory 2: vials, pellets, papers...): the game's own PlayerIns::UseGoods
 *     (vtable slot 0x408, 0x1cf9720) runs at once with dummy poly -1, exactly what the use animation's
 *     TAE event (0x1e25b0c) and the debug menu (0x1ccf180) call: SpEffect through slot 0x3F0 with the
 *     virtual weapon's scaling, the count drop, the menu flags.  [VRI_KIND_USE]
 *   - Bullet goods that do not fly (the cures Antidote 200, Sedative 230, Lead Elixir 320: their bullets
 *     put spEffectIDForShooter on the user): a bullet request built like UseGoods builds it (0x1cc9160),
 *     launched at the player's chest through 0x1cb6680, then one item consumed.  [VRI_KIND_CURE]
 *   - Thrown goods (refCategory 1 with a flying bullet): one item consumed at the release, our own
 *     projectile flies from the hand (BulletParam rules, vri_flight_step), and where it lands the burst
 *     bullet (Bullet.HitBulletID, e.g. 101 for the Molotov) is requested at that point; a bullet without
 *     a burst (knives, pebble) is launched 0.5 m before the hit point along the flight so the game's own
 *     bullet does the hit test.  [VRI_KIND_THROW]
 *   Rope molotovs: their spawner (110 / 114) is followed to the bullet that flies (vri_resolve_flight).
 *   Quicksilver tools that fly [VRI_KIND_DIRECT] and bullets made where they act [VRI_KIND_SPOT]: see the enum.
 * Only the multiplayer bells (and a throwable used without a throw) keep the game's path (findings/44 section 6).
 *
 * The bullet request ("BR", 0xC0 bytes here; the game uses a 0xC0 stack frame for it) that 0x1cb6680
 * consumes, as ChrIns::UseGoods 0x1cc9160 fills it (static RE, findings/44 section 1):
 *   +0x00 s32 -1 (Magic id path; 0x23210e0)        +0x04 s32 goods id (EquipParamGoods; atk power: refVirtualWepId)
 *   +0x08 s32 -1 (BehaviorParam id)                 +0x0c s32 BulletParam id
 *   +0x10 s32 -1 (lock target handle)               +0x14 s32 -1 (hand)
 *   +0x18 s32 -1 (weapon slot)                      +0x1c u8 1 (goods: the emit slot keeps no handle)
 *   +0x1d u8 0                                      +0x20 s32 dummy poly id (-1: the matrix at +0x30 is used)
 *   +0x24 u8 0                                      +0x30..+0x5f launch 3x4 (rows: X[i], Y[i], Z[i], pos[i]; flies along Z)
 *   +0x60..+0x6c f32 x4 scaling (PlayerIns vfuncs 0x298 / 0x2a0 / 0x2a8 / 0x2b0 of refVirtualWepId)
 *   +0x70 s32 0 (HP)   +0x74 s32 0   +0x78 s32 0 (stamina)   +0x7c s32 -consumeHeroPoint   +0x80 s32 bulletConsumeNum */
#ifndef VR_INSTANT_ITEMS_H
#define VR_INSTANT_ITEMS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    float x, y, z;
} VriV3;

static inline VriV3 vri_v3(float x, float y, float z) {
    VriV3 v = {x, y, z};
    return v;
}
static inline VriV3 vri_add(VriV3 a, VriV3 b) { return vri_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline VriV3 vri_sub(VriV3 a, VriV3 b) { return vri_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline VriV3 vri_mul(VriV3 a, float k) { return vri_v3(a.x * k, a.y * k, a.z * k); }
static inline float vri_dot(VriV3 a, VriV3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float vri_len(VriV3 a) { return sqrtf(vri_dot(a, a)); }
static inline VriV3 vri_cross(VriV3 a, VriV3 b) {
    return vri_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline VriV3 vri_norm(VriV3 a, VriV3 fallback) {
    const float l = vri_len(a);
    return l > 1e-6f && isfinite(l) ? vri_mul(a, 1.0f / l) : fallback;
}
static inline int vri_finite(VriV3 a) { return isfinite(a.x) && isfinite(a.y) && isfinite(a.z); }

/* ---- What the game's params say (read on the game thread with the game's own lookups) ---- */
typedef struct {
    int32_t goods;          /* plain id (no 0x40000000) */
    int32_t ref_id;         /* refId: SpEffect (category 2) or Bullet (category 1) */
    uint8_t ref_category;   /* 1 bullet, 2 SpEffect */
    uint8_t is_consume;     /* isConsume */
    uint8_t bullet_consume; /* bulletConsumeNum (Quicksilver) */
    uint8_t hero_point;     /* consumeHeroPoint (Insight) */
    uint8_t use_anim;       /* goodsUseAnim (the cooldown's table key) */
    uint8_t replace_category; /* +0x41: the replace offset 0x1cc9390 adds to refId */
    int32_t virtual_wep;    /* refVirtualWepId (attack power / SpEffect scaling) */
} VriGoods;

typedef struct {
    int32_t id;
    int32_t atk_id;         /* atkId_Bullet (0: no attack) */
    int32_t hit_bullet;     /* HitBulletID (-1: none) */
    int32_t sp_shooter;     /* spEffectIDForShooter */
    float life, dist, init_v, max_v, accel_in, g_in, g_out, hit_radius;
} VriBullet;

/* EquipParamGoods / Bullet field offsets (the game's paramdefs, findings/44 section 1). */
#define VRI_GOODS_REF_ID 0x00u
#define VRI_GOODS_HERO_POINT 0x38u
#define VRI_GOODS_REF_CATEGORY 0x3Bu
#define VRI_GOODS_USE_ANIM 0x3Eu
#define VRI_GOODS_REPLACE_CATEGORY 0x41u
#define VRI_GOODS_FLAGS44 0x44u /* bit 7 isConsume */
#define VRI_GOODS_VIRTUAL_WEP 0x5Cu
#define VRI_GOODS_BULLET_CONSUME 0x60u
#define VRI_GOODS_ROW_MIN 0x6Cu
#define VRI_BULLET_ATK_ID 0x00u
#define VRI_BULLET_LIFE 0x10u
#define VRI_BULLET_DIST 0x14u
#define VRI_BULLET_G_IN 0x1Cu
#define VRI_BULLET_G_OUT 0x20u
#define VRI_BULLET_INIT_V 0x28u
#define VRI_BULLET_ACCEL_IN 0x2Cu
#define VRI_BULLET_MAX_V 0x34u
#define VRI_BULLET_HIT_RADIUS 0x44u
#define VRI_BULLET_SP_SHOOTER 0x60u
#define VRI_BULLET_HIT_BULLET 0x68u
#define VRI_BULLET_ROW_MIN 0x9Du

static inline int32_t vri_rd_s32(const uint8_t *row, uint32_t off) {
    int32_t v;
    memcpy(&v, row + off, 4);
    return v;
}
static inline float vri_rd_f32(const uint8_t *row, uint32_t off) {
    float v;
    memcpy(&v, row + off, 4);
    return v;
}
/* A goods row (EquipParamGoods, >= VRI_GOODS_ROW_MIN bytes) into VriGoods. */
static inline void vri_goods_from_row(int32_t goods, const uint8_t *row, VriGoods *g) {
    memset(g, 0, sizeof(*g));
    g->goods = goods;
    g->ref_id = vri_rd_s32(row, VRI_GOODS_REF_ID);
    g->hero_point = row[VRI_GOODS_HERO_POINT];
    g->ref_category = row[VRI_GOODS_REF_CATEGORY];
    g->use_anim = row[VRI_GOODS_USE_ANIM];
    g->replace_category = row[VRI_GOODS_REPLACE_CATEGORY];
    g->is_consume = (row[VRI_GOODS_FLAGS44] >> 7) & 1u;
    g->virtual_wep = vri_rd_s32(row, VRI_GOODS_VIRTUAL_WEP);
    g->bullet_consume = row[VRI_GOODS_BULLET_CONSUME];
}
/* A Bullet row (>= VRI_BULLET_ROW_MIN bytes) into VriBullet. */
static inline void vri_bullet_from_row(int32_t id, const uint8_t *row, VriBullet *b) {
    memset(b, 0, sizeof(*b));
    b->id = id;
    b->atk_id = vri_rd_s32(row, VRI_BULLET_ATK_ID);
    b->life = vri_rd_f32(row, VRI_BULLET_LIFE);
    b->dist = vri_rd_f32(row, VRI_BULLET_DIST);
    b->g_in = vri_rd_f32(row, VRI_BULLET_G_IN);
    b->g_out = vri_rd_f32(row, VRI_BULLET_G_OUT);
    b->init_v = vri_rd_f32(row, VRI_BULLET_INIT_V);
    b->accel_in = vri_rd_f32(row, VRI_BULLET_ACCEL_IN);
    b->max_v = vri_rd_f32(row, VRI_BULLET_MAX_V);
    b->hit_radius = vri_rd_f32(row, VRI_BULLET_HIT_RADIUS);
    b->sp_shooter = vri_rd_s32(row, VRI_BULLET_SP_SHOOTER);
    b->hit_bullet = vri_rd_s32(row, VRI_BULLET_HIT_BULLET);
}

/* ---- Which path an item takes ---- */
enum { VRI_KIND_NONE = 0, VRI_KIND_USE = 1, VRI_KIND_CURE = 2, VRI_KIND_THROW = 3, VRI_KIND_DIRECT = 4, VRI_KIND_SPOT = 5 };
/* NONE   the game's path (quick slot + square, with the animation)
 * USE    PlayerIns::UseGoods (slot 0x408, dummy -1): every SpEffect item; it pays the item, Quicksilver and Insight
 * CURE   a bullet that puts spEffectIDForShooter on the user (Antidote, Sedative, Lead Elixir): requested at the chest
 * THROW  a consumed throwable: our flight from the hand, the burst (or the bullet itself) requested where it lands
 * DIRECT a Quicksilver tool whose bullet flies (Tonitrus, Executioner's Gloves, Blacksky Eye, Accursed Brew): the
 *        tool's own bullet requested at the hand along the release; the game flies it; Quicksilver paid (0x1cf9f00)
 * SPOT   a bullet that neither flies nor cures but acts where it is made (Choir Bell's heal 360, Shaman Bone
 *        Blade 310): requested at the chest (in front of it when it attacks) */

/* The multiplayer bells (Beckoning, Small / Sinister Resonant and their "stop" twins): the online bell code
 * (src/vr_free_bells.h, the room auto-bell) times and refunds them around the game's own use. */
static inline int vri_is_mp_bell(int32_t goods) {
    return goods == 200 || goods == 201 || goods == 205 || goods == 206 || goods == 225 || goods == 226;
}

static inline int vri_flies(const VriBullet *b) { return b && b->init_v > 0.0f && b->life > 0.0f; }

/* A speed-0 spawner (the rope molotovs' 110 / 114: life 0.03 s, no speed, no cure) only makes its HitBulletID. */
static inline int vri_is_spawner(const VriBullet *b) {
    return b && b->init_v <= 0.0f && b->life > 0.0f && b->life <= 0.1f && b->hit_bullet > 0 && b->sp_shooter <= 0;
}

/* Looks a Bullet row up (the game's own lookup on the host, a table in the tests): 1 and *out when found. */
typedef int (*VriBulletFn)(void *user, int32_t id, VriBullet *out);

/* The bullet that really flies for an item whose own bullet is a spawner (1250 rope molotov: 110 -> 111, 1251:
 * 114 -> 115), at most three steps; otherwise `first` itself. *via = 1 when a spawner was followed. */
static inline VriBullet vri_resolve_flight(const VriBullet *first, VriBulletFn fn, void *user, int *via) {
    VriBullet cur = *first;
    *via = 0;
    for (int i = 0; i < 3 && vri_is_spawner(&cur) && fn; ++i) {
        VriBullet next;
        if (!fn(user, cur.hit_bullet, &next)) break;
        cur = next;
        if (vri_flies(&cur)) {
            *via = 1;
            return cur;
        }
    }
    return *first;
}

/* A spawner's flyer is launched by the game as a short back-lob (111: 10 m/s, gravity 60, about 1.7 m); thrown by
 * the arm in VR it flies like the Molotov (the bridge's table draws the same): 15 m/s, gravity 8 within 6 m then
 * 12, cap 50, life 4. The bullet id and its HitBulletID (the burst) stay. */
static inline void vri_spawner_flight(VriBullet *b) {
    b->init_v = 15.0f;
    b->dist = 6.0f;
    b->g_in = 8.0f;
    b->g_out = 12.0f;
    b->accel_in = 0.0f;
    b->max_v = 50.0f;
    b->life = 4.0f;
}

/* `flight` is the bullet that flies (vri_resolve_flight of refId + replace offset) for category 1, NULL otherwise.
 * `thrown`: the headset sent a release (THROW_ORIGIN / _DIR) for this use. *why says why not. */
static inline int vri_classify(const VriGoods *g, const VriBullet *flight, int thrown, const char **why) {
    const char *dummy;
    if (!why) why = &dummy;
    *why = "";
    if (vri_is_mp_bell(g->goods)) {
        *why = "a multiplayer bell (the online bell code times and refunds it): the game's path";
        return VRI_KIND_NONE;
    }
    if (g->ref_category == 2) return VRI_KIND_USE; /* UseGoods pays the item / Quicksilver / Insight itself */
    if (g->ref_category != 1) { *why = "neither a bullet nor a SpEffect item"; return VRI_KIND_NONE; }
    if (g->hero_point) { *why = "a bullet item that costs Insight (none in the game): the game's path"; return VRI_KIND_NONE; }
    if (!g->is_consume && !g->bullet_consume) { *why = "a free bullet item (none in the game): the game's path"; return VRI_KIND_NONE; }
    if (!flight || flight->id <= 0) { *why = "its bullet row is missing"; return VRI_KIND_NONE; }
    if (vri_flies(flight)) {
        if (!thrown) { *why = "a throwable without a throw gesture: the game's path"; return VRI_KIND_NONE; }
        return g->is_consume ? VRI_KIND_THROW : VRI_KIND_DIRECT;
    }
    if (flight->atk_id == 0 && flight->sp_shooter > 0) return VRI_KIND_CURE;
    if (vri_is_spawner(flight)) { *why = "a spawner whose child does not fly: the game's path"; return VRI_KIND_NONE; }
    if (flight->atk_id > 0) return VRI_KIND_SPOT;
    *why = "a bullet that neither flies, cures nor attacks: the game's path";
    return VRI_KIND_NONE;
}

/* ---- The throw ---- */
/* The release speed: initVellocity x clamp(hand / ref, lo, hi), capped at maxVellocity (the sandbox's
 * mapping, findings/35 3); hand <= 0 (an aimed throw, or unknown) gives initVellocity. */
static inline float vri_throw_speed(const VriBullet *b, float hand_mps, float ref, float lo, float hi) {
    float v = b->init_v;
    if (hand_mps > 0.0f && ref > 0.0f && isfinite(hand_mps)) {
        float k = hand_mps / ref;
        if (k < lo) k = lo;
        if (k > hi) k = hi;
        v *= k;
    }
    if (b->max_v > 0.0f && v > b->max_v) v = b->max_v;
    return v;
}

/* A gravity of 100+ m/s^2 is no gravity (Tiny Tonitrus' 500 over 0.2 s, findings/35 1). */
static inline float vri_gravity(float g) { return g >= 100.0f ? 0.0f : g; }

enum { VRI_FLYING = 0, VRI_HIT_WORLD = 1, VRI_HIT_CHR = 2, VRI_HIT_FLOOR = 3, VRI_EXPIRED = 4 };

typedef struct {
    VriV3 origin, pos, vel;
    float t;
    int state;   /* VRI_FLYING or the end */
    VriV3 impact; /* where it ended */
    VriV3 end_vel;
} VriFlight;

/* A world segment test: 1 and *hit when the segment a..b hits the map. */
typedef int (*VriRayFn)(void *user, VriV3 a, VriV3 b, VriV3 *hit);

static inline void vri_flight_start(VriFlight *f, VriV3 origin, VriV3 dir, float speed) {
    memset(f, 0, sizeof(*f));
    f->origin = f->pos = f->impact = origin;
    f->vel = vri_mul(vri_norm(dir, vri_v3(0, 0, 1)), speed);
    f->end_vel = f->vel;
    f->state = VRI_FLYING;
}

/* The first t in [0, 1] where the segment a + (b - a) t enters the sphere (c, r); -1 when it does not. */
static inline float vri_seg_sphere(VriV3 a, VriV3 b, VriV3 c, float r) {
    const VriV3 d = vri_sub(b, a), m = vri_sub(a, c);
    const float A = vri_dot(d, d), B = vri_dot(m, d), C = vri_dot(m, m) - r * r;
    if (C <= 0.0f) return 0.0f; /* starts inside */
    if (A < 1e-12f) return -1.0f;
    const float disc = B * B - A * C;
    if (disc < 0.0f) return -1.0f;
    const float t = (-B - sqrtf(disc)) / A;
    return t >= 0.0f && t <= 1.0f ? t : -1.0f;
}

/* Advances the flight by dt (explicit Euler in steps of at most 1/120 s, the BulletParam rules: gravity
 * gravityInRange within `dist` meters of horizontal travel and gravityOutRange beyond, accelInRange along
 * the velocity within the range up to maxVellocity, the life), testing each step's segment against the
 * world (ray, may be NULL), the characters' spheres (centres, radius sphere_r) and the floor plane
 * floor_y (a fallback; -INFINITY for none). Returns the state (VRI_FLYING while it goes on). */
static inline int vri_flight_step(VriFlight *f, const VriBullet *b, float dt, VriRayFn ray, void *user,
                                  const VriV3 *spheres, int n_spheres, float sphere_r, float floor_y) {
    if (f->state != VRI_FLYING) return f->state;
    if (!(dt > 0.0f)) return f->state;
    if (dt > 0.25f) dt = 0.25f;
    const float max_step = 1.0f / 120.0f;
    while (dt > 1e-6f && f->state == VRI_FLYING) {
        float h = dt < max_step ? dt : max_step;
        if (b->life > 0.0f && f->t + h > b->life) h = b->life - f->t;
        dt -= h;
        const float dx = f->pos.x - f->origin.x, dz = f->pos.z - f->origin.z;
        const int in_range = b->dist > 0.0f && dx * dx + dz * dz < b->dist * b->dist;
        const float g = vri_gravity(in_range ? b->g_in : b->g_out);
        if (in_range && b->accel_in > 0.0f) {
            const float sp = vri_len(f->vel);
            if (sp > 1e-6f) {
                float ns = sp + b->accel_in * h;
                if (b->max_v > 0.0f && ns > b->max_v) ns = sp > b->max_v ? sp : b->max_v;
                f->vel = vri_mul(f->vel, ns / sp);
            }
        }
        f->vel.y -= g * h;
        const VriV3 a = f->pos, next = vri_add(f->pos, vri_mul(f->vel, h));
        f->t += h;
        /* the nearest of: a character sphere, the world, the floor */
        float best = 2.0f;
        int what = VRI_FLYING;
        VriV3 at = next;
        for (int i = 0; i < n_spheres; ++i) {
            const float t = vri_seg_sphere(a, next, spheres[i], sphere_r);
            if (t >= 0.0f && t < best) { best = t; what = VRI_HIT_CHR; }
        }
        VriV3 hit;
        if (ray && ray(user, a, next, &hit) && vri_finite(hit)) {
            const VriV3 d = vri_sub(next, a);
            const float dd = vri_dot(d, d);
            float t = dd > 1e-12f ? vri_dot(vri_sub(hit, a), d) / dd : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            if (t < best) { best = t; what = VRI_HIT_WORLD; }
        }
        if (next.y <= floor_y && a.y > floor_y) {
            const float t = (a.y - floor_y) / (a.y - next.y);
            if (t < best) { best = t; what = VRI_HIT_FLOOR; }
        } else if (a.y <= floor_y) {
            best = 0.0f;
            what = VRI_HIT_FLOOR;
        }
        if (what != VRI_FLYING) {
            at = vri_add(a, vri_mul(vri_sub(next, a), best));
            f->pos = at;
            f->impact = at;
            f->end_vel = f->vel;
            f->state = what;
            return what;
        }
        f->pos = next;
        if (b->life > 0.0f && f->t >= b->life - 1e-6f) {
            f->impact = f->pos;
            f->end_vel = f->vel;
            f->state = VRI_EXPIRED;
        }
    }
    return f->state;
}

/* What is requested where a flight ended: the burst bullet at the point (backed off `back_burst_m` against
 * the travel so it is not inside the wall), or, without a burst, the flying bullet itself `back_term_m`
 * before the point along the travel (the game's own hit test then decides). Expired in the air: nothing
 * (the game's bullet expires the same way: launchConditionType 0 makes the child only on a hit).
 * Returns the BulletParam id to request (0: none), its position and its flight direction. */
static inline int32_t vri_impact_plan(const VriFlight *f, const VriBullet *flight, float back_burst_m,
                                      float back_term_m, VriV3 *pos, VriV3 *dir) {
    if (f->state == VRI_FLYING || f->state == VRI_EXPIRED) return 0;
    const VriV3 d = vri_norm(f->end_vel, vri_v3(0, 0, 1));
    *dir = d;
    if (flight->hit_bullet > 0) {
        *pos = vri_sub(f->impact, vri_mul(d, back_burst_m));
        return flight->hit_bullet;
    }
    VriV3 p = vri_sub(f->impact, vri_mul(d, back_term_m));
    /* never behind the release point */
    const VriV3 trav = vri_sub(f->impact, f->origin);
    if (vri_dot(vri_sub(p, f->origin), trav) < 0.0f) p = f->origin;
    *pos = p;
    return flight->id;
}

/* The launch 3x4 (row i = X[i], Y[i], Z[i], pos[i]) at `pos` flying along `dir` (Y up; a vertical dir
 * gets X = world X). */
static inline void vri_launch_rows(VriV3 pos, VriV3 dir, float rows[3][4]) {
    const VriV3 z = vri_norm(dir, vri_v3(0, 0, 1));
    VriV3 x = vri_cross(vri_v3(0, 1, 0), z);
    x = vri_norm(x, vri_v3(1, 0, 0));
    const VriV3 y = vri_cross(z, x);
    const float c[4][3] = {{x.x, x.y, x.z}, {y.x, y.y, y.z}, {z.x, z.y, z.z}, {pos.x, pos.y, pos.z}};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 4; ++k) rows[i][k] = c[k][i];
}

/* ---- The bullet request (see the header comment) ---- */
#define VRI_BR_SIZE 0xC0u
static inline void vri_br_s32(uint8_t *br, uint32_t off, int32_t v) { memcpy(br + off, &v, 4); }
static inline void vri_build_request(uint8_t br[VRI_BR_SIZE], const VriGoods *g, int32_t bullet_id,
                                     const float rows[3][4], const float scale[4]) {
    memset(br, 0, VRI_BR_SIZE);
    for (uint32_t off = 0x00; off < 0x20; off += 4) vri_br_s32(br, off, -1);
    vri_br_s32(br, 0x04, g->goods);
    vri_br_s32(br, 0x0c, bullet_id);
    br[0x1c] = 1;
    br[0x1d] = 0;
    vri_br_s32(br, 0x20, -1);
    br[0x24] = 0;
    memcpy(br + 0x30, rows, 48);
    memcpy(br + 0x60, scale, 16);
    vri_br_s32(br, 0x70, 0);
    vri_br_s32(br, 0x74, 0);
    vri_br_s32(br, 0x78, 0);
    vri_br_s32(br, 0x7c, -(int32_t)g->hero_point);
    vri_br_s32(br, 0x80, (int32_t)g->bullet_consume);
}

/* ---- Balance: one instant use per cooldown, as long as the vanilla animation would have taken ----
 * The clip lengths are NOT read from the game's TAE data (not on disk here): estimates by goodsUseAnim,
 * findings/44 section 4 [S]; BB_VR_INSTANT_COOLDOWN_S overrides all of them. */
static inline float vri_cooldown_s(uint8_t use_anim, int kind) {
    switch (use_anim) {
    case 26: /* Blood Vial: the injection */
    case 28: /* Iosefka's vial */
        return 1.3f;
    case 2: case 3: case 13: case 34: /* throws (knife, molotov / urn / pebble, ..., delayed molotov) */
        return 1.0f;
    case 1: /* weapon papers: the rub */
        return 1.6f;
    case 10: /* Sedative / Lead Elixir: a drink */
        return 1.3f;
    default:
        return kind == VRI_KIND_THROW ? 1.0f : 1.2f;
    }
}

typedef struct {
    double until;
} VriCooldown;
static inline int vri_cooldown_ready(const VriCooldown *c, double now) { return now >= c->until; }
static inline void vri_cooldown_arm(VriCooldown *c, double now, float s) {
    c->until = now + (s > 0.0f && isfinite(s) ? (double)s : 0.0);
}

/* ---- The setting: "instant_items=<0|1>" in the control file; 1 on, 0 off, -1 the key is absent ---- */
static inline int vri_parse_on(const char *text, const char *key) {
    /* out/fp.txt is usually ONE line of space-separated key=value tokens, so the key may start any token,
     * not only a line; '#' comments out the rest of its line. The last occurrence wins. */
    const size_t kl = strlen(key);
    int found = -1;
    for (const char *p = text; p && *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (*p == '#') {
            while (*p && *p != '\n') ++p;
            continue;
        }
        if (strncmp(p, key, kl) == 0) {
            const char *v = p + kl;
            while (*v == ' ' || *v == '\t') ++v;
            if (*v == '=') {
                ++v;
                while (*v == ' ' || *v == '\t') ++v;
                if (*v == '1' || *v == 'y' || *v == 'Y' || strncmp(v, "on", 2) == 0 || strncmp(v, "true", 4) == 0) found = 1;
                else if (*v == '0' || *v == 'n' || *v == 'N' || strncmp(v, "off", 3) == 0 || strncmp(v, "false", 5) == 0) found = 0;
            }
        }
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
    }
    return found;
}

/* Online (BB_ONLINE=1 or BB_ONLINE_ROLE set): the feature is single player only. */
static inline int vri_online(const char *bb_online, const char *role) {
    return (bb_online && bb_online[0] == '1') || (role && role[0]);
}

#endif
