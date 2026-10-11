// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Physical items in the game's VR UI (docs/vr/findings/39-physical-items-plan.md, phase 1): a
// consumable picked on the quick wheel is not used at once; it is held in a hand, and a gesture
// then fires the game's own use path (quick slot + square, the host's executor):
//   - Mouth  (heals, cures, body buffs)  the item within mouth_radius_m of the mouth, below the eyes,
//                                        for mouth_hold_s (the sandbox's consumables head gesture);
//   - Blade  (weapon buffs)              the item touches the weapon's blade (a segment from the
//                                        weapon bones), held in the hand that holds no weapon of
//                                        the buffed side;
//   - bells  (Mouth kind, `bell`)         the same head gesture, held in the right hand (the sandbox's
//                                        consumables: bells go to the head);
//   - Crush  (marks)                      squeeze the grip to crush_squeeze for crush_hold_s;
//   - Throw  (throwables)                aim mode (default): hold the grip to aim (an arc and a marker
//                                        show the landing), let go to throw toward the marker.
//                                        flick mode (throw_mode / BB_VR_THROW_MODE=flick): the grip is
//                                        let go faster than throw_min_speed: the release velocity gives
//                                        the direction (the host aims the game's bullet along it).
// Selecting the item (the wheel/dashboard click) IS the grip: it is in the hand at once, no grip
// squeeze is needed (a squeeze is only used by Crush, and a squeeze-then-release throws a Throw item;
// a flick of the hand throws it too). It stays until a gesture uses it; a fresh trigger press (after
// the selecting click was let go) or an open menu puts it back (no use). Pure CPU, no I/O: unit tested in vr_game_ui_test.cpp.
// While an item is in a hand (and lockout_s after) BlockMask() says which pad buttons the filter
// hides from the game (swings, the swap tap, L1), so the throw is not also an attack.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "bbport_xr_bridge.h"
#include "vr_held_fire.h"
#include "sandbox3d/math3d.h"
#include "sandbox3d/ui_layout.h"

static_assert(VHF_BTN_RIGHT == BBXR_BTN_RIGHT && VHF_BTN_L2 == BBXR_BTN_L2 && VHF_BTN_R2 == BBXR_BTN_R2 &&
                  VHF_BTN_L1 == BBXR_BTN_L1 && VHF_BTN_R1 == BBXR_BTN_R1 && VHF_BTN_LEFT == BBXR_BTN_LEFT,
              "vr_held_fire.h bit values follow BBXR_BTN_*");

