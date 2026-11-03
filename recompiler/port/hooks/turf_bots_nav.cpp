// turf_bots_nav.cpp - stage navigation for the Turf War CPU players.
#include "turf_bots_nav.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <queue>

namespace turfnav {
namespace {
uint16_t be16(const uint8_t* p) {
    return uint16_t(p[0] << 8 | p[1]);
}
uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float bef(const uint8_t* p) {
    const uint32_t bits = be32(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 add_scaled(const Vec3& a, const Vec3& d, float s) {
    return {a.x + d.x * s, a.y + d.y * s, a.z + d.z * s};
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
float dist(const Vec3& a, const Vec3& b) {
    const Vec3 d = sub(a, b);
    return std::sqrt(dot(d, d));
}
// Segment p->q against a triangle (Moller-Trumbore).
bool segment_hits(const Vec3& p, const Vec3& q, const Triangle& t) {
    const Vec3 d = sub(q, p), e1 = sub(t.b, t.a), e2 = sub(t.c, t.a);
    const Vec3 h = cross(d, e2);
    const float det = dot(e1, h);
    if (std::fabs(det) < 1e-6f) return false;
    const float inv = 1.f / det;
    const Vec3 s = sub(p, t.a);
    const float u = dot(s, h) * inv;
    if (u < 0.f || u > 1.f) return false;
    const Vec3 qv = cross(s, e1);
    const float v = dot(d, qv) * inv;
    if (v < 0.f || u + v > 1.f) return false;
    const float k = dot(e2, qv) * inv;
    return k > 0.f && k < 1.f;
}
// Height of the triangle's plane at (x, z) when (x, z) lies inside its XZ projection.
bool height_at(const Triangle& t, float x, float z, float& y) {
    const float d1 = (x - t.b.x) * (t.a.z - t.b.z) - (t.a.x - t.b.x) * (z - t.b.z);
    const float d2 = (x - t.c.x) * (t.b.z - t.c.z) - (t.b.x - t.c.x) * (z - t.c.z);
    const float d3 = (x - t.a.x) * (t.c.z - t.a.z) - (t.c.x - t.a.x) * (z - t.a.z);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    if (neg && pos) return false;
    if (std::fabs(t.normal.y) < 1e-4f) return false;
    y = t.a.y - (t.normal.x * (x - t.a.x) + t.normal.z * (z - t.a.z)) / t.normal.y;
    return true;
}
constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDz[8] = {0, 1, 1, 1, 0, -1, -1, -1};
} // namespace

std::vector<uint8_t> yaz0(const std::vector<uint8_t>& d) {
    if (d.size() < 16 || std::memcmp(d.data(), "Yaz0", 4) != 0) return d;
    const uint32_t size = be32(d.data() + 4);
    std::vector<uint8_t> out;
    out.reserve(size);
    size_t i = 16;
    while (out.size() < size && i < d.size()) {
        const uint8_t code = d[i++];
        for (int bit = 0; bit < 8 && out.size() < size && i < d.size(); ++bit) {
            if (code & (0x80 >> bit)) {
                out.push_back(d[i++]);
                continue;
            }
            if (i + 1 >= d.size()) return {};
            const uint8_t b1 = d[i], b2 = d[i + 1];
            i += 2;
            const size_t back = size_t((b1 & 0xF) << 8 | b2) + 1;
            size_t n = b1 >> 4;
            if (n == 0) {
                if (i >= d.size()) return {};
                n = size_t(d[i++]) + 0x12;
            } else
                n += 2;
            if (back > out.size()) return {};
            for (size_t k = 0; k < n; ++k) out.push_back(out[out.size() - back]);
        }
    }
    return out.size() == size ? out : std::vector<uint8_t>{};
}

bool sarc_file(const std::vector<uint8_t>& archive, std::string_view name, std::vector<uint8_t>& out) {
    const std::vector<uint8_t> d = yaz0(archive);
    if (d.size() < 0x20 || std::memcmp(d.data(), "SARC", 4) != 0 || d[6] != 0xFE) return false; // big-endian only
    const uint32_t data_off = be32(d.data() + 0xC);
    const size_t sfat = 0x14;
    const uint16_t count = be16(d.data() + sfat + 6);
    const size_t names = sfat + 12 + size_t(count) * 16 + 8;
    if (names > d.size()) return false;
    for (uint16_t k = 0; k < count; ++k) {
        const uint8_t* e = d.data() + sfat + 12 + size_t(k) * 16;
        const uint32_t attr = be32(e + 4), start = be32(e + 8), end = be32(e + 12);
        if (!(attr & 0x01000000u)) continue;
        const size_t at = names + size_t(attr & 0xFFFF) * 4;
        if (at >= d.size()) continue;
        const char* s = reinterpret_cast<const char*>(d.data() + at);
        const size_t len = strnlen(s, d.size() - at);
        if (std::string_view(s, len) != name) continue;
        if (size_t(data_off) + end > d.size() || start > end) return false;
        out.assign(d.begin() + data_off + start, d.begin() + data_off + end);
        return true;
    }
    return false;
}

bool parse_kcl(const std::vector<uint8_t>& d, std::vector<Triangle>& out) {
    if (d.size() < 0x38 || be32(d.data()) != 0x02020000u) return false;
    const uint32_t models_off = be32(d.data() + 8), models = be32(d.data() + 12);
    for (uint32_t m = 0; m < models; ++m) {
        if (size_t(models_off) + 4 * (m + 1) > d.size()) return false;
        const uint32_t mo = be32(d.data() + models_off + 4 * m);
        if (size_t(mo) + 16 > d.size()) return false;
        const uint32_t pos = mo + be32(d.data() + mo), nrm = mo + be32(d.data() + mo + 4);
        const uint32_t tri = mo + be32(d.data() + mo + 8), end = mo + be32(d.data() + mo + 12);
        if (end > d.size() || tri > end || pos > d.size() || nrm > d.size()) return false;
        auto vec = [&](uint32_t base, uint32_t i, Vec3& v) {
            const size_t at = size_t(base) + 12 * size_t(i);
            if (at + 12 > d.size()) return false;
            v = {bef(&d[at]), bef(&d[at + 4]), bef(&d[at + 8])};
            return true;
        };
        for (uint32_t o = tri; o + 20 <= end; o += 20) { // 0x14-byte records in v2
            const float length = bef(&d[o]);
            Vec3 p, fn, ea, eb, ec;
            if (!vec(pos, be16(&d[o + 4]), p) || !vec(nrm, be16(&d[o + 6]), fn) || !vec(nrm, be16(&d[o + 8]), ea) ||
                !vec(nrm, be16(&d[o + 10]), eb) || !vec(nrm, be16(&d[o + 12]), ec))
                return false;
            const Vec3 ca = cross(ea, fn), cb = cross(eb, fn);
            const float da = dot(ca, ec), db = dot(cb, ec);
            if (std::fabs(da) < 1e-6f || std::fabs(db) < 1e-6f || !std::isfinite(length)) continue; // degenerate
            out.push_back({p, add_scaled(p, cb, length / db), add_scaled(p, ca, length / da), fn, be16(&d[o + 14])});
        }
    }
    return !out.empty();
}

int Grid::cell_of(float x, float z) const {
    const int cx = int(std::floor((x - x0_) / kCell)), cz = int(std::floor((z - z0_) / kCell));
    return cx < 0 || cz < 0 || cx >= w_ || cz >= h_ ? -1 : cz * w_ + cx;
}

bool Grid::build(const std::vector<Triangle>& triangles) {
    nodes_.clear();
    cells_.clear();
    float x1 = -std::numeric_limits<float>::max(), z1 = x1;
    x0_ = z0_ = std::numeric_limits<float>::max();
    for (const auto& t : triangles)
        for (const Vec3* v : {&t.a, &t.b, &t.c}) {
            x0_ = std::min(x0_, v->x);
            z0_ = std::min(z0_, v->z);
            x1 = std::max(x1, v->x);
            z1 = std::max(z1, v->z);
        }
    if (!(x1 > x0_ && z1 > z0_)) return false;
    w_ = int((x1 - x0_) / kCell) + 1;
    h_ = int((z1 - z0_) / kCell) + 1;
    if (int64_t(w_) * h_ > 4'000'000) return false;
    cells_.assign(size_t(w_) * h_, {});

    // Floors and ceilings per cell, and wall triangles bucketed by cell.
    std::vector<std::vector<float>> floors(cells_.size()), ceilings(cells_.size());
    std::vector<std::vector<int32_t>> walls(cells_.size());
    solid_.clear();
    solid_cells_.assign(cells_.size(), {});
    for (size_t i = 0; i < triangles.size(); ++i) {
        const Triangle& t = triangles[i];
        const int32_t solid = int32_t(solid_.size());
        solid_.push_back(t);
        const float lo_x = std::min({t.a.x, t.b.x, t.c.x}), hi_x = std::max({t.a.x, t.b.x, t.c.x});
        const float lo_z = std::min({t.a.z, t.b.z, t.c.z}), hi_z = std::max({t.a.z, t.b.z, t.c.z});
        const int cx0 = std::max(0, int((lo_x - x0_) / kCell)), cx1 = std::min(w_ - 1, int((hi_x - x0_) / kCell));
        const int cz0 = std::max(0, int((lo_z - z0_) / kCell)), cz1 = std::min(h_ - 1, int((hi_z - z0_) / kCell));
        const bool floor = t.normal.y >= 0.6f, ceiling = t.normal.y <= -0.6f;
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cx = cx0; cx <= cx1; ++cx) {
                const size_t c = size_t(cz) * w_ + cx;
                solid_cells_[c].push_back(solid);
                if (!floor && !ceiling) {
                    walls[c].push_back(int32_t(i));
                    continue;
                }
                float y;
                if (!height_at(t, x0_ + (cx + .5f) * kCell, z0_ + (cz + .5f) * kCell, y)) continue;
                (floor ? floors : ceilings)[c].push_back(y);
            }
    }
    for (size_t c = 0; c < cells_.size(); ++c) {
        auto& f = floors[c];
        std::sort(f.begin(), f.end());
        float last = -std::numeric_limits<float>::max();
        for (float y : f) {
            if (y - last < 3.f) continue; // same layer
            last = y;
            bool blocked = false;
            for (float ceil : ceilings[c]) blocked |= ceil > y + 0.5f && ceil < y + kHeadroom;
            for (float upper : f) blocked |= upper > y + 3.f && upper < y + kHeadroom;
            if (blocked) continue;
            Node n;
            n.pos = {x0_ + (float(c % w_) + .5f) * kCell, y, z0_ + (float(c / w_) + .5f) * kCell};
            cells_[c].push_back(int32_t(nodes_.size()));
            nodes_.push_back(n);
        }
    }
    // Links: the neighbour layer closest in height that is climbable or a drop,
    // with no wall across the step at knee and head height.
    for (size_t c = 0; c < cells_.size(); ++c) {
        const int cx = int(c % w_), cz = int(c / w_);
        for (int32_t a : cells_[c])
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cx + kDx[dir], nz = cz + kDz[dir];
                if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
                const size_t nc = size_t(nz) * w_ + nx;
                Node& from = nodes_[size_t(a)];
                const float step = kSlope * kCell * ((kDx[dir] && kDz[dir]) ? 1.41421f : 1.f);
                int32_t best = -1;
                float best_dy = 0;
                for (int32_t b : cells_[nc]) {
                    const float dy = nodes_[size_t(b)].pos.y - from.pos.y;
                    if (dy > kClimb || dy < -kDrop) continue;
                    if (best < 0 || std::fabs(dy) < std::fabs(best_dy)) {
                        best = b;
                        best_dy = dy;
                    }
                }
                if (best < 0) continue;
                const Vec3& to = nodes_[size_t(best)].pos;
                bool wall = false;
                for (const float h : {4.f, 10.f}) {
                    const Vec3 p{from.pos.x, std::max(from.pos.y, to.y) + h, from.pos.z};
                    const Vec3 q{to.x, p.y, to.z};
                    for (size_t wc : {c, nc})
                        for (int32_t t : walls[wc]) {
                            if (segment_hits(p, q, triangles[size_t(t)])) {
                                wall = true;
                                break;
                            }
                        }
                    if (wall) break;
                }
                // Walls are tested above the higher floor, so a ledge's own face never
                // counts; only a climb above jump height goes up a wall.
                if (wall && best_dy <= kJump) continue;
                from.link[size_t(dir)] = best;
                from.jump[size_t(dir)] = best_dy > kJump ? kClimbLink : best_dy > step ? kJumpLink : 0;
            }
    }
    return !nodes_.empty();
}

void Grid::keep_reachable(const std::vector<Vec3>& seeds) {
    std::vector<uint8_t> seen(nodes_.size());
    std::vector<int32_t> stack;
    for (const Vec3& s : seeds)
        if (const int n = nearest(s); n >= 0 && !seen[size_t(n)]) {
            seen[size_t(n)] = 1;
            stack.push_back(n);
        }
    while (!stack.empty()) {
        const int32_t n = stack.back();
        stack.pop_back();
        for (int32_t next : nodes_[size_t(n)].link)
            if (next >= 0 && !seen[size_t(next)]) {
                seen[size_t(next)] = 1;
                stack.push_back(next);
            }
    }
    std::vector<int32_t> remap(nodes_.size(), -1);
    std::vector<Node> kept;
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (seen[i]) {
            remap[i] = int32_t(kept.size());
            kept.push_back(nodes_[i]);
        }
    for (Node& n : kept)
        for (auto& l : n.link) l = l >= 0 ? remap[size_t(l)] : -1;
    for (auto& cell : cells_) {
        std::vector<int32_t> next;
        for (int32_t i : cell)
            if (remap[size_t(i)] >= 0) next.push_back(remap[size_t(i)]);
        cell = std::move(next);
    }
    nodes_ = std::move(kept);
}

int Grid::nearest(const Vec3& p) const {
    const int c = cell_of(p.x, p.z);
    if (c < 0 && nodes_.empty()) return -1;
    const int cx = c >= 0 ? c % w_ : int((p.x - x0_) / kCell), cz = c >= 0 ? c / w_ : int((p.z - z0_) / kCell);
    int best = -1;
    float best_d = std::numeric_limits<float>::max();
    for (int r = 0; r <= 4 && best < 0; ++r)
        for (int z = cz - r; z <= cz + r; ++z)
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || z < 0 || x >= w_ || z >= h_) continue;
                for (int32_t n : cells_[size_t(z) * w_ + x]) {
                    const Vec3& q = nodes_[size_t(n)].pos;
                    // Prefer the layer at or just below the feet.
                    const float dy = p.y - q.y;
                    const float d = dist(p, q) + (dy < -4.f ? 50.f : 0.f);
                    if (d < best_d) {
                        best_d = d;
                        best = n;
                    }
                }
            }
    return best;
}

