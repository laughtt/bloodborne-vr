// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Solid weapons for sandbox3d (pure CPU, unit tested by collision_test.cpp), a simplified
// Boneworks-style "physics hand":
//   - each held weapon gets collision capsules built from its mesh (BuildWeaponShape: the
//     vertices of each bone sliced along their long axis, wide flat slices split into lanes), far
//     tighter than the game's attack capsules (median radius 0.40 m, doc 14);
//   - the world has static colliders: the floor, a wooden training post with a crossbar, a wall,
//     and an enemy (default c2632, the Central Yharnam Huntsman) whose hurtbox capsules come
//     from hitbox/chr_<chr>.json, standing at its bind pose;
//   - every frame the weapon follows where the hand puts it in substeps short enough that nothing
//     tunnels, and is pushed out of whatever it overlaps (position-based rigid body: the hand's
//     mass sits at the grip, the weapon's along its capsules, so a hit near the tip mostly turns
//     the weapon); the two weapons push each other. The scene then moves the hands onto the
//     weapons (CorrectHands) and shows a ghost of the real controller hand;
//   - contacts give haptic pulses scaled by the impact speed along the normal and a light buzz
//     while grinding; the shown attack's hit capsules entering the enemy's hurtboxes during a
//     fast swing give a strong "hit" pulse, a hit marker and a log line (doc 14 option C/D);
//   - every contact start/end, hit and snap is printed and appended to a JSON-lines log.
// Settings: PhysicsConfig (the "physics" object of sandbox3d.json, hot reloaded).

#pragma once

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "collision.h"
#include "model.h"
#include "physics_config.h"

namespace sb {

class Rig;
struct RigInput;

/// A weapon's collision capsules in its model space (the grip is the origin).
struct WeaponShape {
    std::vector<col::Capsule> capsules;
    m3::Vec3 center;      // bounding sphere of the capsules
    float bound = 0.0f;
    float reach = 0.0f;   // the farthest capsule surface point from the grip
    m3::Vec3 tip;         // the capsule end farthest from the grip
    float min_radius = 0.0f, median_radius = 0.0f;
    // Hand (a point mass at the grip) + weapon (spread over the capsules): unscaled model space.
    float mass = 1.0f;
    m3::Vec3 com;
    col::Mat3 inv_inertia; // about com
};

/// The collision capsules of a weapon from its mesh (see PhysicsConfig's shape settings).
WeaponShape BuildWeaponShape(const Model& weapon, const PhysicsConfig& config);

/// (Body-sourced shapes (kicks) are kept when `body_point` is given: it maps a body ShapePoint to
/// the weapon's model space as the scene draws it (bone pose * local), so the physics tests the
/// shapes where they are shown.)
/// The weapon-sourced hit shapes of the attack a hand shows (the scene's choice: `pick` indexes
/// the attacks of `form`, -1 = all of them, each shape once); `name` gets the attack's name.
std::vector<HitShape> AttackShapesFor(
    const Hitboxes& hitboxes, const std::string& form, int pick, std::string* name = nullptr,
    const std::function<m3::Vec3(const ShapePoint&)>& body_point = nullptr);

/// What a hand holds this frame.
struct HeldWeapon {
    const Model* model = nullptr; // null: empty hand
    std::string name;
    std::vector<HitShape> attack; // weapon model space (AttackShapesFor)
    std::string attack_name;
    m3::Mat4 target;              // where the hand puts the weapon (rig, alignment, grip offset)
};

struct PhysicsEvents {
    std::string log;
    float haptic_amplitude[2] = {}, haptic_seconds[2] = {}; // 0: no vibration this frame
};

enum Entity { kFloor = 0, kPost, kWall, kEnemy, kEntityCount };

struct Collider {
    enum Kind { kCapsule, kBox, kPlane } kind = kCapsule;
    int entity = kFloor;
    int part = -1; // the enemy's hurtbox index
    col::Capsule capsule;
    col::Box box;
    col::Plane plane;
    col::Aabb bounds;
};

class WeaponPhysics {
public:
    struct Hand {
        bool active = false; // holds a weapon
        const Model* model = nullptr;
        std::string name;
        WeaponShape shape;
        std::string shape_key;
        std::vector<col::Capsule> attack; // weapon space, radius scaled by attack_radius_scale
        std::string attack_name;
        bool have_pose = false;
        col::Pose pose, target, target_prev; // the weapon as simulated / as the hand puts it
        bool passthrough = false; // let go after a snap, collides again once free
        float separation = 0.0f, separation_deg = 0.0f;
        float separation_arc = 0.0f; // angle x reach (m): what max_separation_arc_m limits
        std::vector<char> touching; // per capsule, this frame
        // The controller hand before the correction: bone segments in the world.
        std::vector<std::pair<m3::Vec3, m3::Vec3>> ghost;
        m3::Vec3 ghost_wrist;
        // Swing state for hits.
        bool overlap = false, hit_this_swing = false;
        float slow_time = 0.0f, swing_speed = 0.0f;
        // The fastest attack shape (else the tip) as the hand drives it, this frame; `swinging`
        // latches from hit_speed until it falls below rearm_speed (the enemy is not solid then).
        bool swinging = false;
        m3::Vec3 swing_dir;           // the last fast direction (a reversal rearms)
        bool have_swing_dir = false;
        // The raw controller before the last frame (the teleport check).
        bool have_raw = false;
        m3::Vec3 raw_position;
        m3::Quat raw_rotation;
        // Haptics.
        double pulse_until = -1.0, next_buzz = -1.0;
    };
    struct Enemy {
        std::string chr;
        int generation = 0;   // bumped on every (re)load
        bool has_model = false;
        Model model;
        std::vector<col::Capsule> parts; // hurtboxes, model space (bind pose)
        std::vector<std::string> part_names;
        std::vector<int> part_groups;
        m3::Mat4 world;
        bool placed = false;
    };
    struct Props {
        bool valid = false;
        col::Capsule post, crossbar;
        col::Box wall;
    };
    struct Marker {
        m3::Vec3 point;
        double time = 0.0;
        int part = -1, side = 1;
    };