namespace vrui {
namespace held {

enum class Kind { None, Mouth, Blade, Throw, Crush };

struct ItemInfo {
    Kind kind = Kind::None;
    int goods = 0;
    int target_side = 1; // Blade: the weapon that is buffed (1 right, 0 left: the gun)
    int bullet = 0;      // Throw: the BulletParam row the game launches
    int aim_bullet = 0;  // Throw: the bullet the host redirects (the molotov-like chains: the first bullet that flies)
    // Throw: the BulletParam's flight (initVellocity, gravityInRange / OutRange, dist, accelInRange, maxVellocity).
    float speed = 0.0f, g_in = 0.0f, g_out = 0.0f, range_m = 0.0f, accel = 0.0f, max_speed = 0.0f;
    bool bell = false;   // Mouth: a bell (right hand, "TO THE HEAD")
    bool aimed = false;  // Throw: its bullet is in the host's redirect list (BBXR_THROW_BULLET_IDS)
};

/// True when `id` is in a list like "100,105,120-121" (the host's vg_id_in_list for the header's
/// default; "all" matches everything). An unparsable list matches nothing.
inline bool IdInList(const char* list, int id) {
    if (!list) {
        return false;
    }
    if (!std::strcmp(list, "all")) {
        return true;
    }
    const char* s = list;
    while (*s) {
        char* end;
        const long lo = std::strtol(s, &end, 10);
        if (end == s) {
            return false;
        }
        long hi = lo;
        s = end;
        if (*s == '-') {
            hi = std::strtol(s + 1, &end, 10);
            if (end == s + 1) {
                return false;
            }
            s = end;
        }
        if (id >= lo && id <= hi) {
            return true;
        }
        while (*s == ',' || *s == ' ') {
            ++s;
        }
    }
    return false;
}

/// What a game item id (goods 0x40000000 | row) is for. Anything not listed is used at once, as
/// before (keys, bullets...).
inline ItemInfo Classify(uint32_t game_id) {
    ItemInfo info;
    if ((game_id & 0xF0000000u) != BBXR_ITEM_GOODS) {
        return info;
    }
    const int goods = int(game_id & 0x0FFFFFFFu);
    info.goods = goods;
    switch (goods) {
    case 1000: case 703: case 706:                      // Blood Vial, Iosefka's, Adeline's
    case 1100: case 1101:                               // Antidote, Sedative
    case 1110: case 1120: case 2030: case 1500: case 2060: // Beast Blood Pellet, Blue / Lead Elixir, Madman's, Old Hunter Bone
        info.kind = Kind::Mouth;
        return info;
    case 200: case 201: case 205: case 206: case 225: case 226: // the bells: the head gesture
    case 2050: case 4312:                               // Choir Bell, Old Hunter Bell
        info.kind = Kind::Mouth;
        info.bell = true;
        return info;
    case 100: case 1400: case 2090:                     // Hunter's Mark, Bold Hunter's Mark, Shaman Bone Blade: crushed
        info.kind = Kind::Crush;
        return info;
    case 1300: case 1320: case 1310:                    // Fire Paper, Bolt Paper, Empty Phantasm Shell
        info.kind = Kind::Blade;
        info.target_side = 1;
        return info;
    case 1330:                                          // Bone Marrow Ash: the left weapon (the gun)
        info.kind = Kind::Blade;
        info.target_side = 0;
        return info;
    default:
        break;
    }
    // The throwables and the BulletParam row each launches (throwables.json: goods -> bullet).
    // (BulletParam: initVellocity, gravityInRange, gravityOutRange, dist, accelInRange, maxVellocity;
    // 1250 / 1251 are speed-0 spawners: the molotov's flight for their first flying bullet 111 / 115.)
    static const struct {
        int goods, bullet, aim_bullet;
        float speed, g_in, g_out, range_m, accel, max_speed;
    } kThrow[] = {{1200, 100, 100, 15, 8, 12, 6, 0, 50},   {1201, 105, 105, 15, 40, 20, 2, 0, 15},
                  {1210, 121, 121, 20, 0, 9.8f, 8, 10, 50}, {1220, 150, 150, 15, 8, 12, 6, 0, 50},
                  {1230, 170, 170, 20, 3, 16, 10, 0, 20},  {1240, 120, 120, 20, 0, 9.8f, 8, 10, 50},
                  {1250, 110, 111, 15, 8, 12, 6, 0, 50},   {1251, 114, 115, 15, 8, 12, 6, 0, 50},
                  {1260, 160, 160, 15, 0, 12, 0, 0, 50},   {1270, 180, 180, 15, 8, 12, 6, 0, 50},
                  {1410, 300, 300, 2, 9.8f, 9.8f, 20, 0, 20}, {2070, 410, 410, 33, 500, 60, 15, 0, 33},
                  {2080, 350, 350, 4, 0, 0, 20, 40, 4},    {2120, 460, 460, 30, 0, 0, 16, 0, 30},
                  {2130, 470, 470, 15, 3, 7, 7.6f, 10, 20}};
    for (const auto& t : kThrow) {
        if (t.goods == goods) {
            info.kind = Kind::Throw;
            info.bullet = t.bullet;
            info.aim_bullet = t.aim_bullet;
            info.speed = t.speed;
            info.g_in = t.g_in;
            info.g_out = t.g_out;
            info.range_m = t.range_m;
            info.accel = t.accel;
            info.max_speed = t.max_speed;
            info.aimed = IdInList(BBXR_THROW_BULLET_IDS, t.aim_bullet);
            return info;
        }
    }
    return info;
}

/// The hand that holds the item: the right hand takes drinks and throws; a weapon buff goes in the
/// hand that is not the buffed weapon's (the paper touches the blade of the other hand).
inline int HandFor(const ItemInfo& info, int hint = -1) {
    if (info.kind == Kind::Blade) {
        return 1 - info.target_side;
    }
    return info.kind == Kind::Mouth && !info.bell && (hint == 0 || hint == 1) ? hint : 1;
}

/// Distance from a point to the segment a-b.
inline float DistToSegment(m3::Vec3 p, m3::Vec3 a, m3::Vec3 b) {
    const m3::Vec3 ab = b - a;
    const float len2 = m3::Dot(ab, ab);
    if (len2 < 1e-9f) {
        return m3::Length(p - a);
    }
    const float t = std::clamp(m3::Dot(p - a, ab) / len2, 0.0f, 1.0f);
    return m3::Length(p - (a + ab * t));
}

/// The mouth test of the sandbox's head gesture: within the radius of the head-local mouth point
/// and at least `below_eyes` under the head.
inline bool NearMouth(m3::Vec3 item, m3::Vec3 head_pos, m3::Quat head_rot, const sb::UiHeldItems& c,
                      float* distance = nullptr) {
    const m3::Vec3 mouth = head_pos + m3::Rotate(head_rot, c.mouth);
    const float d = m3::Length(item - mouth);
    if (distance) {
        *distance = d;
    }
    return d <= c.mouth_radius_m && item.y <= head_pos.y - c.below_eyes_m;
}

/// The touch test: the item within blade_radius of the blade (the weapon's far part: from
/// blade_start of the length to the tip). `approx` (no hitbox data: the segment is the weapon bone
/// alone): within blade_fallback_m of the bone.
/// Two zones: the blade capsule (blade_radius_m; this includes the tip) and a big extra "mid zone":
/// a sphere of blade_mid_radius_m centred in the middle of the blade part, moved OUT of the blade by
/// blade_mid_offset_m toward the player's head (perpendicular to the blade, on the side facing the
/// head), so the paper can be touched to the air in front of the body instead of reaching the blade.
/// Without `head` the sphere sits on the blade. `frac` (optional) gets the touched point along the
/// whole weapon, 0 (grip) .. 1 (tip); the handle part never counts.
inline bool TouchesBlade(m3::Vec3 item, m3::Vec3 base, m3::Vec3 tip, const sb::UiHeldItems& c,
                         float* distance = nullptr, bool approx = false, float* frac = nullptr,
                         const m3::Vec3* head = nullptr) {
    if (approx) {
        const float d = m3::Length(item - base);
        if (distance) {
            *distance = d;
        }
        if (frac) {
            *frac = 0.0f;
        }
        return d <= c.blade_fallback_m;
    }
    const m3::Vec3 ab = tip - base;
    const float len2 = m3::Dot(ab, ab);
    const float t = len2 < 1e-9f ? 1.0f
                                 : std::clamp(m3::Dot(item - base, ab) / len2, c.blade_start, 1.0f);
    const float d = m3::Length(item - (base + ab * t));
    const bool capsule = d <= c.blade_radius_m;
    // The mid zone: centre on the blade, then out toward the head.
    const float tm = c.blade_start + 0.5f * (1.0f - c.blade_start);
    m3::Vec3 centre = base + ab * tm;
    if (head && len2 >= 1e-9f && c.blade_mid_offset_m > 0.0f) {
        const m3::Vec3 axis = ab * (1.0f / std::sqrt(len2));
        m3::Vec3 out = *head - centre;
        out = out - axis * m3::Dot(out, axis);
        const float ol = m3::Length(out);
        if (ol > 1e-4f) {
            centre = centre + out * (c.blade_mid_offset_m / ol);
        }
    }
    const float dm = m3::Length(item - centre);
    const bool mid = dm <= c.blade_mid_radius_m;
    if (distance) {
        *distance = capsule ? d : std::min(d, dm);
    }
    if (frac) {
        *frac = capsule ? t : (mid ? tm : t);
    }
    return capsule || mid;
}

/// A hand-velocity estimate from timed positions: the blend of the mean velocity over the last
/// `window_s` and the fastest step in it (the sandbox's HandVelocity, throwables.cpp).
class VelocityTracker {
public:
    void Reset() { samples.clear(); }
    void Push(double t, m3::Vec3 p) {
        if (!samples.empty() && t <= samples.back().t) {
            return;
        }
        samples.push_back({t, p});
        while (samples.size() > 2 && t - samples.front().t > 0.5) {
            samples.pop_front();
        }
    }
    m3::Vec3 Estimate(float window_s, float peak_weight) const {
        if (samples.size() < 2) {
            return {};
        }
        const double t_end = samples.back().t, t_start = t_end - double(window_s);
        std::vector<const Sample*> in;
        for (const Sample& s : samples) {
            if (s.t >= t_start - 1e-9) {
                in.push_back(&s);
            }
        }
        if (in.size() < 2) { // sparse frames: the two newest
            in.clear();
            for (auto it = samples.rbegin(); it != samples.rend() && in.size() < 2; ++it) {
                in.insert(in.begin(), &*it);
            }
        }
        const double span = in.back()->t - in.front()->t;
        if (in.size() < 2 || span < 1e-4) {
            return {};
        }
        const m3::Vec3 mean = (in.back()->p - in.front()->p) * float(1.0 / span);
        m3::Vec3 peak = mean;
        float best = -1.0f;
        for (size_t i = 0; i + 1 < in.size(); ++i) {
            const double dt = in[i + 1]->t - in[i]->t;
            if (dt < 1e-4) {
                continue;
            }
            const m3::Vec3 v = (in[i + 1]->p - in[i]->p) * float(1.0 / dt);
            if (m3::Length(v) > best) {
                best = m3::Length(v);
                peak = v;
            }
        }
        return mean * (1.0f - peak_weight) + peak * peak_weight;
    }

private:
    struct Sample {
        double t;
        m3::Vec3 p;
    };
    std::deque<Sample> samples;
};

/// A blade segment in the headset's space (from the weapon bone and the weapon's hitboxes).
struct Blade {
    bool valid = false;
    bool approx = false; // no hitbox data for the weapon: base == tip == the weapon bone
    m3::Vec3 base, tip;
};

struct Frame {
    double time = 0.0;
    bool head_valid = false;
    m3::Vec3 head_pos;
    m3::Quat head_rot;
    bool hand_valid[2] = {};
    m3::Vec3 grip_pos[2];
    m3::Quat grip_rot[2];
    float squeeze[2] = {};
    float trigger[2] = {}; // the cancel press: a fresh trigger pull
    Blade blade[2];        // per weapon side (0 left, 1 right)
    bool blocked = false;  // a menu is engaged or the game's own menu is open
};

struct Pulse {
    int hand;
    float amplitude, seconds;
};

/// What a step decided.
struct Result {
    enum Kind { None, Use, Cancel } kind = None;
    ItemInfo item;
    int hand = 1;
    // Use of a Throw item: where and along what the hand let go (the headset's space).
    bool thrown = false;
    m3::Vec3 throw_origin, throw_dir;
    float throw_speed = 0.0f;
    // The throw decision, for the log: the raw hand velocity, its forward component (horizontal head
    // forward), and how the direction was chosen.
    m3::Vec3 throw_raw_velocity;
    float throw_forward = 0.0f;
    std::string throw_note;
    // An aimed throw: where the arc lands (throw_speed 0: the game's own speed).
    bool has_marker = false;
    m3::Vec3 throw_marker;
    bool marker_on_floor = false;
    float flight_s = 0.0f;
    std::string why;
    float blade_frac = -1.0f; // a blade use: the touched point along the weapon (0 grip .. 1 tip)
    std::vector<Pulse> pulses;
};

/// A throw's flight (ComputeArc): points from the hand, where it lands.
struct ThrowArc {
    std::vector<m3::Vec3> points;
    m3::Vec3 impact;
    bool hit_floor = false;
    float flight_s = 0.0f;
};

/// The BulletParam flight of a throwable from `origin` along `dir_unit` (Y up, meters): explicit Euler,
/// gravity g_in inside range_m (horizontal) of the origin and g_out beyond, accel along the velocity
/// inside the range (capped at max_speed). Ends at floor_y (impact interpolated) or after max_time_s.
inline ThrowArc ComputeArc(m3::Vec3 origin, m3::Vec3 dir_unit, const ItemInfo& p, float floor_y,
                           float max_time_s = 4.0f, float dt = 1.0f / 60.0f) {
    constexpr size_t kMaxPoints = 240;
    ThrowArc arc;
    arc.points.push_back(origin);
    arc.impact = origin;
    if (p.speed <= 0.0f || dt <= 0.0f) {
        return arc;
    }
    m3::Vec3 pos = origin, v = dir_unit * p.speed;
    float t = 0.0f;
    while (t < max_time_s) {
        const float dx = pos.x - origin.x, dz = pos.z - origin.z;
        const float d_h = std::sqrt(dx * dx + dz * dz);
        const bool in_range = p.range_m > 0.0f && d_h < p.range_m;
        const float g = in_range ? p.g_in : p.g_out;
        if (p.accel > 0.0f && in_range) {
            const float sp = m3::Length(v);
            if (sp > 1e-6f) {
                float ns = sp + p.accel * dt;
                if (p.max_speed > 0.0f) {
                    ns = std::min(ns, std::max(p.max_speed, sp));
                }
                v = v * (ns / sp);
            }
        }
        v.y -= g * dt;
        const m3::Vec3 prev = pos;
        pos = pos + v * dt;
        t += dt;
        if (pos.y <= floor_y) {
            const float dy = prev.y - pos.y;
            const float k = dy > 1e-9f ? std::clamp((prev.y - floor_y) / dy, 0.0f, 1.0f) : 1.0f;
            arc.impact = prev + (pos - prev) * k;
            arc.hit_floor = true;
            arc.flight_s = t - dt + dt * k;
            if (arc.points.size() < kMaxPoints) {
                arc.points.push_back(arc.impact);
            } else {
                arc.points.back() = arc.impact;
            }
            return arc;
        }
        if (arc.points.size() < kMaxPoints) {
            arc.points.push_back(pos);
        }
        arc.impact = pos;
        arc.flight_s = t;
    }
    return arc;
}

/// The launch elevation (deg) for a grip elevation: pitch_bias + gain * (grip_elev - level), clamped to
/// [min, max]. `level` is the grip elevation that feels level in the hand (the OpenXR grip pose's -Z points
/// up out of a relaxed fist, so a neutral hand reads about +20 deg): there the throw leaves at pitch_bias
/// (flat, straight ahead); raising the hand adds gain per degree (a lob), lowering it aims down.
inline float AimLaunchElevationDeg(float grip_elev_deg, float pitch_bias_deg, float min_pitch_deg, float max_pitch_deg,
                                   float level_deg = 0.0f, float gain = 1.0f) {
    const float e = pitch_bias_deg + gain * (grip_elev_deg - level_deg);
    return std::clamp(e, min_pitch_deg, std::max(min_pitch_deg, max_pitch_deg));
}

/// The aim of a hand: the grip's forward (-Z) heading, its elevation mapped by AimLaunchElevationDeg
/// (the heading kept; near vertical: the grip's up axis gives it).
inline m3::Vec3 AimDirection(m3::Quat grip_rot, float pitch_bias_deg, float min_pitch_deg, float max_pitch_deg,
                             float level_deg = 0.0f, float gain = 1.0f) {
    constexpr float kDeg = 3.14159265f / 180.0f;
    const m3::Vec3 fwd = m3::Rotate(grip_rot, m3::Vec3{0.0f, 0.0f, -1.0f});
    m3::Vec3 h{fwd.x, 0.0f, fwd.z};
    float hl = m3::Length(h);
    float elev = std::atan2(fwd.y, hl);
    if (hl < 1e-3f) {
        const m3::Vec3 up = m3::Rotate(grip_rot, m3::Vec3{0.0f, 1.0f, 0.0f});
        h = {up.x, 0.0f, up.z};
        hl = m3::Length(h);
        if (hl < 1e-4f) {
            h = {0.0f, 0.0f, -1.0f};
            hl = 1.0f;
        }
        // Pitched down from level the grip's up axis leans to the old forward; pitched up, to the back.
        if (fwd.y > 0.0f) {
            h = h * -1.0f;
        }
    }
    h = h * (1.0f / hl);
    elev = AimLaunchElevationDeg(elev / kDeg, pitch_bias_deg, min_pitch_deg, max_pitch_deg, level_deg, gain) * kDeg;
    return {h.x * std::cos(elev), std::sin(elev), h.z * std::cos(elev)};
}

/// The horizontal forward of a head pose (-Z of the head, flattened). Looking straight down/up the
/// head's up axis is used instead. False when degenerate.
inline bool HeadForwardFlat(m3::Quat head_rot, m3::Vec3& out) {
    m3::Vec3 f = m3::Rotate(head_rot, m3::Vec3{0.0f, 0.0f, -1.0f});
    f.y = 0.0f;
    float len = m3::Length(f);
    if (len < 0.25f) {
        f = m3::Rotate(head_rot, m3::Vec3{0.0f, 1.0f, 0.0f});
        f.y = 0.0f;
        len = m3::Length(f);
    }
    if (len < 1e-4f) {
        return false;
    }
    out = f * (1.0f / len);
    return true;
}

/// Limits a unit direction's pitch to [-kMaxDownSin, kMaxUpSin] (sine of the angle), keeping its heading.
inline m3::Vec3 ClampPitch(m3::Vec3 dir, float max_down_sin, float max_up_sin) {
    const float y = std::clamp(dir.y, -max_down_sin, max_up_sin);
    if (y == dir.y) {
        return dir;
    }
    const float hl = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    if (hl < 1e-6f) {
        return dir;
    }
    const float k = std::sqrt(std::max(0.0f, 1.0f - y * y)) / hl;
    return {dir.x * k, y, dir.z * k};
}

/// Keeps `dir` (unit) in the forward hemisphere: its component along `fwd` at least `min_dot`; the
/// rest (up / sideways share) is kept as far as it fits.
inline m3::Vec3 ClampForward(m3::Vec3 dir, m3::Vec3 fwd, float min_dot) {
    const float d = m3::Dot(dir, fwd);
    if (d >= min_dot) {
        return dir;
    }
    m3::Vec3 perp = dir - fwd * d;
    const float pl = m3::Length(perp);
    if (pl < 1e-4f) {
        return fwd;
    }
    perp = perp * (1.0f / pl);
    return fwd * min_dot + perp * std::sqrt(std::max(0.0f, 1.0f - min_dot * min_dot));
}

class HeldItem {
public:
    enum class State { Idle, Pending, Held };