bool Grid::path(int from, int to, std::vector<int>& out) const {
    out.clear();
    if (from < 0 || to < 0 || size_t(from) >= nodes_.size() || size_t(to) >= nodes_.size()) return false;
    std::vector<float> cost(nodes_.size(), std::numeric_limits<float>::max());
    std::vector<int32_t> prev(nodes_.size(), -1);
    using Item = std::pair<float, int32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    cost[size_t(from)] = 0;
    open.push({dist(nodes_[size_t(from)].pos, nodes_[size_t(to)].pos), from});
    while (!open.empty()) {
        const int32_t n = open.top().second;
        open.pop();
        if (n == to) break;
        const Node& node = nodes_[size_t(n)];
        for (int dir = 0; dir < 8; ++dir) {
            const int32_t next = node.link[size_t(dir)];
            if (next < 0) continue;
            const Node& to_node = nodes_[size_t(next)];
            const int open_sides =
                int(std::count_if(to_node.link.begin(), to_node.link.end(), [](int32_t l) { return l >= 0; }));
            const float edge = open_sides < 8 ? 8.f : 0.f; // keep routes off ledges and walls
            const float c = cost[size_t(n)] + dist(node.pos, to_node.pos) + edge +
                            (node.jump[size_t(dir)] == kClimbLink ? kClimbCost
                             : node.jump[size_t(dir)]             ? 6.f
                                                                  : 0.f);
            if (c >= cost[size_t(next)]) continue;
            cost[size_t(next)] = c;
            prev[size_t(next)] = n;
            open.push({c + dist(nodes_[size_t(next)].pos, nodes_[size_t(to)].pos), next});
        }
    }
    if (prev[size_t(to)] < 0 && from != to) return false;
    for (int32_t n = to; n >= 0; n = prev[size_t(n)]) out.push_back(n);
    std::reverse(out.begin(), out.end());
    return true;
}

