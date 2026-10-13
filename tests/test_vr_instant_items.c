/* Unit tests for src/vr_instant_items.h (instant items, docs/vr/findings/44-instant-items.md).
 * Build: ninja -C out/gpu vr-instant-items-test. */
#include "vr_instant_items.h"
#include <stdio.h>
static int failures, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)
#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (float)(eps))

/* The dump's rows (findings/35 table, out of Bullet.param via scripts): */
static VriBullet bullet(int32_t id, int32_t atk, int32_t hit, int32_t sp, float life, float dist, float v0, float vmax,
                        float acc, float gin, float gout) {
    VriBullet b;
    memset(&b, 0, sizeof(b));
    b.id = id; b.atk_id = atk; b.hit_bullet = hit; b.sp_shooter = sp;
    b.life = life; b.dist = dist; b.init_v = v0; b.max_v = vmax; b.accel_in = acc; b.g_in = gin; b.g_out = gout;
    b.hit_radius = 0.1f;
    return b;
}
static VriGoods goods(int32_t id, int32_t ref, uint8_t cat, uint8_t consume, uint8_t qs, uint8_t ins, uint8_t anim) {
    VriGoods g;
    memset(&g, 0, sizeof(g));
    g.goods = id; g.ref_id = ref; g.ref_category = cat; g.is_consume = consume; g.bullet_consume = qs;
    g.hero_point = ins; g.use_anim = anim; g.virtual_wep = 12000;
    return g;
}