    /// An item was picked on the wheel (quick slot `slot`). False (nothing held) when it is not one
    /// of the physical items or another item is still in a hand.
    /// `hand_hint`: the hand that grabbed it from a body slot (honoured for drinks only).
    bool Pick(uint32_t game_id, int slot, double now, int hand_hint = -1) {
        if (state != State::Idle) {
            return false;
        }
        const ItemInfo i = Classify(game_id);
        if (i.kind == Kind::None) {
            return false;
        }
        info = i;
        id = game_id;
        quick_slot = slot;
        hand = HandFor(i, hand_hint);
        state = State::Held; // the selection is the grip
        picked_at = now;
        held_at = now;
        gripped = false;
        trig_armed = false;
        pick_pulse = true;
        dwell_since = -1.0;
        progress = 0.0f;
        contact = false;
        velocity.Reset();
        history.clear();
        windup_seen = false;
        aiming = false;
        aim_ready = false;
        aim_arc = ThrowArc{};
        return true;
    }

    /// Puts the item back (no use).
    Result Cancel(double now, const sb::UiHeldItems& c, const char* why) {
        Result r;
        r.kind = Result::Cancel;
        r.item = info;
        r.hand = hand;
        r.why = why;
        if (state == State::Held) {
            r.pulses.push_back({hand, c.cancel.amplitude, c.cancel.seconds});
        }
        Finish(now, c);
        return r;
    }