bool Grid::visible(const Vec3& a, const Vec3& b) const {
    const float length = dist(a, b);
    const int steps = std::max(1, int(length / (kCell * 0.5f)));
    int last = -1;
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const int c = cell_of(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t);
        if (c < 0 || c == last) continue;
        last = c;
        for (int32_t s : solid_cells_[size_t(c)])
            if (segment_hits(a, b, solid_[size_t(s)])) return false;
    }
    return true;
}

void Grid::unlink(int a, int b) {
    if (a < 0 || size_t(a) >= nodes_.size()) return;
    for (auto& l : nodes_[size_t(a)].link)
        if (l == b) l = -1;
}

int Grid::random_node(std::mt19937& rng) const {
    if (nodes_.empty()) return -1;
    return int(std::uniform_int_distribution<size_t>(0, nodes_.size() - 1)(rng));
}

bool load_stage(const std::string& content_dir, const std::string& model, Grid& grid) {
    std::ifstream file(content_dir + "/Model/" + model + ".szs", std::ios::binary);
    if (!file) return false;
    const std::vector<uint8_t> archive((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> kcl;
    std::vector<Triangle> triangles;
    return sarc_file(archive, model + ".kcl", kcl) && parse_kcl(kcl, triangles) && grid.build(triangles);
}

} // namespace turfnav