/* The dump's rows (EquipParamGoods / Bullet of CUSA03173, generated from the params) for the per-item table. */
static const struct { int32_t id, ref; uint8_t cat, consume, qs, ins, anim; } kGoods[] = {
    {1000, 3010, 2, 1, 0, 0, 26}, {703, 3020, 2, 1, 0, 0, 28},  {706, 3040, 2, 1, 0, 0, 28},  {1100, 200, 1, 1, 0, 0, 0},
    {1101, 230, 1, 1, 0, 0, 10},  {1110, 3240, 2, 1, 0, 0, 0},  {1120, 2014, 2, 1, 0, 0, 10}, {2030, 320, 1, 1, 0, 0, 10},
    {1500, 2160, 2, 1, 0, 0, 9},  {2060, 1120, 2, 0, 4, 0, 20}, {200, 9000, 2, 0, 0, 1, 14},  {201, -1, 0, 0, 0, 1, 14},
    {205, 9005, 2, 0, 0, 0, 14},  {206, -1, 0, 0, 0, 0, 14},    {225, 9025, 2, 0, 0, 0, 14},  {226, -1, 0, 0, 0, 0, 14},
    {2050, 360, 1, 0, 7, 0, 18},  {100, 2100, 2, 0, 0, 0, 16},  {1400, 2101, 2, 1, 0, 0, 16}, {2090, 310, 1, 1, 0, 0, 25},
    {1300, 2200, 2, 1, 0, 0, 1},  {1320, 2220, 2, 1, 0, 0, 1},  {1310, 2210, 2, 0, 3, 0, 1},  {1330, 2230, 2, 1, 0, 0, 5},
    {1200, 100, 1, 1, 0, 0, 3},   {1201, 105, 1, 1, 0, 0, 34},  {1210, 121, 1, 1, 0, 0, 2},   {1220, 150, 1, 1, 0, 0, 3},
    {1230, 170, 1, 1, 0, 0, 3},   {1240, 120, 1, 1, 0, 0, 2},   {1250, 110, 1, 1, 0, 0, 13},  {1251, 114, 1, 1, 0, 0, 13},
    {1260, 160, 1, 1, 0, 0, 3},   {1270, 180, 1, 1, 0, 0, 3},   {1410, 300, 1, 1, 0, 0, 7},   {2070, 410, 1, 0, 6, 0, 23},
    {2080, 350, 1, 0, 3, 0, 24},  {2120, 460, 1, 0, 1, 0, 30},  {2130, 470, 1, 0, 2, 0, 31},
};
/* id, atk, HitBulletID, spEffectIDForShooter, life, dist, initVellocity, maxVellocity, accelInRange, gravity in, out, hitRadius */
static const VriBullet kBullets[] = {
    {100, 16, 101, 0, 4.0f, 6.0f, 15.0f, 50.0f, 0.0f, 8.0f, 12.0f, 0.1f},
    {101, 1052, 75, 0, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.4f},
    {105, 1061, 106, 0, 4.0f, 2.0f, 15.0f, 15.0f, 0.0f, 40.0f, 20.0f, 0.05f},
    {106, 16, 107, 0, 0.03f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f, 0.05f},
    {110, 16, 111, 0, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.1f},
    {111, 16, 112, 0, 4.0f, 0.0f, 10.0f, 20.0f, 0.0f, 0.0f, 60.0f, 0.1f},
    {112, 1052, 75, 0, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.4f},
    {114, 16, 115, 0, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.1f},
    {115, 1061, 116, 0, 4.0f, 0.0f, 10.0f, 20.0f, 0.0f, 0.0f, 60.0f, 0.05f},
    {116, 16, 117, 0, 0.03f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f, 0.05f},
    {120, 1050, -1, 0, 2.0f, 8.0f, 20.0f, 50.0f, 10.0f, 0.0f, 9.8f, 0.12f},
    {121, 1051, -1, 0, 2.0f, 8.0f, 20.0f, 50.0f, 10.0f, 0.0f, 9.8f, 0.12f},
    {150, 17, 151, 0, 4.0f, 6.0f, 15.0f, 50.0f, 0.0f, 8.0f, 12.0f, 0.1f},
    {160, 0, 161, 0, 4.0f, 0.0f, 15.0f, 50.0f, 0.0f, 0.0f, 12.0f, 0.08f},
    {170, 1000, -1, 0, 4.0f, 10.0f, 20.0f, 20.0f, 0.0f, 3.0f, 16.0f, 0.1f},
    {180, 0, 181, 0, 4.0f, 6.0f, 15.0f, 50.0f, 0.0f, 8.0f, 12.0f, 0.1f},
    {200, 0, 201, 3200, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.01f},
    {230, 0, 231, 3230, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.01f},
    {300, 0, 301, 0, 2.04f, 20.0f, 2.0f, 20.0f, 0.0f, 9.8f, 9.8f, 0.01f},
    {310, 1002, -1, 0, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f},
    {320, 0, 321, 1130, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.01f},
    {350, 10090, -1, 0, 3.0f, 20.0f, 4.0f, 4.0f, 40.0f, 0.0f, 0.0f, 0.2f},
    {360, 10100, -1, 0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.8f},
    {410, 24000, 411, 0, 0.2f, 15.0f, 33.0f, 33.0f, 0.0f, 500.0f, 60.0f, 0.1f},
    {460, 15040, 461, 0, 0.54f, 16.0f, 30.0f, 30.0f, 0.0f, 0.0f, 0.0f, 0.1f},
    {470, 15050, 471, 0, 0.5f, 7.6f, 15.0f, 20.0f, 10.0f, 3.0f, 7.0f, 0.15f},
};
static int table_bullet(void *user, int32_t id, VriBullet *out) {
    (void)user;
    for (size_t i = 0; i < sizeof(kBullets) / sizeof(kBullets[0]); ++i)
        if (kBullets[i].id == id) { *out = kBullets[i]; return 1; }
    return 0;
}
static VriGoods goods_of(int32_t id) {
    VriGoods g;
    memset(&g, 0, sizeof(g));
    for (size_t i = 0; i < sizeof(kGoods) / sizeof(kGoods[0]); ++i)
        if (kGoods[i].id == id) {
            g.goods = id; g.ref_id = kGoods[i].ref; g.ref_category = kGoods[i].cat; g.is_consume = kGoods[i].consume;
            g.bullet_consume = kGoods[i].qs; g.hero_point = kGoods[i].ins; g.use_anim = kGoods[i].anim; g.virtual_wep = 16000;
        }
    return g;
}
/* The table: goods, sent with a throw release (the held-item kind Throw), expected path, via a spawner, the bullet
 * requested (THROW: where it lands; CURE / SPOT / DIRECT: at once). */
