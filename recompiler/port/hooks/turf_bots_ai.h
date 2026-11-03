// turf_bots_ai.h - decision making for one Turf War CPU player.
//
// Pure host logic: it reads a snapshot of the match and returns controller
// input, so it is unit-tested without the game. Directions are angles in the
// XZ plane, atan2(z, x), the convention of the game's aim vector.
#pragma once
#include "turf_bots_nav.h"
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace turfai {

using turfnav::Vec3;

// sead pad bits as the game's controller wrapper reads them.
constexpr uint32_t kPadB = 0x2, kPadZL = 0x4, kPadZR = 0x20, kPadR = 0x10;

struct Pad {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};

struct PlayerView {
    bool valid = false;
    int team = 0;
    Vec3 pos;
    float aim = 0; // atan2(z, x) of the aim vector
    float ink = 1; // ink tank, 0..1
};
using Snapshot = std::array<PlayerView, 8>;

// What one team painted recently, per grid node (frame of the last paint).
class TeamPaint {
public:
    void reset(size_t nodes);
    // Paint around `at` while firing.
    void mark(const turfnav::Grid& grid, const Vec3& at, uint64_t frame);
    // Share (0..1) of the spots around `node` not painted in the last `memory` frames.
    float fresh_share(const turfnav::Grid& grid, int node, uint64_t frame) const;

private:
    std::vector<uint64_t> painted_;
};

// How the main weapon is used: shooters and rollers hold ZR, chargers charge
// with ZR held and fire on release.
enum class WeaponKind { Shooter, Roller, Charger };

class Brain {
public:
    explicit Brain(uint32_t seed = 1, WeaponKind weapon = WeaponKind::Shooter) : rng_(seed), weapon_(weapon) {}
    // One frame of input for player `self`; `frame` counts game frames.
    // Links the bot cannot pass are removed from the shared grid.
    Pad think(int self, const Snapshot& world, turfnav::Grid& grid, TeamPaint& paint, uint64_t frame);
    void reset();

private:
    bool pick_goal(int self, const Snapshot& world, const turfnav::Grid& grid, const TeamPaint& paint, uint64_t frame);
    int target_enemy(int self, const Snapshot& world, const turfnav::Grid& grid) const;

    std::mt19937 rng_;
    WeaponKind weapon_;
    std::vector<int> path_;
    size_t next_ = 0;
    Vec3 last_pos_;
    uint64_t progress_frame_ = 0, goal_frame_ = 0;
    bool refilling_ = false; // swimming to refill the ink tank
    uint64_t climb_until_ = 0, jump_until_ = 0;
    float best_dist_ = 1e9f; // closest approach to the current waypoint
    int stuck_tries_ = 0;
    bool has_last_ = false;
};

// Steering helpers (exposed for tests).
float wrap_angle(float a);
// Left stick that moves along `heading` while aiming along `aim`.
void move_stick(float heading, float aim, float& lx, float& ly);
// Right stick turning `aim` towards `target`.
float turn_stick(float target, float aim);

} // namespace turfai
