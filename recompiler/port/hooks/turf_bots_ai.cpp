// turf_bots_ai.cpp - decision making for one Turf War CPU player.
//
// Each bot paints its way along A* paths to spread-out goals, turns to fight
// enemies it can see in range, and swims in squid form now and then to refill
// ink. Ledges on the path are jumped; walls above jump height are inked and
// swum up.
#include "turf_bots_ai.h"
#include <algorithm>
#include <cmath>

namespace turfai {
namespace {
constexpr float kPi = 3.14159265f;
constexpr float kReach = 10.f;        // waypoint reached (XZ distance)
constexpr float kSight = 140.f;       // enemies noticed within this distance
constexpr float kRange = 105.f;       // and shot at within this one
constexpr float kEye = 8.f;           // sight lines from chest height
constexpr float kInkLow = 0.12f, kInkFull = 0.85f; // refill below, back to painting above
constexpr uint64_t kStuckFrames = 45, kGoalFrames = 900;
constexpr uint64_t kPaintMemory = 2400; // painted spots count as fresh again after 40 s
constexpr float kPaintRadius = 20.f;
constexpr uint64_t kChargeCycle = 60;

float dist_xz(const Vec3& a, const Vec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}
float heading_to(const Vec3& from, const Vec3& to) {
    return std::atan2(to.z - from.z, to.x - from.x);
}
Vec3 eye(const Vec3& p) {
    return {p.x, p.y + kEye, p.z};
}
} // namespace

void TeamPaint::reset(size_t nodes) {
    painted_.assign(nodes, 0);
}

// Nodes linked to `center` within the paint radius (breadth-first, bounded).
template <class F> void around(const turfnav::Grid& grid, int center, F&& visit) {
    int queue[64];
    int count = 0;
    queue[count++] = center;
    const Vec3 at = grid.node(center).pos;
    for (int i = 0; i < count; ++i) {
        visit(queue[i]);
        for (int32_t next : grid.node(queue[i]).link)
            if (next >= 0 && count < 64 && dist_xz(grid.node(next).pos, at) < kPaintRadius &&
                std::find(queue, queue + count, next) == queue + count)
                queue[count++] = next;
    }
}

void TeamPaint::mark(const turfnav::Grid& grid, const Vec3& at, uint64_t frame) {
    const int center = grid.nearest(at);
    if (center < 0 || size_t(center) >= painted_.size()) return;
    around(grid, center, [&](int n) { painted_[size_t(n)] = frame; });
}

float TeamPaint::fresh_share(const turfnav::Grid& grid, int node, uint64_t frame) const {
    if (node < 0 || size_t(node) >= painted_.size()) return 0.f;
    int fresh = 0, total = 0;
    around(grid, node, [&](int n) {
        ++total;
        fresh += painted_[size_t(n)] == 0 || frame - painted_[size_t(n)] > kPaintMemory;
    });
    return total ? float(fresh) / float(total) : 0.f;
}

float wrap_angle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

void move_stick(float heading, float aim, float& lx, float& ly) {
    const float rel = wrap_angle(heading - aim);
    lx = std::sin(rel);
    ly = std::cos(rel);
}

float turn_stick(float target, float aim) {
    // Proportional turn; full stick turns roughly three degrees per frame.
    return std::clamp(wrap_angle(target - aim) * 1.6f, -1.f, 1.f);
}

void Brain::reset() {
    path_.clear();
    next_ = 0;
    has_last_ = false;
    climb_until_ = jump_until_ = 0;
}

bool Brain::pick_goal(int self, const Snapshot& world, const turfnav::Grid& grid, const TeamPaint& paint,
                      uint64_t frame) {
    const PlayerView& me = world[size_t(self)];
    const int start = grid.nearest(me.pos);
    if (start < 0) return false;
    // Sample spots; prefer ones away from teammates (spread the paint) and,
    // sometimes, ones near enemies (contest their turf).
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    const bool aggressive = unit(rng_) < 0.35f;
    int best = -1;
    float best_score = -1e9f;
    for (int i = 0; i < 24; ++i) {
        const int n = grid.random_node(rng_);
        if (n < 0) break;
        const Vec3& p = grid.node(n).pos;
        if (dist_xz(p, me.pos) < 60.f) continue;
        float mate = 1e9f, enemy = 1e9f;
        for (size_t k = 0; k < world.size(); ++k) {
            if (int(k) == self || !world[k].valid) continue;
            const float d = dist_xz(p, world[k].pos);
            (world[k].team == me.team ? mate : enemy) = std::min(world[k].team == me.team ? mate : enemy, d);
        }
        // Unpainted turf first, spread out from teammates, not too far away.
        float score = paint.fresh_share(grid, n, frame) * 400.f + std::min(mate, 250.f) * 0.6f -
                      dist_xz(p, me.pos) * 0.15f + unit(rng_) * 60.f;
        if (aggressive) score -= std::min(enemy, 600.f) * 0.5f;
        if (score > best_score) {
            best_score = score;
            best = n;
        }
    }
    std::vector<int> route;
    if (best < 0 || !grid.path(start, best, route) || route.size() < 2) return false;
    path_ = std::move(route);
    next_ = 1;
    return true;
}

int Brain::target_enemy(int self, const Snapshot& world, const turfnav::Grid& grid) const {
    const PlayerView& me = world[size_t(self)];
    int best = -1;
    float best_d = kSight;
    for (size_t k = 0; k < world.size(); ++k) {
        const PlayerView& other = world[k];
        if (int(k) == self || !other.valid || other.team == me.team) continue;
        const float d = dist_xz(me.pos, other.pos);
        if (d < best_d && std::fabs(other.pos.y - me.pos.y) < 60.f && grid.visible(eye(me.pos), eye(other.pos))) {
            best_d = d;
            best = int(k);
        }
    }
    return best;
}

Pad Brain::think(int self, const Snapshot& world, turfnav::Grid& grid, TeamPaint& paint, uint64_t frame) {
    Pad pad;
    const PlayerView& me = world[size_t(self)];
    if (!me.valid || grid.size() == 0) return pad;

    // Respawn or knock-back: a jump in position invalidates the route.
    if (has_last_ && dist_xz(me.pos, last_pos_) > 40.f) reset();
    last_pos_ = me.pos;
    has_last_ = true;

    // Route upkeep: new goal when finished or stale.
    if (next_ >= path_.size() || frame - goal_frame_ > kGoalFrames) {
        path_.clear();
        goal_frame_ = frame;
        progress_frame_ = frame;
        best_dist_ = 1e9f;
        stuck_tries_ = 0;
        pick_goal(self, world, grid, paint, frame);
    }
    while (next_ < path_.size() && dist_xz(me.pos, grid.node(path_[next_]).pos) < kReach) {
        ++next_;
        best_dist_ = 1e9f;
        stuck_tries_ = 0;
        progress_frame_ = frame;
    }
    // Progress means getting closer to the current waypoint, not just moving.
    if (next_ < path_.size()) {
        const float d = dist_xz(me.pos, grid.node(path_[next_]).pos);
        if (d < best_dist_ - 1.5f) {
            best_dist_ = d;
            progress_frame_ = frame;
        }
        if (frame - progress_frame_ > kStuckFrames) {
            progress_frame_ = frame;
            if (++stuck_tries_ >= 3) {
                // Hops did not help: something the collision does not show is in the way.
                grid.unlink(path_[next_ - 1], path_[next_]);
                next_ = path_.size();
            } else
                jump_until_ = frame + 10;
        }
    }

    float heading = me.aim;
    bool moving = false;
    uint8_t link = 0;
    if (next_ < path_.size()) {
        const auto& from = grid.node(path_[next_ - 1]);
        for (size_t d = 0; d < from.link.size(); ++d)
            if (from.link[d] == path_[next_]) link = from.jump[d];
        // Look a few nodes ahead for a smoother line.
        const size_t ahead = std::min(path_.size() - 1, next_ + 2);
        heading = heading_to(me.pos, grid.node(path_[link ? next_ : ahead]).pos);
        moving = true;
    }

    // Ledges: a short jump; walls: ink them, then swim up.
    if (moving && link == turfnav::kJumpLink && dist_xz(me.pos, grid.node(path_[next_]).pos) < 16.f)
        jump_until_ = frame + 8;
    // Climb only once at the foot of the wall.
    if (moving && link == turfnav::kClimbLink && climb_until_ < frame &&
        dist_xz(me.pos, grid.node(path_[next_]).pos) < turfnav::Grid::kCell * 1.3f)
        climb_until_ = frame + 130;

    const int enemy = target_enemy(self, world, grid);
    float aim_target = heading;
    bool fire = true;
    if (enemy >= 0) {
        const PlayerView& e = world[size_t(enemy)];
        aim_target = heading_to(me.pos, e.pos);
        fire = std::fabs(wrap_angle(aim_target - me.aim)) < 0.3f && dist_xz(me.pos, e.pos) < kRange;
        // Close in while fighting.
        if (dist_xz(me.pos, e.pos) > kRange * 0.7f) {
            heading = aim_target;
            moving = true;
        }
    }

    // Ink: paint until the tank runs low, then swim until it is nearly full.
    if (!refilling_ && me.ink < kInkLow) refilling_ = true;
    if (refilling_ && me.ink > kInkFull) refilling_ = false;
    bool squid = refilling_;
    if (refilling_) fire = false;
    if (frame < climb_until_) {
        // First ink the wall, then swim up it.
        aim_target = heading;
        const bool inking = climb_until_ - frame > 70;
        fire = inking;
        squid = !inking;
    }

    pad.rx = turn_stick(aim_target, me.aim);
    if (moving) move_stick(heading, me.aim, pad.lx, pad.ly);
    if (squid)
        pad.buttons |= kPadZL;
    else if (fire) {
        // A charger holds for a full charge, then lets go to shoot.
        if (weapon_ != WeaponKind::Charger || frame % kChargeCycle < kChargeCycle - 6) pad.buttons |= kPadZR;
        // Shots land some way ahead of the aim.
        const Vec3 ahead{me.pos.x + std::cos(me.aim) * 35.f, me.pos.y, me.pos.z + std::sin(me.aim) * 35.f};
        paint.mark(grid, me.pos, frame);
        paint.mark(grid, ahead, frame);
    }
    if (frame < jump_until_) pad.buttons |= kPadB;
    // Sub weapon now and then at close enemies.
    if (enemy >= 0 && dist_xz(me.pos, world[size_t(enemy)].pos) < 80.f && frame % 300 < 3) pad.buttons |= kPadR;
    return pad;
}

} // namespace turfai