    /// Remembers the assets folder and loads the configured enemy.
    void Load(const std::filesystem::path& assets, const PhysicsConfig& config, std::string& log);
    /// Puts the props around this head pose: on the floor, relative to where it looks.
    void PlaceProps(m3::Vec3 head_position, m3::Quat head_rotation);

    struct StepInput {
        double time = 0.0;
        bool head_valid = false;
        m3::Vec3 head_position;
        m3::Quat head_rotation;
        float floor_y = 0.0f;
        HeldWeapon held[2]; // 0 left, 1 right
        // The raw controller grips (not the IK-clamped hand): the teleport check. Optional.
        bool controller_valid[2] = {};
        m3::Vec3 controller_position[2];
        m3::Quat controller_rotation[2];
        const Rig* rig = nullptr; // posed from the controllers (for the ghost hands)
        const Model* body = nullptr;
    };
    /// One frame: moves the weapons toward their targets, resolves contacts, hits and haptics.
    void Step(const StepInput& in, const PhysicsConfig& config, PhysicsEvents& events);
    /// Moves the hands of `input` by each weapon's displacement from its target (the hand holds
    /// the blocked weapon). True when a hand moved (run the rig again).
    bool CorrectHands(RigInput& input) const;
    /// The simulated weapon of a hand, or `fallback` when it holds none.
    m3::Mat4 WeaponPose(int side, const m3::Mat4& fallback) const;
    /// Forgets poses, contacts and swings (the next Step starts at the targets).
    void Reset();
    /// Test hook: replaces the enemy's hurtboxes (model space) and places it at `world`.
    void SetEnemyParts(const std::vector<col::Capsule>& parts, const m3::Mat4& world);

    const Hand& GetHand(int side) const { return hands[side]; }
    const Enemy& GetEnemy() const { return enemy; }
    const Props& GetProps() const { return props; }
    const std::vector<Collider>& Colliders() const { return colliders; }
    const std::vector<Marker>& Markers() const { return markers; }
    double Now() const { return now; }
    float FloorY() const { return floor_y; }
    /// The deepest penetration left after the last Step's solve (meters; tests).
    float ResidualDepth() const { return residual_depth; }
    int LastSubsteps() const { return last_substeps; }
    int HitCount() const { return hit_count; }
    float LastStepMs() const { return last_step_ms; }