    Result Step(const Frame& f, const sb::UiHeldItems& c) {
        Result r;
        if (state == State::Idle) {
            return r;
        }
        if (f.blocked) {
            return Cancel(f.time, c, "a menu opened");
        }
        if (!f.hand_valid[hand]) {
            return Cancel(f.time, c, "tracking lost");
        }
        item_pos = f.grip_pos[hand] + m3::Rotate(f.grip_rot[hand], c.hold);
        if (pick_pulse) {
            pick_pulse = false;
            r.pulses.push_back({hand, c.pick.amplitude, c.pick.seconds});
        }
        // The cancel press: both triggers must come up first (the selecting click is still down at the
        // pick), then any fresh pull puts the item back.
        const bool trig_down = f.trigger[0] >= kCancelPull || f.trigger[1] >= kCancelPull;
        if (!trig_armed) {
            trig_armed = f.trigger[0] < kCancelRelease && f.trigger[1] < kCancelRelease;
        } else if (trig_down) {
            return Cancel(f.time, c, "put back (trigger)");
        }
        velocity.Push(f.time, item_pos);
        const m3::Vec3 v_now = velocity.Estimate(c.throw_window_s, c.throw_peak_weight);
        if (f.head_valid) {
            have_fwd = HeadForwardFlat(f.head_rot, head_fwd) || have_fwd;
        }
        history.push_back({f.time, v_now});
        while (history.size() > 1 && f.time - history.front().t > kPeakWindowS) {
            history.pop_front();
        }
        aim_mode = info.kind == Kind::Throw && ThrowMode(c) == "aim";
        if (aim_mode) {
            return StepAim(f, c, r);
        }
        const bool squeezed = f.squeeze[hand] >= c.grab_squeeze;
        const bool opened = gripped && f.squeeze[hand] < c.release_squeeze;
        gripped = squeezed || (gripped && !opened);
        if (info.kind == Kind::Throw) {
            // Thrown by opening a closed grip, or by a flick of the hand once the pick motion is over.
            // Only a FORWARD motion throws: the wind-up (backswing) is as fast as the throw itself and
            // must not release the item backwards; sideways and backwards motions never throw.
            const bool flick_ok = f.time - picked_at >= kFlickArmS;
            const float speed = m3::Length(v_now);
            const float fwd_speed = have_fwd ? m3::Dot(v_now, head_fwd) : speed;
            if (have_fwd && flick_ok && -fwd_speed >= std::max(c.throw_min_speed, kMinForwardSpeed)) {
                windup_seen = true;
            }
            if ((opened || flick_ok) && speed >= c.throw_min_speed && fwd_speed >= std::max(c.throw_min_speed, kMinForwardSpeed)) {
                m3::Vec3 dir = v_now * (1.0f / speed);
                float use_speed = speed;
                std::string note = "forward swing";
                if (have_fwd) {
                    if (m3::Dot(dir, head_fwd) < kMinForwardDot) {
                        // The release velocity points mostly sideways/up: use the fastest forward moment
                        // of the last 150 ms instead.
                        const Hist* best = nullptr;
                        float best_fwd = 0.0f;
                        for (const Hist& h : history) {
                            const float hf = m3::Dot(h.v, head_fwd);
                            if (hf > best_fwd && m3::Dot(h.v * (1.0f / std::max(m3::Length(h.v), 1e-6f)), head_fwd) >= kMinForwardDot) {
                                best_fwd = hf;
                                best = &h;
                            }
                        }
                        if (best) {
                            use_speed = m3::Length(best->v);
                            dir = best->v * (1.0f / use_speed);
                            note = "peak forward swing (last 150 ms)";
                        } else {
                            // Head forward with the swing's pitch.
                            const float hor = std::max(fwd_speed, 1e-3f);
                            const float up = std::clamp(v_now.y / hor, -0.6f, 1.0f);
                            dir = ClampForward(m3::Normalize(head_fwd + m3::Vec3{0.0f, up, 0.0f}), head_fwd, kMinForwardDot);
                            note = "head forward with swing pitch";
                        }
                    }
                    const m3::Vec3 clamped = ClampForward(dir, head_fwd, kMinForwardDot);
                    if (m3::Length(clamped - dir) > 1e-5f) {
                        note += ", clamped to forward";
                    }
                    dir = clamped;
                }
                {
                    const m3::Vec3 pitched = ClampPitch(dir, kMaxDownSin, kMaxUpSin);
                    if (m3::Length(pitched - dir) > 1e-5f) {
                        note += ", pitch limited";
                    }
                    dir = pitched;
                }
                if (windup_seen) {
                    note += ", after a backswing";
                }
                r.kind = Result::Use;
                r.item = info;
                r.hand = hand;
                r.thrown = true;
                r.throw_origin = item_pos;
                r.throw_dir = dir;
                r.throw_speed = use_speed;
                r.throw_raw_velocity = v_now;
                r.throw_forward = fwd_speed;
                r.throw_note = note + (opened ? " (grip opened)" : " (flick)");
                r.why = "thrown";
                r.pulses.push_back({hand, c.use.amplitude, c.use.seconds});
                Finish(f.time, c);
                return r;
            }
        }
        switch (info.kind) {
        case Kind::Mouth: {
            const bool at_mouth = f.head_valid && NearMouth(item_pos, f.head_pos, f.head_rot, c);
            contact = at_mouth;
            if (!at_mouth) {
                dwell_since = -1.0;
                progress = 0.0f;
                break;
            }
            if (dwell_since < 0.0) {
                dwell_since = f.time;
                r.pulses.push_back({hand, c.touch.amplitude, c.touch.seconds});
            }
            progress = c.mouth_hold_s > 0.0f
                           ? std::clamp(float((f.time - dwell_since) / double(c.mouth_hold_s)), 0.0f, 1.0f)
                           : 1.0f;
            if (f.time - dwell_since >= double(c.mouth_hold_s)) {
                return Fire(f, c, "mouth", r);
            }
            break;
        }
        case Kind::Blade: {
            const Blade& b = f.blade[info.target_side];
            bool touch = false;
            if (b.valid) {
                touch = TouchesBlade(item_pos, b.base, b.tip, c, nullptr, b.approx, &touch_frac,
                                     f.head_valid ? &f.head_pos : nullptr);
            }
            contact = touch;
            if (!touch) {
                dwell_since = -1.0;
                progress = 0.0f;
                break;
            }
            if (dwell_since < 0.0) {
                dwell_since = f.time;
                r.pulses.push_back({hand, c.touch.amplitude, c.touch.seconds});
            }
            progress = c.blade_hold_s > 0.0f
                           ? std::clamp(float((f.time - dwell_since) / double(c.blade_hold_s)), 0.0f, 1.0f)
                           : 1.0f;
            if (f.time - dwell_since >= double(c.blade_hold_s)) {
                r.blade_frac = touch_frac;
                return Fire(f, c, "blade", r);
            }
            break;
        }
        case Kind::Crush: {
            const bool squeezed = f.squeeze[hand] >= c.crush_squeeze;
            contact = squeezed;
            if (!squeezed) {
                dwell_since = -1.0;
                progress = 0.0f;
                break;
            }
            if (dwell_since < 0.0) {
                dwell_since = f.time;
                r.pulses.push_back({hand, c.touch.amplitude, c.touch.seconds});
            }
            progress = c.crush_hold_s > 0.0f
                           ? std::clamp(float((f.time - dwell_since) / double(c.crush_hold_s)), 0.0f, 1.0f)
                           : 1.0f;
            if (f.time - dwell_since >= double(c.crush_hold_s)) {
                return Fire(f, c, "crush", r);
            }
            break;
        }
        case Kind::Throw:
        case Kind::None:
            break;
        }
        return r;
    }

