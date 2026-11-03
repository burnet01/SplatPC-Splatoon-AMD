// Offline tests for the Turf War CPU players: archive and collision parsing,
// the navigation grid and the bot's steering decisions. No game files.
#include "hooks/turf_bots_ai.h"
#include "hooks/turf_bots_nav.h"
#include "rebrewu/logger.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using turfnav::Triangle;
using turfnav::Vec3;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
bool close_to(float a, float b, float eps = 0.05f) {
    return std::fabs(a - b) <= eps;
}

void put16(std::vector<uint8_t>& d, size_t at, uint16_t v) {
    d[at] = uint8_t(v >> 8);
    d[at + 1] = uint8_t(v);
}
void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + i] = uint8_t(v >> (24 - 8 * i));
}
void putf(std::vector<uint8_t>& d, size_t at, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put32(d, at, bits);
}
Vec3 sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 unit(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return {v.x / l, v.y / l, v.z / l};
}

// KCL v2 with the standard per-triangle encoding (position, face normal,
// three edge normals and the distance along the third edge normal).
std::vector<uint8_t> make_kcl(const std::vector<std::array<Vec3, 3>>& tris) {
    std::vector<Vec3> positions, normals;
    struct Rec {
        float length;
        uint16_t p, n, a, b, c;
    };
    std::vector<Rec> recs;
    for (const auto& t : tris) {
        const Vec3 n = unit(cross(sub(t[1], t[0]), sub(t[2], t[0])));
        const Vec3 e1 = unit(cross(sub(t[0], t[2]), n)), e2 = unit(cross(sub(t[1], t[0]), n));
        const Vec3 e3 = unit(cross(sub(t[2], t[1]), n));
        const auto idx = uint16_t(normals.size());
        normals.insert(normals.end(), {n, e1, e2, e3});
        recs.push_back({dot(sub(t[1], t[0]), e3), uint16_t(positions.size()), idx, uint16_t(idx + 1), uint16_t(idx + 2),
                        uint16_t(idx + 3)});
        positions.push_back(t[0]);
    }
    const size_t model = 0x40, pos = 0x40, nrm = pos + positions.size() * 12, tri = nrm + normals.size() * 12;
    const size_t end = tri + recs.size() * 20;
    std::vector<uint8_t> d(model + end);
    put32(d, 0, 0x02020000u);
    put32(d, 8, 0x38);
    put32(d, 12, 1);
    put32(d, 0x38, uint32_t(model));
    put32(d, model, uint32_t(pos));
    put32(d, model + 4, uint32_t(nrm));
    put32(d, model + 8, uint32_t(tri));
    put32(d, model + 12, uint32_t(end));
    for (size_t i = 0; i < positions.size(); ++i) {
        putf(d, model + pos + 12 * i, positions[i].x);
        putf(d, model + pos + 12 * i + 4, positions[i].y);
        putf(d, model + pos + 12 * i + 8, positions[i].z);
    }
    for (size_t i = 0; i < normals.size(); ++i) {
        putf(d, model + nrm + 12 * i, normals[i].x);
        putf(d, model + nrm + 12 * i + 4, normals[i].y);
        putf(d, model + nrm + 12 * i + 8, normals[i].z);
    }
    for (size_t i = 0; i < recs.size(); ++i) {
        const size_t at = model + tri + 20 * i;
        putf(d, at, recs[i].length);
        put16(d, at + 4, recs[i].p);
        put16(d, at + 6, recs[i].n);
        put16(d, at + 8, recs[i].a);
        put16(d, at + 10, recs[i].b);
        put16(d, at + 12, recs[i].c);
        put16(d, at + 14, 0);
    }
    return d;
}

// A square quad as two triangles (counter-clockwise seen from `up`).
void quad(std::vector<std::array<Vec3, 3>>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    out.push_back({a, b, c});
    out.push_back({a, c, d});
}

// 120 x 120 floor at y = 0 with a 30-unit wall along x = 0 from z = -60 to 20.
std::vector<Triangle> arena() {
    std::vector<std::array<Vec3, 3>> tris;
    quad(tris, {-60, 0, -60}, {-60, 0, 60}, {60, 0, 60}, {60, 0, -60});
    quad(tris, {0, 0, -60}, {0, 30, -60}, {0, 30, 20}, {0, 0, 20});
    quad(tris, {0, 0, 20}, {0, 30, 20}, {0, 30, -60}, {0, 0, -60});
    std::vector<Triangle> out;
    require(turfnav::parse_kcl(make_kcl(tris), out), "synthetic KCL rejected");
    return out;
}

void test_steering() {
    float lx, ly;
    turfai::move_stick(0.5f, 0.5f, lx, ly);
    require(close_to(lx, 0) && close_to(ly, 1), "heading along the aim is straight ahead");
    turfai::move_stick(0.5f + 1.5708f, 0.5f, lx, ly);
    require(close_to(lx, 1) && close_to(ly, 0), "heading 90 degrees counter-clockwise is stick right");
    require(turfai::turn_stick(1.f, 0.f) > 0 && turfai::turn_stick(-1.f, 0.f) < 0, "turn direction");
    require(turfai::turn_stick(3.f, 0.f) == 1.f, "turn is clamped");
    require(close_to(turfai::wrap_angle(7.f), 7.f - 6.2832f, 1e-3f), "angle wrap");
}