typedef struct { int32_t goods; int thrown, kind, via; int32_t spawn; } ItemCase;
static const ItemCase kItems[] = {
    /* SpEffect items: UseGoods */
    {1000, 0, VRI_KIND_USE, 0, 0}, {703, 0, VRI_KIND_USE, 0, 0},  {706, 0, VRI_KIND_USE, 0, 0},  {1110, 0, VRI_KIND_USE, 0, 0},
    {1120, 0, VRI_KIND_USE, 0, 0}, {1500, 0, VRI_KIND_USE, 0, 0}, {2060, 0, VRI_KIND_USE, 0, 0}, {100, 0, VRI_KIND_USE, 0, 0},
    {1400, 0, VRI_KIND_USE, 0, 0}, {1300, 0, VRI_KIND_USE, 0, 0}, {1320, 0, VRI_KIND_USE, 0, 0}, {1310, 0, VRI_KIND_USE, 0, 0},
    {1330, 0, VRI_KIND_USE, 0, 0},
    /* cures at the chest */
    {1100, 0, VRI_KIND_CURE, 0, 200}, {1101, 0, VRI_KIND_CURE, 0, 230}, {2030, 0, VRI_KIND_CURE, 0, 320},
    /* multiplayer bells: the game's path */
    {200, 0, VRI_KIND_NONE, 0, 0}, {201, 0, VRI_KIND_NONE, 0, 0}, {205, 0, VRI_KIND_NONE, 0, 0}, {206, 0, VRI_KIND_NONE, 0, 0},
    {225, 0, VRI_KIND_NONE, 0, 0}, {226, 0, VRI_KIND_NONE, 0, 0},
    /* made where they act: Choir Bell (heal area, Quicksilver 7), Shaman Bone Blade */
    {2050, 0, VRI_KIND_SPOT, 0, 360}, {2090, 0, VRI_KIND_SPOT, 0, 310},
    /* thrown: our flight, then the burst / the bullet itself */
    {1200, 1, VRI_KIND_THROW, 0, 101}, {1201, 1, VRI_KIND_THROW, 0, 106}, {1210, 1, VRI_KIND_THROW, 0, 121},
    {1220, 1, VRI_KIND_THROW, 0, 151}, {1230, 1, VRI_KIND_THROW, 0, 170}, {1240, 1, VRI_KIND_THROW, 0, 120},
    {1250, 1, VRI_KIND_THROW, 1, 112}, {1251, 1, VRI_KIND_THROW, 1, 116}, {1260, 1, VRI_KIND_THROW, 0, 161},
    {1270, 1, VRI_KIND_THROW, 0, 181}, {1410, 1, VRI_KIND_THROW, 0, 301},
    /* Quicksilver tools that fly: their own bullet from the hand */
    {2070, 1, VRI_KIND_DIRECT, 0, 410}, {2080, 1, VRI_KIND_DIRECT, 0, 350}, {2120, 1, VRI_KIND_DIRECT, 0, 460},
    {2130, 1, VRI_KIND_DIRECT, 0, 470},
};

/* A wall: the plane x = wall_x. */
static int wall_ray(void *user, VriV3 a, VriV3 b, VriV3 *hit) {
    const float wx = *(const float *)user;
    if ((a.x - wx) * (b.x - wx) > 0.0f || a.x == b.x) return 0;
    const float t = (wx - a.x) / (b.x - a.x);
    *hit = vri_add(a, vri_mul(vri_sub(b, a), t));
    return 1;
}
static int calls;
static int never_ray(void *user, VriV3 a, VriV3 b, VriV3 *hit) {
    (void)user; (void)a; (void)b; (void)hit;
    ++calls;
    return 0;
}

static VriFlight fly(const VriBullet *b, VriV3 o, VriV3 d, float speed, VriRayFn ray, void *u, const VriV3 *sp, int n,
                     float floor_y) {
    VriFlight f;
    vri_flight_start(&f, o, d, speed);
    for (int i = 0; i < 2000 && f.state == VRI_FLYING; ++i) vri_flight_step(&f, b, 1.0f / 90.0f, ray, u, sp, n, 0.5f, floor_y);
    return f;
}