    ~WeaponPhysics();

private:
    // Contact pairs: side * kEntityCount + entity (a hand's weapon against the world), and the
    // two weapons against each other.
    enum { kPairWeapons = 2 * kEntityCount, kPairCount };
    struct Pair {
        bool active = false;
        double start = 0.0, last = 0.0;
        float max_speed = 0.0f, max_slide = 0.0f, max_depth = 0.0f, max_buzz = 0.0f;
        int part = -1;
        m3::Vec3 point;
    };
    struct HapticRequest {
        int priority = 0; // 3 hit, 2 impact, 1 grinding buzz
        float amplitude = 0.0f, seconds = 0.0f;
    };
    struct Touch {
        bool any = false;
        float approach = 0.0f, depth = 0.0f;
        float slide_sum = 0.0f, slide_time = 0.0f;
        int part = -1;
        m3::Vec3 point;
    };
    struct SubTouch {
        bool any = false;
        m3::Vec3 local_a, local_b, normal; // contact points in the weapons' model space
        int side_a = 0, side_b = -1;       // side_b -1: a static collider
    };
    struct Candidate {
        int side, capsule, collider;
    };

    void LoadEnemy(const std::string& chr, std::string& log);
    void BuildWorld(const PhysicsConfig& config);
    m3::Vec3 AnchorPoint(float x, float z) const;
    void CaptureGhost(const Rig& rig, const Model& body);
    col::Capsule CapsuleWorld(int side, size_t index) const;
    col::Capsule CapsuleWorld(int side, const col::Capsule& local) const;
    /// How far the weapon's farthest point from the axis of `turn` is (meters, world).
    float TurnLever(int side, m3::Quat turn) const;
    float InverseMassAt(int side, m3::Vec3 point, m3::Vec3 normal) const;
    void ApplyImpulse(int side, m3::Vec3 point, m3::Vec3 impulse);
    /// One substep's push-out. `prev`: the weapons at the substep's start; `from`/`to`: where
    /// the hands put them at its start and end (the contact speeds).
    void Solve(const PhysicsConfig& config, const col::Pose prev[2], const col::Pose from[2],
               const col::Pose to[2], float h, float step);
    bool Overlaps(int side, const PhysicsConfig& config) const;
    /// The enemy's hurtboxes stop this hand's weapon (not while a swing is in progress).
    bool EnemySolid(const PhysicsConfig& config, int side) const;
    void TestHits(const PhysicsConfig& config, const col::Pose from[2], const col::Pose to[2],
                  float h, PhysicsEvents& events);
    /// Forgets a hand's swing (hit, overlap, slow time, direction): teleport, weapon change.
    void ResetSwing(int side);
    /// The hand's attack probe (the fastest attack shape's center, else the tip) in weapon
    /// space, and its velocity as the targets drive it over `dt`.
    m3::Vec3 SwingProbe(int side, float dt, m3::Vec3& velocity) const;
    void Request(int side, int priority, float amplitude, float seconds);
    void UpdateContacts(const PhysicsConfig& config, PhysicsEvents& events);
    void EndContacts(int side, const PhysicsConfig& config, PhysicsEvents& events, bool quiet);
    std::string PairName(int pair) const;
    void Emit(const PhysicsConfig& config, PhysicsEvents& events, const std::string& line,
              const std::string& json);

    std::filesystem::path assets_dir;
    Hand hands[2];
    Enemy enemy;
    bool enemy_fixed = false; // placed by SetEnemyParts, not by the props
    std::vector<col::Capsule> enemy_world_parts;
    col::Aabb enemy_bounds;
    Props props;
    HapticRequest requests[2];
    std::vector<Collider> colliders;
    std::vector<Marker> markers;
    Pair pairs[kPairCount];
    Touch touches[kPairCount];
    SubTouch sub[kPairCount];
    std::vector<Candidate> candidates;
    std::vector<std::pair<int, int>> weapon_candidates;
    bool simulating = false; // physics on in the last Step (else weapons are where hands put them)
    bool anchored = false;
    m3::Vec3 anchor;
    float anchor_yaw = 0.0f;
    float floor_y = 0.0f;
    double now = 0.0, last_time = 0.0;
    bool have_time = false;
    float residual_depth = 0.0f;
    int last_substeps = 0, hit_count = 0;
    float last_step_ms = 0.0f;
    // A summary line every 5 s while a weapon is held (cost, substeps, contacts, hits).
    double stats_start = -1.0, stats_last = -1.0, stats_ms = 0.0, stats_ms_max = 0.0;
    int stats_frames = 0, stats_substeps = 0, stats_contacts = 0, stats_hits = 0;
    const Model* ghost_body = nullptr;
    int ghost_bones[2][16];
    std::FILE* log_file = nullptr;
    std::string log_path;
};

} // namespace sb