    /// The pad buttons the filter hides from the game while an item is in a hand (and lockout_s
    /// after): swings, the right grip tap's weapon swap (d-pad right), L1 when the left hand holds.
    /// Which pad buttons: vr_held_fire.h (the holding hand's attack inputs: left L1 + L2, right R1 + R2 and the
    /// swap tap; the other hand keeps its own; BB_VR_HELD_BLOCK_FIRE=0 gives the older mask).
    uint32_t BlockMask(double now, const sb::UiHeldItems& c) const {
        return vhf_block(BlockFireEnabled() ? 1 : 0, state != State::Idle ? 1 : 0, hand, now,
                         block_until - double(c.lockout_s), VHF_TAIL_S, double(c.lockout_s));
    }
    /// BB_VR_HELD_BLOCK_FIRE (default on), read once.
    static bool BlockFireEnabled() {
        static const bool on = [] {
            const char* e = std::getenv("BB_VR_HELD_BLOCK_FIRE");
            return !(e && *e && std::atoi(e) == 0);
        }();
        return on;
    }

    /// A float env override (read each call is cheap enough: aiming only), else the layout's value.
    static float AimEnv(const char* name, float fallback) {
        const char* e = std::getenv(name);
        if (e && *e) {
            char* end = nullptr;
            const float v = std::strtof(e, &end);
            if (end != e && std::isfinite(v)) {
                return v;
            }
        }
        return fallback;
    }