void test_archive() {
    // SARC (big-endian) holding "a.kcl" = "HELLO", then Yaz0 with literal blocks.
    std::vector<uint8_t> sarc(0x40 + 8, 0);
    std::memcpy(sarc.data(), "SARC", 4);
    sarc[6] = 0xFE;
    sarc[7] = 0xFF;
    put32(sarc, 0xC, 0x40);
    std::memcpy(sarc.data() + 0x14, "SFAT", 4);
    put16(sarc, 0x14 + 6, 1);
    put32(sarc, 0x20 + 4, 0x01000000u); // name at offset 0
    put32(sarc, 0x20 + 8, 0);
    put32(sarc, 0x20 + 12, 5);
    std::memcpy(sarc.data() + 0x30, "SFNT", 4);
    std::memcpy(sarc.data() + 0x38, "a.kcl", 5);
    std::memcpy(sarc.data() + 0x40, "HELLO", 5);
    std::vector<uint8_t> yaz(16, 0);
    std::memcpy(yaz.data(), "Yaz0", 4);
    put32(yaz, 4, uint32_t(sarc.size()));
    for (size_t i = 0; i < sarc.size(); i += 8) {
        yaz.push_back(0xFF);
        for (size_t k = i; k < i + 8 && k < sarc.size(); ++k) yaz.push_back(sarc[k]);
    }
    require(turfnav::yaz0(yaz) == sarc, "Yaz0 literal stream");
    std::vector<uint8_t> file;
    require(turfnav::sarc_file(yaz, "a.kcl", file) && std::string(file.begin(), file.end()) == "HELLO", "SARC lookup");
    require(!turfnav::sarc_file(yaz, "b.kcl", file), "missing SARC entry");
}

void test_kcl() {
    const auto tris = arena();
    require(tris.size() == 6, "triangle count");
    // The first triangle round-trips its corners.
    require(close_to(tris[0].b.x, -60) && close_to(tris[0].b.z, 60) && close_to(tris[0].c.x, 60) &&
                close_to(tris[0].c.z, 60),
            "triangle corners");
    require(close_to(tris[0].normal.y, 1), "floor normal");
}

void test_grid() {
    turfnav::Grid grid;
    require(grid.build(arena()), "grid build");
    grid.keep_reachable({{-30, 0, 0}});
    require(grid.size() > 50, "floor nodes");
    const int a = grid.nearest({-30, 0, -40}), b = grid.nearest({30, 0, -40});
    std::vector<int> path;
    require(a >= 0 && b >= 0 && grid.path(a, b, path), "path around the wall");
    float max_z = -1e9f;
    for (int n : path) max_z = std::max(max_z, grid.node(n).pos.z);
    require(max_z > 20.f, "the path goes around the wall's end");
    require(!grid.visible({-30, 5, -40}, {30, 5, -40}), "the wall blocks sight");
    require(grid.visible({-30, 5, 40}, {30, 5, 40}), "open sight past the wall");
}

void test_brain() {
    turfnav::Grid grid;
    require(grid.build(arena()), "grid build");
    turfai::Snapshot world;
    world[1] = {true, 0, {-30, 0, 40}, 0.f};
    world[2] = {true, 1, {20, 0, 40}, 0.f}; // visible enemy straight ahead (+x)
    turfai::TeamPaint paint;
    paint.reset(grid.size());
    turfai::Brain brain(7);
    turfai::Pad pad;
    for (uint64_t f = 1; f < 5; ++f) pad = brain.think(1, world, grid, paint, f);
    require(pad.buttons & turfai::kPadZR, "fires at a visible enemy in range");
    require(std::fabs(pad.rx) < 0.1f, "already aiming at it");
    world[2].pos = {-30, 0, 59}; // enemy on the +z side: turn counter-clockwise
    pad = brain.think(1, world, grid, paint, 6);
    require(pad.rx > 0.5f, "turns towards the enemy");
    // Alone: roams and paints.
    turfai::Snapshot alone;
    alone[1] = world[1];
    turfai::Brain roamer(3);
    bool moved = false;
    for (uint64_t f = 1; f < 10; ++f) {
        pad = roamer.think(1, alone, grid, paint, f);
        moved |= std::hypot(pad.lx, pad.ly) > 0.5f;
    }
    require(moved && (pad.buttons & turfai::kPadZR), "roams while painting");
    // Low ink: swims (ZL) instead of firing until the tank is nearly full.
    turfai::Snapshot dry = alone;
    dry[1].ink = 0.05f;
    pad = roamer.think(1, dry, grid, paint, 20);
    require((pad.buttons & turfai::kPadZL) && !(pad.buttons & turfai::kPadZR), "refills when the tank is low");
    dry[1].ink = 0.5f;
    pad = roamer.think(1, dry, grid, paint, 21);
    require(pad.buttons & turfai::kPadZL, "keeps refilling until nearly full");
    dry[1].ink = 0.9f;
    pad = roamer.think(1, dry, grid, paint, 22);
    require(!(pad.buttons & turfai::kPadZL), "paints again once refilled");
    // Painting marks the spot; it counts as fresh again after the memory runs out.
    turfai::TeamPaint fresh;
    fresh.reset(grid.size());
    const int spot = grid.nearest({30, 0, 40});
    require(fresh.fresh_share(grid, spot, 100) == 1.f, "unpainted spot is fresh");
    fresh.mark(grid, grid.node(spot).pos, 100);
    require(fresh.fresh_share(grid, spot, 101) < 0.2f, "painted spot is not fresh");
    require(fresh.fresh_share(grid, spot, 100 + 5000) == 1.f, "paint memory fades");
}
} // namespace

int main() {
    try {
        test_steering();
        test_archive();
        test_kcl();
        test_grid();
        test_brain();
    } catch (const std::exception& e) {
        RBREW_LOG_ERROR("test", "FAIL: %s", e.what());
        return 1;
    }
    std::puts("PASS: archive, KCL, grid paths and sight, bot steering and fire decisions");
    return 0;
}