int main(void) {
    const VriBullet molotov = bullet(100, 16, 101, 0, 4.0f, 6.0f, 15.0f, 50.0f, 0.0f, 8.0f, 12.0f);
    const VriBullet pebble = bullet(170, 1000, -1, 0, 4.0f, 10.0f, 20.0f, 20.0f, 0.0f, 3.0f, 16.0f);
    const VriBullet knife = bullet(120, 1050, -1, 0, 2.0f, 8.0f, 20.0f, 50.0f, 10.0f, 0.0f, 9.8f);
    const VriBullet antidote = bullet(200, 0, 201, 3200, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const VriBullet rope = bullet(110, 16, 111, 0, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const VriBullet tonitrus = bullet(410, 1, 411, 0, 0.2f, 15.0f, 33.0f, 33.0f, 0.0f, 500.0f, 60.0f);

    /* ---- classification ---- */
    const char *why = NULL;
    VriGoods vial = goods(1000, 3010, 2, 1, 0, 0, 26);
    CHECK(vri_classify(&vial, NULL, 0, &why) == VRI_KIND_USE, "a vial is used through UseGoods");
    VriGoods shell = goods(1310, 2210, 2, 0, 3, 0, 1);
    CHECK(vri_classify(&shell, NULL, 0, &why) == VRI_KIND_USE, "a Quicksilver SpEffect tool: UseGoods pays it");
    VriGoods reusable = goods(1500, 2160, 2, 0, 0, 0, 0);
    CHECK(vri_classify(&reusable, NULL, 0, &why) == VRI_KIND_USE, "a reusable SpEffect item: UseGoods");
    VriGoods mark = goods(100, 2100, 2, 0, 0, 0, 0);
    CHECK(vri_classify(&mark, NULL, 0, &why) == VRI_KIND_USE, "Hunter's Mark: UseGoods (its warp SpEffect)");
    VriGoods bell = goods(205, 9000, 2, 0, 0, 1, 0);
    CHECK(vri_classify(&bell, NULL, 0, &why) == VRI_KIND_NONE && why[0], "a multiplayer bell keeps the game's path (%s)", why);
    VriGoods insight = goods(1700, 3000, 2, 1, 0, 1, 0);
    CHECK(vri_classify(&insight, NULL, 0, &why) == VRI_KIND_USE, "an Insight cost: UseGoods pays it");
    VriGoods g_anti = goods(1100, 200, 1, 1, 0, 0, 0);
    CHECK(vri_classify(&g_anti, &antidote, 0, &why) == VRI_KIND_CURE, "Antidote: its bullet cures the shooter");
    VriGoods g_molo = goods(1200, 100, 1, 1, 0, 0, 3);
    CHECK(vri_classify(&g_molo, &molotov, 1, &why) == VRI_KIND_THROW, "a thrown molotov");
    CHECK(vri_classify(&g_molo, &molotov, 0, &why) == VRI_KIND_NONE, "a molotov with no throw: the game's path");
    CHECK(vri_classify(&g_molo, NULL, 1, &why) == VRI_KIND_NONE, "no bullet row: the game's path");
    VriGoods g_rope = goods(1250, 110, 1, 1, 0, 0, 3);
    CHECK(vri_classify(&g_rope, &rope, 1, &why) == VRI_KIND_NONE, "an unresolved spawner: the game's path (%s)", why);
    VriGoods g_ton = goods(2070, 410, 1, 0, 6, 0, 0);
    CHECK(vri_classify(&g_ton, &tonitrus, 1, &why) == VRI_KIND_DIRECT, "a flying Quicksilver tool: its own bullet from the hand");
    CHECK(vri_classify(&g_ton, &tonitrus, 0, &why) == VRI_KIND_NONE, "... only with a release");

    /* ---- the per-item table (findings/44 section 6): every goods the held-item code takes, the dump's rows ---- */
    {
        int done = 0;
        for (size_t i = 0; i < sizeof(kItems) / sizeof(kItems[0]); ++i) {
            const ItemCase *c = &kItems[i];
            const VriGoods g = goods_of(c->goods);
            CHECK(g.goods == c->goods, "goods %d in the table", (int)c->goods);
            VriBullet own, fl;
            int via = 0;
            const VriBullet *bp = NULL;
            if (g.ref_category == 1 && table_bullet(NULL, g.ref_id, &own)) {
                fl = vri_resolve_flight(&own, table_bullet, NULL, &via);
                if (via) vri_spawner_flight(&fl);
                bp = &fl;
            }
            const int kind = vri_classify(&g, bp, c->thrown, &why);
            CHECK(kind == c->kind, "goods %d: kind %d, want %d (%s)", (int)c->goods, kind, c->kind, why);
            CHECK(via == c->via, "goods %d: via a spawner %d, want %d", (int)c->goods, via, c->via);
            if (kind == VRI_KIND_THROW) {
                /* thrown at a wall 5 m away: what is requested where it lands */
                float wx = 5.0f;
                const VriFlight f = fly(bp, vri_v3(0, 1.5f, 0), vri_v3(1, 0.2f, 0), bp->init_v, wall_ray, &wx, NULL, 0, 0.0f);
                VriV3 pos, dir;
                const int32_t id = vri_impact_plan(&f, bp, 0.05f, 0.5f, &pos, &dir);
                CHECK(id == c->spawn, "goods %d: requests bullet %d where it lands, want %d (state %d)", (int)c->goods,
                      (int)id, (int)c->spawn, f.state);
            } else if (kind == VRI_KIND_CURE || kind == VRI_KIND_SPOT || kind == VRI_KIND_DIRECT) {
                CHECK(bp && bp->id == c->spawn, "goods %d: requests bullet %d, want %d", (int)c->goods, bp ? (int)bp->id : 0,
                      (int)c->spawn);
            }
            ++done;
        }
        CHECK(done == 39, "39 items mapped (%d)", done);
    }

    /* ---- rows ---- */
    {
        uint8_t row[0x70];
        memset(row, 0, sizeof(row));
        const int32_t ref = 100, vw = 12000;
        memcpy(row + 0x00, &ref, 4);
        memcpy(row + 0x5c, &vw, 4);
        row[0x3b] = 1; row[0x3e] = 3; row[0x41] = 2; row[0x44] = 0x80; row[0x60] = 0; row[0x38] = 0;
        VriGoods g;
        vri_goods_from_row(1200, row, &g);
        CHECK(g.goods == 1200 && g.ref_id == 100 && g.ref_category == 1 && g.use_anim == 3 && g.replace_category == 2 &&
                  g.is_consume == 1 && g.virtual_wep == 12000 && g.bullet_consume == 0,
              "goods row fields");
        row[0x44] = 0x7f;
        vri_goods_from_row(1200, row, &g);
        CHECK(g.is_consume == 0, "isConsume is bit 7 of +0x44");
        uint8_t br[0xA0];
        memset(br, 0, sizeof(br));
        const float life = 4.0f, dist = 6.0f, v0 = 15.0f, gi = 8.0f, go = 12.0f, vmax = 50.0f, rad = 0.1f;
        const int32_t atk = 16, hit = 101, sp = 0;
        memcpy(br + 0x00, &atk, 4); memcpy(br + 0x10, &life, 4); memcpy(br + 0x14, &dist, 4); memcpy(br + 0x1c, &gi, 4);
        memcpy(br + 0x20, &go, 4); memcpy(br + 0x28, &v0, 4); memcpy(br + 0x34, &vmax, 4); memcpy(br + 0x44, &rad, 4);
        memcpy(br + 0x60, &sp, 4); memcpy(br + 0x68, &hit, 4);
        VriBullet b;
        vri_bullet_from_row(100, br, &b);
        CHECK(b.id == 100 && b.atk_id == 16 && b.hit_bullet == 101 && b.life == 4.0f && b.dist == 6.0f && b.init_v == 15.0f &&
                  b.g_in == 8.0f && b.g_out == 12.0f && b.max_v == 50.0f && NEAR(b.hit_radius, 0.1f, 1e-6),
              "bullet row fields");
    }

    /* ---- speed ---- */
    CHECK(NEAR(vri_throw_speed(&molotov, 7.0f, 7.0f, 0.3f, 1.5f), 15.0f, 1e-4), "a 7 m/s hand throws at initVellocity");
    CHECK(NEAR(vri_throw_speed(&molotov, 0.0f, 7.0f, 0.3f, 1.5f), 15.0f, 1e-4), "no hand speed (aimed): initVellocity");
    CHECK(NEAR(vri_throw_speed(&molotov, 1.0f, 7.0f, 0.3f, 1.5f), 4.5f, 1e-4), "a soft throw: 0.3 x");
    CHECK(NEAR(vri_throw_speed(&molotov, 30.0f, 7.0f, 0.3f, 1.5f), 22.5f, 1e-4), "a hard throw: 1.5 x");
    CHECK(NEAR(vri_throw_speed(&pebble, 30.0f, 7.0f, 0.3f, 1.5f), 20.0f, 1e-4), "capped at maxVellocity");
    CHECK(vri_gravity(500.0f) == 0.0f && vri_gravity(9.8f) == 9.8f, "a 100+ gravity is none");

    /* ---- flight: the findings/35 reference throws (flat at 7 m/s hand speed from 1.3 m) ---- */
    {
        VriFlight f = fly(&molotov, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 15.0f, NULL, NULL, NULL, 0, 0.0f);
        CHECK(f.state == VRI_HIT_FLOOR && NEAR(f.impact.x, 8.38f, 0.15f) && NEAR(f.impact.y, 0.0f, 1e-3),
              "molotov lands 8.4 m away (%.2f, state %d)", (double)f.impact.x, f.state);
        f = fly(&pebble, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 20.0f, NULL, NULL, NULL, 0, 0.0f);
        CHECK(f.state == VRI_HIT_FLOOR && NEAR(f.impact.x, 15.18f, 0.2f), "pebble lands 15.2 m away (%.2f)", (double)f.impact.x);
        f = fly(&knife, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 20.0f, NULL, NULL, NULL, 0, 0.0f);
        CHECK(f.state == VRI_HIT_FLOOR && f.impact.x > 18.0f && f.impact.x < 22.5f, "knife flat for 8 m, then falls (%.2f)",
              (double)f.impact.x);
        /* a knife thrown straight up expires at its life (2 s) */
        f = fly(&knife, vri_v3(0, 1.3f, 0), vri_v3(0, 1, 0), 20.0f, NULL, NULL, NULL, 0, -1000.0f);
        CHECK(f.state == VRI_EXPIRED && NEAR(f.t, 2.0f, 1e-3), "expired at its life (t %.3f state %d)", (double)f.t, f.state);
        VriV3 pos, dir;
        CHECK(vri_impact_plan(&f, &knife, 0.05f, 0.5f, &pos, &dir) == 0, "an expired bullet requests nothing");
        /* Tiny Tonitrus' 500 m/s^2 is no gravity: straight for 0.2 s */
        f = fly(&tonitrus, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 33.0f, NULL, NULL, NULL, 0, 0.0f);
        CHECK(f.state == VRI_EXPIRED && NEAR(f.impact.y, 1.3f, 1e-3) && NEAR(f.impact.x, 6.6f, 0.05f),
              "tonitrus flies straight 6.6 m (%.2f %.2f)", (double)f.impact.x, (double)f.impact.y);
    }
    /* the world first, then characters, then the floor */
    {
        float wx = 5.0f;
        VriFlight f = fly(&molotov, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 15.0f, wall_ray, &wx, NULL, 0, 0.0f);
        CHECK(f.state == VRI_HIT_WORLD && NEAR(f.impact.x, 5.0f, 1e-3) && f.impact.y > 0.5f, "a wall at 5 m (%.3f)",
              (double)f.impact.x);
        VriV3 pos, dir;
        const int32_t id = vri_impact_plan(&f, &molotov, 0.05f, 0.5f, &pos, &dir);
        CHECK(id == 101 && NEAR(pos.x, 4.95f, 0.01f) && dir.x > 0.9f, "the burst 101 just before the wall (%d at %.3f)", id,
              (double)pos.x);
        const VriV3 enemy = vri_v3(4.0f, 1.2f, 0.0f);
        f = fly(&molotov, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 15.0f, wall_ray, &wx, &enemy, 1, 0.0f);
        CHECK(f.state == VRI_HIT_CHR && NEAR(f.impact.x, 3.5f, 0.05f), "an enemy sphere before the wall (%.3f)", (double)f.impact.x);
        f = fly(&knife, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 20.0f, wall_ray, &wx, NULL, 0, 0.0f);
        CHECK(vri_impact_plan(&f, &knife, 0.05f, 0.5f, &pos, &dir) == 120 && NEAR(pos.x, 4.5f, 0.02f),
              "no burst: the knife's own bullet 0.5 m before the hit (%.3f)", (double)pos.x);
        /* a hit right at the release: the terminal bullet is not put behind the hand */
        wx = 0.2f;
        f = fly(&knife, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 20.0f, wall_ray, &wx, NULL, 0, 0.0f);
        CHECK(vri_impact_plan(&f, &knife, 0.05f, 0.5f, &pos, &dir) == 120 && NEAR(pos.x, 0.0f, 1e-4),
              "a hit at the hand launches from the hand (%.3f)", (double)pos.x);
        calls = 0;
        f = fly(&molotov, vri_v3(0, 1.3f, 0), vri_v3(1, 0, 0), 15.0f, never_ray, NULL, NULL, 0, 0.0f);
        CHECK(f.state == VRI_HIT_FLOOR && calls > 10, "a ray that never hits: the floor fallback (%d calls)", calls);
        /* already below the floor: ends at once */
        f = fly(&molotov, vri_v3(0, -1.0f, 0), vri_v3(1, 0, 0), 15.0f, NULL, NULL, NULL, 0, 0.0f);
        CHECK(f.state == VRI_HIT_FLOOR && NEAR(f.impact.x, 0.0f, 1e-3), "starts below the floor");
        /* big dt is cut */
        VriFlight g;
        vri_flight_start(&g, vri_v3(0, 100, 0), vri_v3(1, 0, 0), 15.0f);
        vri_flight_step(&g, &molotov, 10.0f, NULL, NULL, NULL, 0, 0.5f, -1000.0f);
        CHECK(NEAR(g.t, 0.25f, 1e-4), "a hitch advances at most 0.25 s (%.3f)", (double)g.t);
        vri_flight_step(&g, &molotov, NAN, NULL, NULL, NULL, 0, 0.5f, -1000.0f);
        CHECK(NEAR(g.t, 0.25f, 1e-4), "a NaN dt does nothing");
    }
    CHECK(vri_seg_sphere(vri_v3(0, 0, 0), vri_v3(2, 0, 0), vri_v3(1, 0, 0), 0.5f) > 0.24f &&
              vri_seg_sphere(vri_v3(0, 0, 0), vri_v3(2, 0, 0), vri_v3(1, 0, 0), 0.5f) < 0.26f,
          "segment enters the sphere at t 0.25");
    CHECK(vri_seg_sphere(vri_v3(0, 0, 0), vri_v3(2, 0, 0), vri_v3(1, 2, 0), 0.5f) < 0.0f, "a miss");
    CHECK(vri_seg_sphere(vri_v3(1, 0, 0), vri_v3(2, 0, 0), vri_v3(1, 0, 0), 0.5f) == 0.0f, "starting inside");

    /* ---- launch matrix: rows (X[i], Y[i], Z[i], pos[i]), flies along Z ---- */
    {
        float r[3][4];
        vri_launch_rows(vri_v3(1, 2, 3), vri_v3(0, 0, 2), r);
        CHECK(r[0][3] == 1.0f && r[1][3] == 2.0f && r[2][3] == 3.0f, "position in column 4");
        CHECK(NEAR(r[2][2], 1.0f, 1e-6) && NEAR(r[0][2], 0.0f, 1e-6) && NEAR(r[1][2], 0.0f, 1e-6), "Z = the direction");
        const VriV3 X = vri_v3(r[0][0], r[1][0], r[2][0]), Y = vri_v3(r[0][1], r[1][1], r[2][1]),
                    Z = vri_v3(r[0][2], r[1][2], r[2][2]);
        CHECK(NEAR(vri_dot(X, Y), 0, 1e-5) && NEAR(vri_dot(X, Z), 0, 1e-5) && NEAR(vri_dot(Y, Z), 0, 1e-5) &&
                  NEAR(vri_len(X), 1, 1e-5) && NEAR(vri_len(Y), 1, 1e-5) && Y.y > 0.99f,
              "orthonormal, Y up");
        const VriV3 c = vri_cross(X, Y);
        CHECK(NEAR(c.x, Z.x, 1e-5) && NEAR(c.y, Z.y, 1e-5) && NEAR(c.z, Z.z, 1e-5), "right-handed X x Y = Z");
        vri_launch_rows(vri_v3(0, 0, 0), vri_v3(0, -1, 0), r);
        const VriV3 X2 = vri_v3(r[0][0], r[1][0], r[2][0]);
        CHECK(NEAR(r[1][2], -1.0f, 1e-6) && NEAR(vri_len(X2), 1, 1e-5) && isfinite(r[0][1]), "straight down is valid");
    }

    /* ---- the bullet request: UseGoods' layout ---- */
    {
        uint8_t br[VRI_BR_SIZE];
        float rows[3][4] = {{1, 0, 0, 5}, {0, 1, 0, 6}, {0, 0, 1, 7}};
        const float scale[4] = {1.0f, 1.1f, 1.2f, 1.3f};
        VriGoods g = goods(1200, 100, 1, 1, 0, 0, 3);
        vri_build_request(br, &g, 101, (const float (*)[4])rows, scale);
        CHECK(vri_rd_s32(br, 0x00) == -1 && vri_rd_s32(br, 0x04) == 1200 && vri_rd_s32(br, 0x08) == -1 &&
                  vri_rd_s32(br, 0x0c) == 101 && vri_rd_s32(br, 0x10) == -1 && vri_rd_s32(br, 0x14) == -1 &&
                  vri_rd_s32(br, 0x18) == -1,
              "ids: magic -1, goods, behavior -1, bullet, target/hand/slot -1");
        CHECK(br[0x1c] == 1 && br[0x1d] == 0 && vri_rd_s32(br, 0x20) == -1 && br[0x24] == 0, "goods flag, dummy -1");
        CHECK(vri_rd_f32(br, 0x30) == 1.0f && vri_rd_f32(br, 0x3c) == 5.0f && vri_rd_f32(br, 0x4c) == 6.0f &&
                  vri_rd_f32(br, 0x5c) == 7.0f,
              "the matrix rows at +0x30, positions at +0x3c/+0x4c/+0x5c (what 0x1dedc20 reads)");
        CHECK(vri_rd_f32(br, 0x60) == 1.0f && vri_rd_f32(br, 0x68) == 1.2f && vri_rd_f32(br, 0x6c) == 1.3f,
              "scaling at +0x60..+0x6c (+0x68/+0x6c go to the request's +0x24/+0x28)");
        CHECK(vri_rd_s32(br, 0x70) == 0 && vri_rd_s32(br, 0x78) == 0 && vri_rd_s32(br, 0x7c) == 0 && vri_rd_s32(br, 0x80) == 0,
              "no HP, stamina, Insight or Quicksilver change");
        CHECK(br[0x25] == 0 && br[0x2f] == 0 && br[0xbf] == 0, "padding zeroed");
    }

    /* ---- cooldown, setting, online ---- */
    CHECK(vri_cooldown_s(26, VRI_KIND_USE) == 1.3f && vri_cooldown_s(3, VRI_KIND_THROW) == 1.0f &&
              vri_cooldown_s(99, VRI_KIND_THROW) == 1.0f && vri_cooldown_s(99, VRI_KIND_USE) == 1.2f,
          "cooldown table");
    {
        VriCooldown c = {0};
        CHECK(vri_cooldown_ready(&c, 1.0), "ready at start");
        vri_cooldown_arm(&c, 10.0, 1.3f);
        CHECK(!vri_cooldown_ready(&c, 11.0) && vri_cooldown_ready(&c, 11.31), "blocks for 1.3 s");
        vri_cooldown_arm(&c, 20.0, NAN);
        CHECK(vri_cooldown_ready(&c, 20.0), "a NaN cooldown does not block");
    }
    CHECK(vri_parse_on("gun_speed=2\ninstant_items=1\n", "instant_items") == 1, "key on");
    CHECK(vri_parse_on("instant_items = 0\n", "instant_items") == 0, "key off");
    CHECK(vri_parse_on("# instant_items=1\n", "instant_items") == -1, "a comment is no key");
    CHECK(vri_parse_on("instant_items_x=1\n", "instant_items") == -1, "a longer key is another key");
    CHECK(vri_parse_on("x=1\r\n  instant_items=on\r\n", "instant_items") == 1, "CRLF, indent, 'on'");
    CHECK(vri_parse_on("", "instant_items") == -1, "empty");
    CHECK(vri_parse_on("fp=1 anim_smooth=0.25 hurt_speed=2 instant_items=1", "instant_items") == 1,
          "one line of tokens (out/fp.txt), key last");
    CHECK(vri_parse_on("fp=1 instant_items=1 x=2 # instant_items=0", "instant_items") == 1, "mid-line, comment after");
    CHECK(vri_parse_on("fp=1 xinstant_items=1", "instant_items") == -1, "inside another token is no key");
    CHECK(vri_online("1", NULL) && vri_online(NULL, "host") && !vri_online("0", "") && !vri_online(NULL, NULL), "online");

    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("All %d VR instant items checks passed\n", checks);
    return 0;
}