    /// "aim" or "flick": the layout's throw_mode, BB_VR_THROW_MODE (read once) first.
    static std::string ThrowMode(const sb::UiHeldItems& c) {
        static const std::string env = [] {
            const char* e = std::getenv("BB_VR_THROW_MODE");
            return std::string(e ? e : "");
        }();
        if (env == "flick" || env == "aim") {
            return env;
        }
        return c.throw_mode == "flick" ? "flick" : "aim";
    }
    bool Aiming() const { return aiming && state != State::Idle; }
    const ThrowArc* Aim() const { return Aiming() ? &aim_arc : nullptr; }
    m3::Vec3 AimDir() const { return aim_dir; }

    State GetState() const { return state; }
    bool Active() const { return state != State::Idle; }
    int Hand() const { return hand; }
    const ItemInfo& Info() const { return info; }
    uint32_t Id() const { return id; }
    int QuickSlot() const { return quick_slot; }
    m3::Vec3 ItemPos() const { return item_pos; }
    float Progress() const { return progress; }
    bool InContact() const { return contact; }
    bool Gripped() const { return state == State::Held; } // in the hand (the selection is the grip)
    double BlockUntil() const { return block_until; }
    /// What to do with the item, for the HUD.
    const char* Hint() const {
        switch (info.kind) {
        case Kind::Mouth: return info.bell ? "TO THE HEAD" : "TO THE MOUTH";
        case Kind::Crush: return "SQUEEZE TO CRUSH";
        case Kind::Blade: return "TOUCH THE BLADE";
        case Kind::Throw: return aim_mode ? (aiming ? "RELEASE: THROW" : "HOLD GRIP: AIM") : "THROW IT";
        default: return "";
        }
    }
    /// The second HUD line: how to put it back.
    const char* CancelHint() const { return "TRIGGER: PUT BACK"; }

private:
    Result& Fire(const Frame& f, const sb::UiHeldItems& c, const char* how, Result& r) {
        r.kind = Result::Use;
        r.item = info;
        r.hand = hand;
        r.why = how;
        r.pulses.push_back({hand, c.use.amplitude, c.use.seconds});
        Finish(f.time, c);
        return r;
    }

