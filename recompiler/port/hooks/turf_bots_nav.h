// turf_bots_nav.h - stage navigation for the Turf War CPU players.
//
// Builds a walkable grid from the stage collision (KCL inside the stage's
// Model/Fld_<name>.szs archive) and finds paths on it. Pure host code: no
// guest memory access, so it is unit-tested with synthetic collision.
#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace turfnav {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// Yaz0 decompression; returns the input unchanged when it is not Yaz0.
std::vector<uint8_t> yaz0(const std::vector<uint8_t>& data);
// Extracts one file from a (Yaz0-compressed) SARC archive.
bool sarc_file(const std::vector<uint8_t>& archive, std::string_view name, std::vector<uint8_t>& out);

struct Triangle {
    Vec3 a, b, c, normal;
    uint16_t flags = 0; // KCL collision attribute
};
// KCL v2 (Wii U, big-endian): every model's triangles in world space.
bool parse_kcl(const std::vector<uint8_t>& kcl, std::vector<Triangle>& out);

constexpr uint8_t kJumpLink = 1, kClimbLink = 2;

// A walkable spot: one floor layer of one grid cell.
struct Node {
    Vec3 pos;
    std::array<int32_t, 8> link{-1, -1, -1, -1, -1, -1, -1, -1}; // neighbour per direction
    std::array<uint8_t, 8> jump{};                               // 0 walk, kJumpLink, kClimbLink
};

class Grid {
public:
    static constexpr float kCell = 12.f;  // grid spacing (player is about 6 units wide)
    static constexpr float kSlope = 0.9f; // walkable climb per unit of horizontal distance (about 42 degrees)
    static constexpr float kJump = 22.f;  // climb per cell with a jump
    static constexpr float kClimb = 60.f; // wall height a squid swims up (after inking it)
    static constexpr float kClimbCost = 600.f; // path cost of a wall climb: only when no ramp leads up
    static constexpr float kDrop = 80.f;  // largest drop per cell
    static constexpr float kHeadroom = 12.f;

    bool build(const std::vector<Triangle>& triangles);
    // Drops every node that cannot be reached from the seeds.
    void keep_reachable(const std::vector<Vec3>& seeds);

    int nearest(const Vec3& p) const; // closest node at most a few cells away, or -1
    bool path(int from, int to, std::vector<int>& out) const;
    int random_node(std::mt19937& rng) const;
    // True when no collision triangle crosses the segment a-b.
    bool visible(const Vec3& a, const Vec3& b) const;
    // Removes the link a->b (learned obstacle, e.g. a stage object).
    void unlink(int a, int b);

    const Node& node(int i) const { return nodes_[size_t(i)]; }
    size_t size() const { return nodes_.size(); }

private:
    int cell_of(float x, float z) const;
    float x0_ = 0, z0_ = 0;
    int w_ = 0, h_ = 0;
    std::vector<Node> nodes_;
    std::vector<std::vector<int32_t>> cells_; // node indices per cell
    std::vector<Triangle> solid_;             // every blocking triangle, for sight lines
    std::vector<std::vector<int32_t>> solid_cells_;
};

// Loads <content>/Model/<model>.szs and builds the grid for that stage.
bool load_stage(const std::string& content_dir, const std::string& model, Grid& grid);

} // namespace turfnav
