// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT

#include "parry.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "../../../src/vr_gun_math.h"

namespace sb {

using namespace m3;

namespace {

void Read(const json::Value& v, float& out) {
    if (v.IsNumber()) {
        out = v.Float();
    }
}
void Read(const json::Value& v, bool& out) {
    if (!v.IsNull()) {
        out = v.Bool(out);
    }
}

VrVec3 ToVr(Vec3 v) { return vr_v3(v.x, v.y, v.z); }
Vec3 FromVr(VrVec3 v) { return {v.x, v.y, v.z}; }

std::string Fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

int PopCount(unsigned v) {
    int n = 0;
    for (; v; v >>= 1) {
        n += int(v & 1u);
    }
    return n;
}

} // namespace

void ParryConfig::Load(const json::Value& d) {
    if (!d.IsObject()) {
        return;
    }
    Read(d["enabled"], enabled);
    Read(d["trigger_fire"], trigger_fire);
    Read(d["sound"], sound);
    const json::Value& s = d["shoot"];
    Read(s["speed_mps"], shoot_speed);
    Read(s["align"], shoot_align);
    Read(s["rearm_mps"], shoot_rearm);
    Read(s["debounce_ms"], shoot_debounce_ms);
    const json::Value& a = d["assist"];
    Read(a["enabled"], assist);
    Read(a["strength"], assist_strength);
    Read(a["radius_m"], assist_radius_m);
    Read(a["cone_min_deg"], assist_cone_min_deg);
    Read(a["cone_max_deg"], assist_cone_max_deg);
    Read(a["range_m"], assist_range_m);
    const json::Value& b = d["bullet"];
    Read(b["speed_mps"], bullet_speed);
    Read(b["range_m"], bullet_range_m);
    const json::Value& m = d["dummy"];
    Read(m["enabled"], dummy);
    Read(m["attack_loop"], attack_loop);
    Read(m["distance_m"], distance_m);
    Read(m["x_m"], x_m);
    Read(m["idle_s"], idle_s);
    Read(m["windup_s"], windup_s);
    Read(m["window_s"], window_s);
    Read(m["swing_s"], swing_s);
    Read(m["recover_s"], recover_s);
    Read(m["stagger_s"], stagger_s);
    Read(m["down_s"], down_s);
    const json::Value& v = d["visceral"];
    const json::Value& vm = v["mode"];
    if (vm.IsString()) {
        if (vm.Str() == "thrust") visceral_mode = kVisceralThrust;
        else if (vm.Str() == "swing") visceral_mode = kVisceralSwing;
        else if (vm.Str() == "auto") visceral_mode = kVisceralAuto;
    }
    Read(v["speed_mps"], visceral_speed);
    Read(v["align"], visceral_align);
    Read(v["reach_m"], visceral_reach_m);
    Read(v["debounce_ms"], visceral_debounce_ms);
    visceral_speed = std::clamp(visceral_speed, 0.1f, 10.0f);
    visceral_align = std::clamp(visceral_align, -1.0f, 0.999f);
    visceral_reach_m = std::clamp(visceral_reach_m, 0.1f, 5.0f);
    visceral_debounce_ms = std::clamp(visceral_debounce_ms, 0.0f, 3000.0f);
    shoot_speed = std::clamp(shoot_speed, 0.1f, 10.0f);
    shoot_align = std::clamp(shoot_align, -1.0f, 0.999f);
    shoot_rearm = std::clamp(shoot_rearm, 0.0f, 5.0f);
    shoot_debounce_ms = std::clamp(shoot_debounce_ms, 0.0f, 3000.0f);
    assist_strength = std::clamp(assist_strength, 0.0f, 1.0f);
    assist_radius_m = std::max(assist_radius_m, 0.01f);
    assist_cone_min_deg = std::max(assist_cone_min_deg, 0.0f);
    assist_cone_max_deg = std::max(assist_cone_max_deg, assist_cone_min_deg);
    assist_range_m = std::max(assist_range_m, 0.5f);
    bullet_speed = std::clamp(bullet_speed, 1.0f, 500.0f);
    bullet_range_m = std::clamp(bullet_range_m, 1.0f, 500.0f);
    distance_m = std::clamp(distance_m, 0.5f, 12.0f);
    idle_s = std::max(idle_s, 0.05f);
    windup_s = std::max(windup_s, 0.05f);
    window_s = std::clamp(window_s, 0.01f, 3.0f);
    swing_s = std::max(swing_s, 0.05f);
    recover_s = std::max(recover_s, 0.0f);
    stagger_s = std::max(stagger_s, 0.0f);
    down_s = std::max(down_s, 0.0f);
}

const char* ParryConfig::ModeName(VisceralMode m) {
    return m == kVisceralSwing ? "swing" : m == kVisceralAuto ? "auto" : "thrust";
}

std::string ParryConfig::Summary(const std::string& in) const {
    std::ostringstream s;
    s << std::boolalpha;
    s << "{\n"
      << in << "  \"enabled\": " << enabled << ", \"trigger_fire\": " << trigger_fire
      << ", \"sound\": " << sound << ",\n"
      << in << "  \"shoot\": {\"speed_mps\": " << shoot_speed << ", \"align\": " << shoot_align
      << ", \"rearm_mps\": " << shoot_rearm << ", \"debounce_ms\": " << shoot_debounce_ms << "},\n"
      << in << "  \"assist\": {\"enabled\": " << assist << ", \"strength\": " << assist_strength
      << ", \"radius_m\": " << assist_radius_m << ", \"cone_min_deg\": " << assist_cone_min_deg
      << ", \"cone_max_deg\": " << assist_cone_max_deg << ", \"range_m\": " << assist_range_m
      << "},\n"
      << in << "  \"bullet\": {\"speed_mps\": " << bullet_speed << ", \"range_m\": "
      << bullet_range_m << "},\n"
      << in << "  \"dummy\": {\"enabled\": " << dummy << ", \"attack_loop\": " << attack_loop
      << ", \"distance_m\": " << distance_m << ", \"x_m\": " << x_m << ", \"idle_s\": " << idle_s
      << ", \"windup_s\": " << windup_s << ", \"window_s\": " << window_s
      << ", \"swing_s\": " << swing_s << ", \"recover_s\": " << recover_s
      << ", \"stagger_s\": " << stagger_s << ", \"down_s\": " << down_s << "},\n"
      << in << "  \"visceral\": {\"mode\": \"" << ModeName(visceral_mode)
      << "\", \"speed_mps\": " << visceral_speed << ", \"align\": " << visceral_align
      << ", \"reach_m\": " << visceral_reach_m << ", \"debounce_ms\": " << visceral_debounce_ms
      << "}\n"
      << in << "}";
    return s.str();
}

std::string RejectNames(unsigned bits) {
    if (!bits) {
        return "none";
    }
    std::string out;
    const struct {
        unsigned bit;
        const char* name;
    } names[] = {{kRejectSpeed, "speed"},       {kRejectAlign, "alignment"},
                 {kRejectFalling, "falling"},   {kRejectDebounce, "debounce"},
                 {kRejectDisarmed, "disarmed"},
                 {kRejectReach, "reach"}};
    for (const auto& n : names) {
        if (bits & n.bit) {
            out += (out.empty() ? "" : "+") + std::string(n.name);
        }
    }
    return out;
}

const char* ParryTrainer::PhaseName(Phase p) {
    switch (p) {
    case kIdle: return "idle";
    case kWindUp: return "wind-up";
    case kWindow: return "WINDOW";
    case kSwing: return "swing";
    case kRecover: return "recover";
    case kStagger: return "staggered";
    case kDown: return "down";
    }
    return "?";
}

std::string ParryTrainer::ScoreText() const {
    return Fmt("Parries %d  Early %d  Late %d  Miss %d  Visceral %d  (shots %d)", score.parries,
               score.early, score.late, score.misses, score.visceral, score.shots);
}

float ParryTrainer::WindUpFraction(double now) const {
    if (phase != kWindUp) {
        return phase == kWindow ? 1.0f : 0.0f;
    }
    const double len = std::max(phase_end - phase_start, 1e-6);
    return float(std::clamp((now - phase_start) / len, 0.0, 1.0));
}

void ParryTrainer::Reset() {
    *this = ParryTrainer{};
}

void ParryTrainer::Say(ParryEvents& e, const std::string& text) {
    e.log += "parry: " + text + "\n";
}

void ParryTrainer::Pulse(ParryEvents& e, int hand, float amplitude, float seconds) {
    if (amplitude > e.haptic_amplitude[hand]) {
        e.haptic_amplitude[hand] = amplitude;
        e.haptic_seconds[hand] = seconds;
    }
}

void ParryTrainer::EnterPhase(Phase p, double t, double length) {
    phase = p;
    phase_start = t;
    phase_end = t + length;
}

void ParryTrainer::UpdateShape(const ParryInput& in, const ParryConfig& c) {
    if (!in.parts.empty()) {
        parts = in.parts;
    } else if (c.dummy && !own_placed && in.head_valid) {
        // No enemy model: a stand-in of capsules straight ahead of the head.
        Vec3 f = Rotate(in.head_rotation, {0, 0, -1});
        f.y = 0.0f;
        f = Normalize(f, {0, 0, -1});
        const Vec3 right = Cross(f, {0, 1, 0});
        const Vec3 p = in.head_position + f * c.distance_m + right * c.x_m;
        const float y = in.floor_y;
        own_parts = {{{p.x, y + 0.95f, p.z}, {p.x, y + 1.45f, p.z}, 0.24f},
                     {{p.x, y + 1.70f, p.z}, {p.x, y + 1.70f, p.z}, 0.12f},
                     {{p.x - 0.12f, y + 0.12f, p.z}, {p.x - 0.12f, y + 0.85f, p.z}, 0.13f},
                     {{p.x + 0.12f, y + 0.12f, p.z}, {p.x + 0.12f, y + 0.85f, p.z}, 0.13f}};
        own_placed = true;
    }
    if (in.parts.empty() && own_placed) {
        parts = own_parts;
    }
    have_dummy = c.dummy && !parts.empty();
    if (!have_dummy) {
        return;
    }
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const col::Capsule& cap : parts) {
        for (const Vec3 p : {cap.a, cap.b}) {
            lo = {std::min(lo.x, p.x - cap.r), std::min(lo.y, p.y - cap.r), std::min(lo.z, p.z - cap.r)};
            hi = {std::max(hi.x, p.x + cap.r), std::max(hi.y, p.y + cap.r), std::max(hi.z, p.z + cap.r)};
        }
    }
    height = std::max(hi.y - lo.y, 0.5f);
    const Vec3 centre{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
    base = {centre.x, lo.y, centre.z};
    target = {centre.x, lo.y + 0.68f * height, centre.z}; // the chest
    if (in.head_valid) {
        const Vec3 to_head = Normalize(Vec3{in.head_position.x - base.x, 0.0f, in.head_position.z - base.z},
                                       facing);
        facing = to_head;
    }
}

void ParryTrainer::AdvanceTo(double t, const ParryConfig& c, ParryEvents& e) {
    for (int guard = 0; guard < 16 && started && t >= phase_end; ++guard) {
        const double at = phase_end;
        switch (phase) {
        case kIdle:
            if (c.attack_loop) {
                EnterPhase(kWindUp, at, c.windup_s);
                window_start = at + c.windup_s;
                window_len = c.window_s;
                Pulse(e, 0, 0.15f, 0.03f);
                if (c.sound) {
                    e.beep_hz = 440.0f, e.beep_ms = 60.0f;
                }
            } else {
                phase_end = at + 0.25; // stands still: stay idle
            }
            break;
        case kWindUp:
            EnterPhase(kWindow, at, window_len);
            Pulse(e, 0, 0.5f, 0.05f);
            if (c.sound) {
                e.beep_hz = 1320.0f, e.beep_ms = 45.0f;
            }
            break;
        case kWindow: EnterPhase(kSwing, at, c.swing_s); break;
        case kSwing: EnterPhase(kRecover, at, c.recover_s); break;
        case kRecover: EnterPhase(kIdle, at, c.idle_s); break;
        case kStagger:
            Say(e, Fmt("the dummy recovers after %.1f s (no visceral)", phase_end - phase_start));
            EnterPhase(kIdle, at, c.idle_s);
            break;
        case kDown: EnterPhase(kIdle, at, c.idle_s); break;
        }
    }
}

void ParryTrainer::Judge(double t, const ParryConfig& c, ParryEvents& e) {
    switch (phase) {
    case kWindow: {
        ++score.parries;
        const double into = (t - window_start) * 1000.0;
        EnterPhase(kStagger, t, c.stagger_s);
        Say(e, Fmt("PARRY! the bullet hit %.0f ms into the %.0f ms window (stagger %.1f s) | %s", into,
                   window_len * 1000.0, c.stagger_s, ScoreText().c_str()));
        e.toast_title = "PARRY!";
        e.toast_body = Fmt("%.0f ms into the window. R1 swing / thrust = visceral (%.1f s).\n", into, c.stagger_s) +
                       ScoreText();
        Pulse(e, 0, 1.0f, 0.25f);
        Pulse(e, 1, 0.6f, 0.12f);
        if (c.sound) {
            e.beep_hz = 880.0f, e.beep_ms = 140.0f;
        }
        break;
    }
    case kWindUp: {
        ++score.early;
        const double ms = (window_start - t) * 1000.0;
        Say(e, Fmt("TOO EARLY by %.0f ms (wind-up, the window opens at +%.0f ms) | %s", ms, ms,
                   ScoreText().c_str()));
        e.toast_title = "TOO EARLY";
        e.toast_body = Fmt("by %.0f ms\n", ms) + ScoreText();
        Pulse(e, 0, 0.35f, 0.06f);
        break;
    }
    case kSwing:
    case kRecover: {
        ++score.late;
        const double ms = (t - (window_start + window_len)) * 1000.0;
        Say(e, Fmt("TOO LATE by %.0f ms (the window closed) | %s", ms, ScoreText().c_str()));
        e.toast_title = "TOO LATE";
        e.toast_body = Fmt("by %.0f ms\n", ms) + ScoreText();
        Pulse(e, 0, 0.35f, 0.06f);
        break;
    }
    case kIdle:
        ++score.idle_hits;
        Say(e, "hit the dummy while it was idle (not graded)");
        break;
    case kStagger:
    case kDown: Say(e, Fmt("hit the %s dummy (not graded)", PhaseName(phase))); break;
    }
}

void ParryTrainer::Fire(const ParryInput& in, const ParryConfig& c, ParryEvents& e, const char* how) {
    Vec3 fwd = Normalize(Rotate(in.aim_rotation, {0, 0, -1}), {0, 0, -1});
    const Vec3 muzzle = in.muzzle_valid ? in.muzzle : in.aim_position + fwd * 0.12f;
    Vec3 dir = fwd;
    std::string assist_text = "assist off";
    if (have_dummy) {
        const float to_target = vg_angle_deg(ToVr(fwd), ToVr(target - muzzle));
        if (c.assist && c.assist_strength > 0.0f) {
            VgAssist a = vg_assist_defaults();
            a.radius = c.assist_radius_m;
            a.cone_min = c.assist_cone_min_deg;
            a.cone_max = c.assist_cone_max_deg;
            a.strength = c.assist_strength;
            a.range = c.assist_range_m;
            const VrVec3 tv = ToVr(target);
            int picked = -1;
            float angle = 0.0f;
            dir = FromVr(vg_assist_dir(ToVr(fwd), ToVr(muzzle), &tv, 1, &a, &picked, &angle));
            const float cone = vg_assist_cone(&a, Length(target - muzzle));
            if (picked >= 0) {
                assist_text = Fmt("assist pulled %.1f deg (ray %.1f deg off the chest, cone %.1f)",
                                  vg_angle_deg(ToVr(fwd), ToVr(dir)), angle, cone);
            } else {
                assist_text = Fmt("assist: no pull (ray %.1f deg off the chest, cone %.1f deg, %.1f m)",
                                  to_target, cone, Length(target - muzzle));
            }
        } else {
            assist_text = Fmt("assist off (ray %.1f deg off the chest)", to_target);
        }
    }
    Bullet b;
    b.pos = muzzle;
    b.dir = Normalize(dir, fwd);
    b.born = in.time;
    bullets.push_back(b);
    last_shot = in.time;
    last_muzzle = muzzle;
    last_dir = b.dir;
    ++score.shots;
    Say(e, Fmt("SHOT #%d (%s) thrust %.2f m/s (>= %.2f) align %.2f (>= %.2f) | %s", score.shots, how,
               thrust, c.shoot_speed, align, c.shoot_align, assist_text.c_str()));
    Pulse(e, 0, 0.8f, 0.06f);
}

void ParryTrainer::Visceral(double t, const ParryConfig& c, ParryEvents& e, const char* how,
                            const std::string& detail) {
    ++score.visceral;
    const double after = (t - phase_start) * 1000.0;
    EnterPhase(kDown, t, c.down_s);
    Say(e, Fmt("VISCERAL! (%s) %.0f ms after the parry%s%s | %s", how, after, detail.empty() ? "" : " ",
               detail.c_str(), ScoreText().c_str()));
    e.toast_title = "VISCERAL!";
    e.toast_body = ScoreText();
    Pulse(e, 1, 1.0f, 0.3f);
    if (c.sound) {
        e.beep_hz = 220.0f, e.beep_ms = 200.0f;
    }
}

void ParryTrainer::VisceralStep(const ParryInput& in, const ParryConfig& c, ParryEvents& e) {
    const bool stagger = phase == kStagger && have_dummy;
    const bool track = in.rhand_valid && in.head_valid;
    if (!track) {
        rhistory.clear();
        vis_have_head = false;
        vis_attempt = false;
    }
    // Auto: standing within reach of the staggered dummy finishes it.
    if (stagger && c.visceral_mode == ParryConfig::kVisceralAuto) {
        const float dh = in.rhand_valid ? Length(in.rhand_position - target) : 1e9f;
        const float dhead = in.head_valid ? Length(in.head_position - target) : 1e9f;
        if (std::min(dh, dhead) <= c.visceral_reach_m) {
            Visceral(in.time, c, e, "auto",
                     Fmt("(%s %.2f m from the chest, reach %.2f m)", dh <= dhead ? "hand" : "head",
                         std::min(dh, dhead), c.visceral_reach_m));
            return;
        }
    }
    if (!track) {
        return;
    }
    rhistory.push_back({in.time, in.rhand_position, in.head_position});
    while (rhistory.size() > 2 && in.time - rhistory[1].t > 0.25) {
        rhistory.erase(rhistory.begin());
    }
    const Sample* ref = nullptr;
    for (const Sample& s : rhistory) {
        if (in.time - s.t >= 0.030) {
            ref = &s;
        }
    }
    Vec3 rel{};
    if (ref) {
        const float w = float(in.time - ref->t);
        rel = ((in.rhand_position - ref->hand) - (in.head_position - ref->head)) * (1.0f / w);
    }
    const float yaw = YawOf(in.head_rotation);
    bool blocked = false;
    if (vis_have_head) {
        float dyaw = std::fabs(yaw - vis_yaw_prev);
        dyaw = std::min(dyaw, 2.0f * kPi - dyaw);
        blocked = Length(in.head_position - vis_head_prev) > 0.25f || dyaw > 25.0f * kDeg;
    }
    vis_head_prev = in.head_position;
    vis_yaw_prev = yaw;
    vis_have_head = true;

    const Vec3 fwd = Normalize(Rotate(in.rhand_rotation, {0, 0, -1}), {0, 0, -1});
    const float t = Dot(rel, fwd); // the thrust along the hand's aim
    const float sp = Length(rel);
    const Vec3 to_chest = have_dummy ? Normalize(target - in.rhand_position, fwd) : fwd;
    const float a = sp > 1e-3f ? Dot(rel, to_chest) / sp : 0.0f; // the stroke runs at the chest
    const float dist = have_dummy ? Length(target - in.rhand_position) : 1e9f;
    const double now_ms = in.time * 1000.0;
    const float before = vis_prev;
    vis_prev = t;
    unsigned bits = 0;
    if (t < c.visceral_speed) bits |= kRejectSpeed;
    if (a < c.visceral_align) bits |= kRejectAlign;
    if (!(t > before)) bits |= kRejectFalling;
    if (now_ms - vis_last_ms < c.visceral_debounce_ms) bits |= kRejectDebounce;
    if (!vis_armed || blocked) bits |= kRejectDisarmed;
    if (dist > c.visceral_reach_m) bits |= kRejectReach;

    // Outside the stagger (or in another mode) a thrust does nothing and does not use up the arming.
    const bool live = stagger && c.visceral_mode == ParryConfig::kVisceralThrust;
    bool fired = false;
    if (blocked) {
        vis_armed = false;
    } else if (bits == 0 && live) {
        fired = true;
        vis_armed = false;
        vis_last_ms = now_ms;
    } else if (t < c.shoot_rearm) {
        vis_armed = true;
    }

    // One "rejected" line per thrust, only while the dummy is staggered.
    if (live && !vis_attempt && t >= 0.5f * c.visceral_speed) {
        vis_attempt = true;
        vis_fired = false;
        vis_bits = 0xFFu;
        vis_peak = 0.0f;
    }
    if (vis_attempt) {
        vis_peak = std::max(vis_peak, t);
        if (PopCount(bits) < PopCount(vis_bits) || (PopCount(bits) == PopCount(vis_bits) && t > vis_best_t)) {
            vis_bits = bits;
            vis_best_t = t;
            vis_best_a = a;
            vis_best_d = dist;
        }
    }
    if (fired) {
        vis_fired = true;
        Visceral(in.time, c, e, "thrust",
                 Fmt("(thrust %.2f m/s >= %.2f, align %.2f >= %.2f, %.2f m from the chest <= %.2f)", t,
                     c.visceral_speed, a, c.visceral_align, dist, c.visceral_reach_m));
    }
    if (vis_attempt && (t < c.shoot_rearm || !live)) {
        if (!vis_fired && live) {
            Say(e, Fmt("visceral thrust rejected: %s (best frame: thrust %.2f m/s vs %.2f, align %.2f vs "
                       "%.2f, %.2f m vs reach %.2f; peak thrust %.2f m/s)",
                       RejectNames(vis_bits).c_str(), vis_best_t, c.visceral_speed, vis_best_a,
                       c.visceral_align, vis_best_d, c.visceral_reach_m, vis_peak));
        }
        vis_attempt = false;
    }
}

void ParryTrainer::Step(const ParryInput& in, const ParryConfig& c, ParryEvents& e) {
    if (!c.enabled) {
        return;
    }
    UpdateShape(in, c);
    const double dt = started || last_time > 0.0 ? std::clamp(in.time - last_time, 0.0, 0.1) : 0.0;
    if (!started && have_dummy) {
        started = true;
        EnterPhase(kIdle, in.time, c.idle_s);
        Say(e, Fmt("dummy ready %.1f m away: wind-up %.2f s, window %.0f ms, swing %.2f s; "
                   "thrust the left gun at it (>= %.1f m/s along the barrel)",
                   have_dummy ? Length(Vec3{base.x - in.head_position.x, 0, base.z - in.head_position.z}) : 0.0f,
                   c.windup_s, c.window_s * 1000.0f, c.swing_s, c.shoot_speed));
    }

    // 1. The bullets fly over [last_time, time]; hits are judged at the dummy's clock of that moment.
    std::vector<Hit> hits;
    for (size_t i = 0; i < bullets.size(); ++i) {
        Bullet& b = bullets[i];
        if (!b.alive) {
            continue;
        }
        const float step = c.bullet_speed * float(dt);
        const Vec3 p0 = b.pos, p1 = b.pos + b.dir * step;
        if (have_dummy) {
            // Closest approach to the chest (for the miss report).
            const Vec3 q = col::ClosestOnSegment(p0, p1, target);
            b.closest_m = std::min(b.closest_m, Length(q - target));
        }
        float best_s = 2.0f;
        for (const col::Capsule& cap : parts) {
            float s = 0.0f, u = 0.0f;
            Vec3 c1, c2;
            const float d2 = col::ClosestSegmentSegment(p0, p1, cap.a, cap.b, s, u, c1, c2);
            if (d2 <= cap.r * cap.r && have_dummy) {
                best_s = std::min(best_s, s);
            }
        }
        if (best_s <= 1.0f) {
            b.alive = false;
            b.pos = p0 + (p1 - p0) * best_s;
            hits.push_back({i, last_time + double(best_s) * dt, 0.0f});
            continue;
        }
        b.pos = p1;
        b.travelled += step;
        if (p1.y < in.floor_y || b.travelled > c.bullet_range_m) {
            b.alive = false;
            ++score.misses;
            Say(e, Fmt("MISS: the bullet passed %.0f cm from the dummy's chest (%s) | %s",
                       b.closest_m * 100.0f, p1.y < in.floor_y ? "floor" : "range", ScoreText().c_str()));
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.time < b.time; });
    for (const Hit& h : hits) {
        AdvanceTo(h.time, c, e);
        Judge(h.time, c, e);
    }
    AdvanceTo(in.time, c, e);
    bullets.erase(std::remove_if(bullets.begin(), bullets.end(),
                                 [&](const Bullet& b) { return !b.alive && in.time - b.born > 1.0; }),
                  bullets.end());

    // 2. A right-hand melee hit on the staggered dummy is a visceral (every mode); a thrust of the
    // right hand at it (mode thrust) or standing within reach (mode auto) too.
    if (in.melee_hit_time > last_melee_seen) {
        if (phase == kStagger && in.melee_hit_time >= phase_start) {
            Visceral(in.melee_hit_time, c, e, "swing", "");
        } else {
            Say(e, Fmt("melee hit while %s (a visceral needs the staggered dummy)", PhaseName(phase)));
        }
        last_melee_seen = in.melee_hit_time;
    }
    VisceralStep(in, c, e);

    // 3. The thrust gate on the left hand (velocity relative to the head, along the barrel).
    gate.on = c.shoot_speed;
    gate.align = c.shoot_align;
    gate.rearm = c.shoot_rearm;
    gate.debounce_ms = c.shoot_debounce_ms;
    const bool ok = in.hand_valid && in.head_valid && in.hand_free;
    if (!ok) {
        history.clear();
        attempt = false;
        have_head_prev = false;
        thrust = align = 0.0f;
        last_time = in.time;
        trigger_prev = in.trigger;
        return;
    }
    history.push_back({in.time, in.hand_position, in.head_position});
    while (history.size() > 2 && in.time - history[1].t > 0.25) {
        history.erase(history.begin());
    }
    // The newest sample at least 30 ms old gives the velocity (a one-frame difference is noise).
    const Sample* ref = nullptr;
    for (const Sample& s : history) {
        if (in.time - s.t >= 0.030) {
            ref = &s;
        }
    }
    Vec3 rel{};
    if (ref) {
        const float w = float(in.time - ref->t);
        rel = ((in.hand_position - ref->hand) - (in.head_position - ref->head)) * (1.0f / w);
    }
    const Vec3 fwd = Normalize(Rotate(in.aim_rotation, {0, 0, -1}), {0, 0, -1});
    // Walking turns and snap turns move the head fast: the game disarms the gate then.
    const float yaw = YawOf(in.head_rotation);
    bool blocked = false;
    if (have_head_prev) {
        float dyaw = std::fabs(yaw - head_yaw_prev);
        dyaw = std::min(dyaw, 2.0f * kPi - dyaw);
        blocked = Length(in.head_position - head_prev) > 0.25f || dyaw > 25.0f * kDeg;
    }
    head_prev = in.head_position;
    head_yaw_prev = yaw;
    have_head_prev = true;

    const float t = Dot(rel, fwd);
    const float sp = Length(rel);
    const float a = sp > 1e-3f ? t / sp : 0.0f;
    const double now_ms = in.time * 1000.0;
    unsigned bits = 0;
    if (t < gate.on) bits |= kRejectSpeed;
    if (a < gate.align) bits |= kRejectAlign;
    if (!(t > gate.prev)) bits |= kRejectFalling;
    if (now_ms - gate.last_ms < gate.debounce_ms) bits |= kRejectDebounce;
    if (!gate.armed) bits |= kRejectDisarmed;
    thrust = t;
    align = a;
    const float rel_a[3] = {rel.x, rel.y, rel.z}, fwd_a[3] = {fwd.x, fwd.y, fwd.z};
    const bool fired = gate.Update(now_ms, rel_a, fwd_a, blocked);
    if (blocked) {
        bits |= kRejectDisarmed;
    }

    // One decision line per jab: the shot, or the frame that came closest to firing.
    if (!attempt && t >= 0.5f * gate.on) {
        attempt = true;
        attempt_fired = false;
        attempt_bits = 0xFFu;
        attempt_peak = 0.0f;
    }
    if (attempt) {
        attempt_peak = std::max(attempt_peak, t);
        if (PopCount(bits) < PopCount(attempt_bits) ||
            (PopCount(bits) == PopCount(attempt_bits) && t > attempt_thrust)) {
            attempt_bits = bits;
            attempt_thrust = t;
            attempt_align = a;
        }
    }
    if (fired) {
        attempt_fired = true;
        Fire(in, c, e, "thrust");
    } else if (c.trigger_fire && in.trigger >= 0.9f && trigger_prev < 0.5f &&
               now_ms - gate.last_ms >= gate.debounce_ms) {
        gate.last_ms = now_ms;
        Fire(in, c, e, "trigger");
    }
    if (attempt && t < gate.rearm) {
        if (!attempt_fired) {
            Say(e, Fmt("jab rejected: %s (best frame: thrust %.2f m/s vs %.2f, align %.2f vs %.2f; peak "
                       "thrust %.2f m/s)",
                       RejectNames(attempt_bits).c_str(), attempt_thrust, gate.on, attempt_align,
                       gate.align, attempt_peak));
        }
        attempt = false;
    }
    trigger_prev = in.trigger;
    last_time = in.time;
}

} // namespace sb