    // Aim mode: hold the grip to aim (arc + marker), let go to throw. The grip must be opened once
    // after the pick before it can aim (the pick may have been made with it closed).
    Result StepAim(const Frame& f, const sb::UiHeldItems& c, Result& r) {
        const float sq = f.squeeze[hand];
        if (!aim_ready && sq < c.release_squeeze) {
            aim_ready = true;
        }
        if (!aiming && aim_ready && sq >= c.grab_squeeze) {
            aiming = true;
            r.pulses.push_back({hand, c.touch.amplitude, c.touch.seconds});
        }
        if (!aiming) {
            return r;
        }
        if (sq < c.release_squeeze) {
            r.kind = Result::Use;
            r.item = info;
            r.hand = hand;
            r.thrown = true;
            r.throw_origin = item_pos;
            r.throw_dir = aim_dir;
            r.throw_speed = 0.0f;
            r.has_marker = true;
            r.throw_marker = aim_arc.impact;
            r.marker_on_floor = aim_arc.hit_floor;
            r.flight_s = aim_arc.flight_s;
            r.throw_note = "aimed (grip released)";
            r.why = "thrown";
            r.pulses.push_back({hand, c.use.amplitude, c.use.seconds});
            Finish(f.time, c);
            return r;
        }
        aim_dir = AimDirection(f.grip_rot[hand], AimEnv("BB_VR_THROW_AIM_BIAS", c.throw_aim_pitch_bias_deg),
                               c.throw_aim_min_pitch_deg, c.throw_aim_max_pitch_deg,
                               AimEnv("BB_VR_THROW_AIM_LEVEL", c.throw_aim_level_deg),
                               AimEnv("BB_VR_THROW_AIM_GAIN", c.throw_aim_gain));
        const float floor_y = f.head_valid ? f.head_pos.y - c.throw_eye_height_m : item_pos.y - 1.0f;
        aim_arc = ComputeArc(item_pos, aim_dir, info, floor_y);
        return r;
    }

    static constexpr float kCancelPull = 0.6f, kCancelRelease = 0.3f;
    static constexpr float kMinForwardDot = 0.3f;   // a throw direction never has less forward than this
    static constexpr float kMinForwardSpeed = 2.5f; // a real forward flick (live: 1.5-1.9 m/s releases were wind-ups)
    static constexpr float kMaxUpSin = 0.866f, kMaxDownSin = 0.707f; // pitch limits: +60 / -45 degrees
    static constexpr double kPeakWindowS = 0.15;
    static constexpr double kFlickArmS = 0.35; // a throw by flick waits for the pick's own motion to end

    void Finish(double now, const sb::UiHeldItems& c) {
        state = State::Idle;
        block_until = now + double(c.lockout_s);
        dwell_since = -1.0;
        progress = 0.0f;
        contact = false;
        aiming = false;
    }

    State state = State::Idle;
    ItemInfo info;
    uint32_t id = BBXR_ITEM_NONE;
    int quick_slot = -1, hand = 1;
    double picked_at = 0.0, held_at = 0.0, dwell_since = -1.0, block_until = -1e9;
    float progress = 0.0f, touch_frac = 0.0f;
    bool contact = false, gripped = false, trig_armed = false, pick_pulse = false;
    m3::Vec3 item_pos;
    VelocityTracker velocity;
    struct Hist {
        double t;
        m3::Vec3 v;
    };
    std::deque<Hist> history; // the velocity estimate per frame, last kPeakWindowS
    m3::Vec3 head_fwd;
    bool have_fwd = false, windup_seen = false;
    bool aiming = false, aim_ready = false, aim_mode = false;
    ThrowArc aim_arc;
    m3::Vec3 aim_dir;
};

/// The weapon hand's pulse for a collision of the game's weapon (BBXR_EV_SWING_HIT): `contact` is
/// the event's b (0 character, 1 wall, 2 object), `paid` its a (the stamina the attack cost, 0
/// when unknown), `strength_ref` the cost that counts as a full-strength hit.
inline Pulse CollisionPulse(const sb::UiCollisionHaptics& c, int hand, int contact, int paid) {
    const sb::UiPulse base = contact == 1 ? c.wall : contact == 2 ? c.object : c.hit;
    float t = 0.0f;
    if (c.scale_by_strength && paid > 0) {
        t = std::clamp(float(paid) / c.strength_ref, 0.0f, 1.0f);
    }
    // A wall or object bounce already starts strong; the blend only lifts it towards hit_strong.
    Pulse p{hand, base.amplitude, base.seconds};
    if (c.scale_by_strength) {
        p.amplitude = base.amplitude + (std::max(c.hit_strong.amplitude, base.amplitude) - base.amplitude) * t;
        p.seconds = base.seconds + (std::max(c.hit_strong.seconds, base.seconds) - base.seconds) * t;
    }
    p.amplitude = std::clamp(p.amplitude, 0.0f, 1.0f);
    p.seconds = std::clamp(p.seconds, 0.0f, 1.0f);
    return p;
}

} // namespace held
} // namespace vrui
