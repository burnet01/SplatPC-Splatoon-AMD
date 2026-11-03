// turf_bots.cpp - offline Turf War with CPU players (Splatoon AGMP01 v16).
//
// A bot match is the game's own VSGame scene, started offline on a Turf War
// stage. The game builds its players from eight PlayerInfo slots and offline
// scenes enable only slot 0; while a bot match starts, the per-scene slot
// setup (0216F8E0, manifest hook) also enables slots 1-7. Those players are
// switched to local control (025A90A0, as the game does for local players)
// and the controller getter (025A96A4, manifest hook) hands each its own
// controller: a copy of the real one whose button and stick fields are
// written every frame from turf_bots_ai (turf_bots_tick, from VPADRead on
// the game thread).
//
// Layouts reverse-engineered from the v16 executable; names are descriptive.
//   MatchInfo:         +0x10 PlayerInfo*[8], +0x1C next stage file name
//   PlayerInfo (0xFC): +0x04 valid (u8), +0x30 team
//   Game::Player:      +0x2C team, +0x58 remote (0 = local), +0x98 index,
//                      +0x198 aim, +0x244 position
//   Controller wrapper: +0x00 sead controller*, +0x04 action -> button masks[32]
//   sead controller:   +0x00 trigger, +0x04 release, +0x08 hold, +0x0C hold
//                      frames[32], +0x98 left stick, +0xA0 right stick
#include "turf_bots.h"
#include "../runtime/game_revision.h"
#include "turf_bots_ai.h"
#include "turf_bots_nav.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <random>
#include <string>

namespace {
// Opt-in trace (RBREW_BOTS_LOG=<file>); release builds compile the logger out.
void trace(const char* format, ...) {
    static std::FILE* file = [] {
        const char* path = std::getenv("RBREW_BOTS_LOG");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    if (!file) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

// --- Game addresses (v16) -----------------------------------------------------
constexpr uint32_t kSceneMgr = 0x101C9578u;        // +0x15C current, +0x160 requested ID, +0x168 request
constexpr uint32_t kSceneKinds = 0x101C0F28u;      // *(this)+0x18: kind per scene ID
constexpr uint32_t kMatchInfo = 0x101C1358u;       // MatchInfo*
constexpr uint32_t kCtrlMgr = 0x101C1D34u;         // +0x15C controller wrapper ring
constexpr uint32_t kMainMgr = 0x101C6000u;         // +0x268 PlayerMgr
constexpr uint32_t kPlayerActorType = 0x101C7228u; // actor type index of Game::Player
constexpr uint32_t kPlayerVtable = 0x100D65CCu;
constexpr uint32_t kSeqVtable = 0x100E5FF4u;     // SeqMgrVSGame (constructor 026A5D00)
constexpr uint32_t kSeqState = 0x254;            // u8 current state of its state machine
constexpr int kSeqRewards = 11;                  // last state: the rewards screen (online it waits for peers)
constexpr uint32_t kPadA = 0x1;                  // sead pad bit
constexpr uint32_t kInfoSetup = 0x0216F8E0u;     // (infos, &kind, ...): enables the slots a scene uses
constexpr uint32_t kInitInfo = 0x0216EFB4u;      // (info, team offset): defaults, valid, team
constexpr uint32_t kGetController = 0x025A96A4u; // (player) -> controller wrapper
constexpr uint32_t kMakeWeapon = 0x02171158u;    // (record, set ID): {set, main, sub, special, 0}
constexpr uint32_t kInfoWeapon = 0x40, kWeaponRecord = 0x14;
constexpr uint32_t kSetLocal = 0x025A90A0u;      // (player, local, ...)
constexpr int kKindVSGame = 0, kKindPlaza = 2;   // scene kind table (0 VSGame, 1 Mission, 2 Plaza ...)
constexpr uint32_t kSceneCurrent = 0x15C, kSceneRequest = 0x160, kSceneRequested = 0x168;
constexpr uint32_t kInfoValid = 0x04, kInfoTeam = 0x30;
constexpr uint32_t kPlayerTeam = 0x2C, kPlayerRemote = 0x58, kPlayerIndex = 0x98;
constexpr uint32_t kPlayerAim = 0x198, kPlayerPos = 0x244;
constexpr uint32_t kPlayerTank = 0x6A0, kTankInk = 0x1C; // ink tank object (025F6B34); ink 0..1
constexpr uint32_t kWrapperSize = 0x100, kInnerSize = 0x400;
constexpr uint32_t kTrigger = 0x00, kRelease = 0x04, kHold = 0x08, kHoldFrames = 0x0C;
constexpr uint32_t kLeftStick = 0x98, kRightStick = 0xA0;

uint32_t rd(uint8_t* mem, uint32_t a) {
    return rbrew_memory_accessible(mem, a, 4) ? rbrew_read32(mem, a) : 0;
}
float rf(uint8_t* mem, uint32_t a) {
    const uint32_t bits = rd(mem, a);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
void wf(uint8_t* mem, uint32_t a, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    rbrew_write32(mem, a, bits);
}

// Runs a guest function with integer arguments (r3...) and restores every
// register the interrupted caller can observe.
uint32_t call_guest(CPUState* cpu, uint32_t address, std::initializer_list<uint32_t> args) {
    struct Saved {
        uint32_t r[32], cr[8], gqr[8], lr, ctr, xer, fpscr;
        double f[32], ps1[32];
    } saved;
    std::memcpy(saved.r, cpu->r, sizeof saved.r);
    std::memcpy(saved.cr, cpu->cr, sizeof saved.cr);
    std::memcpy(saved.f, cpu->f, sizeof saved.f);
    std::memcpy(saved.ps1, cpu->ps1, sizeof saved.ps1);
    std::memcpy(saved.gqr, cpu->gqr, sizeof saved.gqr);
    saved.lr = cpu->lr;
    saved.ctr = cpu->ctr;
    saved.xer = cpu->xer;
    saved.fpscr = cpu->fpscr;
    unsigned n = 3;
    for (uint32_t value : args) cpu->r[n++] = value;
    cpu->lr = 0;
    rbrew_call(cpu, address);
    const uint32_t result = cpu->r[3];
    std::memcpy(cpu->r, saved.r, sizeof saved.r);
    std::memcpy(cpu->cr, saved.cr, sizeof saved.cr);
    std::memcpy(cpu->f, saved.f, sizeof saved.f);
    std::memcpy(cpu->ps1, saved.ps1, sizeof saved.ps1);
    std::memcpy(cpu->gqr, saved.gqr, sizeof saved.gqr);
    cpu->lr = saved.lr;
    cpu->ctr = saved.ctr;
    cpu->xer = saved.xer;
    cpu->fpscr = saved.fpscr;
    return result;
}

// --- Scenes and players ---------------------------------------------------------
int current_scene(uint8_t* mem) {
    const uint32_t mgr = rd(mem, kSceneMgr);
    return mgr ? int(rd(mem, mgr + kSceneCurrent)) : -1;
}
int scene_id_of_kind(uint8_t* mem, int kind) {
    const uint32_t kinds = rd(mem, kSceneKinds);
    if (!kinds) return -1;
    for (int id = 0; id < 0x40; ++id)
        if (int(rd(mem, kinds + 0x18 + id * 4)) == kind) return id;
    return -1;
}
bool request_scene(uint8_t* mem, int id) {
    const uint32_t mgr = rd(mem, kSceneMgr);
    if (!mgr || id < 0) return false;
    rbrew_write32(mem, mgr + kSceneRequest, uint32_t(id));
    rbrew_write8(mem, mgr + kSceneRequested, 1);
    return true;
}
// MatchInfo +0x1C points to the next stage's file name (0x100-byte buffer);
// +0x238/+0x23C are reset to 0 (Turf War) whenever the game picks a stage.
bool set_stage(uint8_t* mem, const std::string& name) {
    const uint32_t mi = rd(mem, kMatchInfo);
    const uint32_t text = mi ? rd(mem, mi + 0x1C) : 0;
    if (!text || name.size() >= 0x100 || !rbrew_memory_accessible(mem, text, uint32_t(name.size() + 1))) return false;
    std::memcpy(mem + text, name.c_str(), name.size() + 1);
    rbrew_write32(mem, mi + 0x238, 0);
    rbrew_write32(mem, mi + 0x23C, 0);
    return true;
}
// Game::Player i from PlayerMgr's actor table (as 0261ED44 indexes it), or 0.
uint32_t player_ptr(uint8_t* mem, int i) {
    const uint32_t mgr = rd(mem, rd(mem, kMainMgr) + 0x268);
    if (!mgr) return 0;
    const int count = int(rd(mem, mgr + 0x320));
    if (i < 0 || i >= count || count > 8) return 0;
    const uint32_t type = rbrew_read8(mem, mgr + 0x104 + rd(mem, kPlayerActorType));
    const uint32_t list = rd(mem, rd(mem, rd(mem, mgr + 0x30) + type * 4) + 4);
    const uint32_t player = list ? rd(mem, list + 4 * i) : 0;
    return player && rd(mem, player + 4) == kPlayerVtable ? player : 0;
}

// --- Bot controllers ------------------------------------------------------------
struct Bot {
    uint32_t wrapper = 0, inner = 0, last_hold = 0;
    turfai::WeaponKind weapon = turfai::WeaponKind::Shooter;
    turfai::Brain brain;
};

// Weapon sets (Mush/WeaponSet.byaml) whose main weapon models ship in v16.
struct Loadout {
    uint32_t set;
    turfai::WeaponKind kind;
};
constexpr Loadout kLoadouts[] = {
    {1010, turfai::WeaponKind::Shooter}, // Shot_First_00
    {1040, turfai::WeaponKind::Shooter}, // Shot_Normal_00
    {1041, turfai::WeaponKind::Shooter}, // Shot_Normal_01
    {1030, turfai::WeaponKind::Shooter}, // Shot_Blaze_00
    {1050, turfai::WeaponKind::Shooter}, // Shot_Gravity_00
    {1060, turfai::WeaponKind::Shooter}, // Shot_QuickMiddle_00
    {1100, turfai::WeaponKind::Shooter}, // Shot_Long_00
    {1000, turfai::WeaponKind::Shooter}, // Shot_Short_00
    {2010, turfai::WeaponKind::Roller},  // Roller_Normal_00
    {2000, turfai::WeaponKind::Roller},  // Roller_Compact_00
    {4000, turfai::WeaponKind::Charger}, // Charge_Quick_00
    {4050, turfai::WeaponKind::Charger}, // Charge_Light_00
};

struct Match {
    std::string stage; // e.g. Fld_Crank00_Vss
    int scene = -1;    // VSGame scene ID while it runs
    bool setup_pending = true, entered = false;
    std::array<Bot, 8> bots;
    std::shared_ptr<turfnav::Grid> grid;
    std::array<turfai::TeamPaint, 2> paint;
    std::future<std::shared_ptr<turfnav::Grid>> loading;
    std::future<uint32_t> seq_search; // SeqMgrVSGame, found by its vtable
    bool seq_searched = false;
    uint32_t seq = 0;
    int seq_state = -1;
    uint64_t rewards_frame = 0; // when the rewards screen came up
    bool leaving = false;
    uint32_t seed = std::random_device{}(); // every match plays out differently
    uint64_t frame = 0;
};
std::unique_ptr<Match> s_match;
std::atomic<bool> s_can_start{false}, s_active{false}; // read by the menu (render thread)
std::mutex s_request_mutex;
std::string s_requested; // stage asked for by turf_bots_start (menu thread)
int s_players = 8;

// Copies the primary controller into guest heap memory for each CPU slot.
bool ensure_controllers(CPUState* cpu, Match& match) {
    uint8_t* mem = cpu->mem;
    const uint32_t cm = rd(mem, kCtrlMgr);
    const uint32_t real = cm ? rd(mem, rd(mem, cm + 0x15C)) : 0;
    const uint32_t real_inner = real ? rd(mem, real) : 0;
    if (!real_inner || !rbrew_memory_accessible(mem, real, kWrapperSize) ||
        !rbrew_memory_accessible(mem, real_inner, kInnerSize))
        return false;
    for (int i = 1; i < 8; ++i) {
        Bot& bot = match.bots[size_t(i)];
        if (bot.wrapper) continue;
        static std::array<uint32_t, 8> blocks{}; // guest memory is kept for later matches
        if (!blocks[size_t(i)])
            blocks[size_t(i)] = call_guest(cpu, RBREW_GAME_HEAP_ALLOC_SLOT, {kWrapperSize + kInnerSize});
        if (!blocks[size_t(i)]) return false;
        bot.wrapper = blocks[size_t(i)];
        bot.inner = bot.wrapper + kWrapperSize;
        std::memmove(mem + bot.wrapper, mem + real, kWrapperSize);
        std::memmove(mem + bot.inner, mem + real_inner, kInnerSize);
        rbrew_write32(mem, bot.wrapper, bot.inner);
        bot.brain = turfai::Brain(match.seed ^ (0x9E3779B9u * uint32_t(i + 1)), bot.weapon);
    }
    return true;
}

void write_pad(uint8_t* mem, Bot& bot, const turfai::Pad& pad) {
    const uint32_t hold = pad.buttons;
    rbrew_write32(mem, bot.inner + kTrigger, hold & ~bot.last_hold);
    rbrew_write32(mem, bot.inner + kRelease, bot.last_hold & ~hold);
    rbrew_write32(mem, bot.inner + kHold, hold);
    for (uint32_t bit = 0; bit < 32; ++bit) {
        const uint32_t at = bot.inner + kHoldFrames + 4 * bit;
        rbrew_write32(mem, at, (hold >> bit & 1) ? rd(mem, at) + 1 : 0);
    }
    wf(mem, bot.inner + kLeftStick, pad.lx);
    wf(mem, bot.inner + kLeftStick + 4, pad.ly);
    wf(mem, bot.inner + kRightStick, pad.rx);
    wf(mem, bot.inner + kRightStick + 4, pad.ry);
    bot.last_hold = hold;
}

std::string content_dir() {
    const char* dir = std::getenv("RBREW_CONTENT_DIR");
    return dir && *dir ? dir : "./content";
}

// Each team gets a roller and a charger; the other slots carry shooters.
// Slots 1 and 2 are the first bots of the two teams (teams alternate by slot).
const Loadout& pick_loadout(std::mt19937& rng, int slot) {
    const turfai::WeaponKind role = slot <= 2   ? turfai::WeaponKind::Roller
                                    : slot <= 4 ? turfai::WeaponKind::Charger
                                                : turfai::WeaponKind::Shooter;
    const Loadout* options[std::size(kLoadouts)];
    size_t count = 0;
    for (const Loadout& l : kLoadouts)
        if (l.kind == role) options[count++] = &l;
    return *options[rng() % count];
}

// --- Hooks ----------------------------------------------------------------------
void info_setup(CPUState* cpu, uint32_t, HostFunc original) {
    uint8_t* mem = cpu->mem;
    const uint32_t infos = cpu->r[3];
    original(cpu);
    if (!s_match || !s_match->setup_pending) return;
    s_match->setup_pending = false;
    // Slot 0 is the player; the game's own initializer gives slot i the team
    // (i + offset) & 1, so even slots join the player (4 vs 4).
    const uint32_t own = rd(mem, rd(mem, infos) + kInfoTeam) & 1;
    static uint32_t record = 0; // guest scratch for the weapon record, kept for later matches
    if (!record) record = call_guest(cpu, RBREW_GAME_HEAP_ALLOC_SLOT, {0x20});
    std::mt19937 rng(s_match->seed);
    for (int i = 1; i < s_players && i < 8; ++i) {
        const uint32_t info = rd(mem, infos + 4 * i);
        if (!info) continue;
        call_guest(cpu, kInitInfo, {info, own});
        // A random weapon set, built by the game as for any player.
        const Loadout& loadout = pick_loadout(rng, i);
        if (record) {
            call_guest(cpu, kMakeWeapon, {record, loadout.set});
            std::memmove(mem + info + kInfoWeapon, mem + record, kWeaponRecord);
        }
        s_match->bots[size_t(i)].weapon = loadout.kind;
        trace("slot %d weapon set %u -> %u %u %u", i, rd(mem, info + kInfoWeapon), rd(mem, info + kInfoWeapon + 4),
              rd(mem, info + kInfoWeapon + 8), rd(mem, info + kInfoWeapon + 12));
    }
    trace("player slots: %d players", s_players);
}

void get_controller(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t index = rd(cpu->mem, cpu->r[3] + kPlayerIndex);
    if (s_match && index > 0 && index < 8 && s_match->bots[index].wrapper) {
        cpu->r[3] = s_match->bots[index].wrapper;
        return;
    }
    original(cpu);
}

// --- Match ------------------------------------------------------------------------
void start_match(CPUState* cpu, const std::string& stage) {
    uint8_t* mem = cpu->mem;
    const int id = scene_id_of_kind(mem, kKindVSGame);
    if (id < 0 || !set_stage(mem, stage)) {
        trace("cannot start %s (scene %d)", stage.c_str(), id);
        return;
    }
    s_match = std::make_unique<Match>();
    s_match->stage = stage;
    s_match->scene = id;
    if (const char* seed = std::getenv("RBREW_BOTS_SEED")) s_match->seed = uint32_t(std::strtoul(seed, nullptr, 10));
    request_scene(mem, id);
    trace("match on %s requested", stage.c_str());
}

void run_match(CPUState* cpu, Match& match) {
    uint8_t* mem = cpu->mem;
    const int scene = current_scene(mem);
    if (scene == match.scene)
        match.entered = true;
    else if (match.entered) {
        trace("match over (scene %d)", scene);
        s_match.reset();
        return;
    }
    if (!match.entered || !ensure_controllers(cpu, match)) return;
    ++match.frame;

    turfai::Snapshot world;
    for (int i = 0; i < 8; ++i) {
        const uint32_t p = player_ptr(mem, i);
        if (!p) continue;
        if (i > 0 && rd(mem, p + kPlayerRemote) != 0) call_guest(cpu, kSetLocal, {p, 1, 0});
        turfai::PlayerView& v = world[size_t(i)];
        v.valid = true;
        v.team = int(rd(mem, p + kPlayerTeam));
        v.pos = {rf(mem, p + kPlayerPos), rf(mem, p + kPlayerPos + 4), rf(mem, p + kPlayerPos + 8)};
        v.aim = std::atan2(rf(mem, p + kPlayerAim + 8), rf(mem, p + kPlayerAim));
        if (const uint32_t tank = rd(mem, p + kPlayerTank)) v.ink = std::clamp(rf(mem, tank + kTankInk), 0.f, 1.f);
        v.valid = std::isfinite(v.pos.x) && std::isfinite(v.aim) && (v.pos.x != 0 || v.pos.z != 0);
    }

    // Navigation grid, built off the game thread once the players stand at their bases.
    if (!match.grid && !match.loading.valid() && world[0].valid) {
        std::vector<turfnav::Vec3> seeds;
        for (const auto& v : world)
            if (v.valid) seeds.push_back(v.pos);
        const std::string model = match.stage.substr(0, match.stage.rfind('_'));
        match.loading = std::async(std::launch::async, [dir = content_dir(), model, seeds] {
            auto grid = std::make_shared<turfnav::Grid>();
            if (!turfnav::load_stage(dir, model, *grid)) return std::shared_ptr<turfnav::Grid>();
            grid->keep_reachable(seeds);
            return grid;
        });
    }
    if (match.loading.valid() && match.loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        match.grid = match.loading.get();
        trace("grid %s: %zu nodes", match.stage.c_str(), match.grid ? match.grid->size() : size_t(0));
        if (!match.grid) match.grid = std::make_shared<turfnav::Grid>(); // keep bots idle
        for (auto& team : match.paint) team.reset(match.grid->size());
    }

    for (int i = 1; i < 8; ++i) {
        Bot& bot = match.bots[size_t(i)];
        turfai::Pad pad;
        const int team = world[size_t(i)].team & 1;
        if (match.grid && match.grid->size())
            pad = bot.brain.think(i, world, *match.grid, match.paint[size_t(team)], match.frame);
        write_pad(mem, bot, pad);
    }
    // Match sequence: located once, off the game thread (read-only heap scan).
    if (!match.seq_searched) {
        match.seq_searched = true;
        match.seq_search = std::async(std::launch::async, [mem] {
            for (uint32_t a = 0x28000000u; a < 0x68000000u; a += 4) {
                if (!rbrew_memory_accessible(mem, a, 4)) {
                    a |= 0xFFFC;
                    continue;
                }
                if (rbrew_read32(mem, a) == kSeqVtable) return a - 4;
            }
            return 0u;
        });
    }
    if (match.seq_search.valid() && match.seq_search.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        match.seq = match.seq_search.get();
    if (match.seq && rd(mem, match.seq + 4) == kSeqVtable) {
        const int state = rbrew_read8(mem, match.seq + kSeqState);
        if (state != match.seq_state) trace("f%llu sequence state %d", (unsigned long long)match.frame, state);
        match.seq_state = state;
        // Offline the rewards screen has no way out: back to the Plaza after A
        // (once the screen had a moment) or after half a minute.
        if (state == kSeqRewards && !match.rewards_frame) match.rewards_frame = match.frame;
        if (match.rewards_frame && !match.leaving) {
            const uint32_t cm = rd(mem, kCtrlMgr);
            const uint32_t pad = rd(mem, rd(mem, rd(mem, cm + 0x15C)));
            const bool a = pad && (rd(mem, pad + kHold) & kPadA);
            const uint64_t shown = match.frame - match.rewards_frame;
            if ((a && shown > 120) || shown > 1800) {
                match.leaving = request_scene(mem, scene_id_of_kind(mem, kKindPlaza));
                trace("back to the Plaza: %d", match.leaving);
            }
        }
    }
    if (match.frame % 300 == 0)
        for (int i = 0; i < 8; ++i)
            if (world[size_t(i)].valid)
                trace("f%llu p%d team %d pos %.0f %.0f %.0f ink %.2f", (unsigned long long)match.frame, i,
                      world[size_t(i)].team, world[size_t(i)].pos.x, world[size_t(i)].pos.y, world[size_t(i)].pos.z,
                      world[size_t(i)].ink);
}

// RBREW_BOTS_MATCH=<stage file>@<frame> (testing): asks for a match from the
// Plaza at that frame, as the menu does. RBREW_BOTS_PLAYERS=<2..8> plays with fewer.
void scripted_start(uint64_t frame) {
    static std::string stage;
    static uint64_t at = 0;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* v = std::getenv("RBREW_BOTS_MATCH")) {
            stage = v;
            if (const size_t sep = stage.find('@'); sep != std::string::npos) {
                at = std::strtoull(stage.c_str() + sep + 1, nullptr, 10);
                stage.resize(sep);
            }
        }
        if (const char* v = std::getenv("RBREW_BOTS_PLAYERS")) s_players = std::clamp(std::atoi(v), 2, 8);
    }
    if (!stage.empty() && frame == at) {
        std::lock_guard lock(s_request_mutex);
        s_requested = stage;
    }
}
} // namespace

void turf_bots_register() {
    if (RBREW_GAME_REVISION != 16) return; // addresses are for the base executable
    rbrew_set_guest_hook(kInfoSetup, info_setup);
    rbrew_set_guest_hook(kGetController, get_controller);
}

void turf_bots_start(const char* stage) {
    std::lock_guard lock(s_request_mutex);
    s_requested = stage ? stage : "";
}

bool turf_bots_active() {
    return s_active.load(std::memory_order_relaxed);
}

bool turf_bots_can_start() {
    return s_can_start.load(std::memory_order_relaxed);
}

void turf_bots_tick(CPUState* cpu) {
    if (RBREW_GAME_REVISION != 16) return;
    static uint64_t frame = 0;
    ++frame;
    scripted_start(frame);
    std::string requested;
    {
        std::lock_guard lock(s_request_mutex);
        requested.swap(s_requested);
    }
    const int scene = current_scene(cpu->mem);
    const bool in_plaza = scene >= 0 && scene == scene_id_of_kind(cpu->mem, kKindPlaza);
    if (!requested.empty() && !s_match && in_plaza) start_match(cpu, requested);
    if (s_match) run_match(cpu, *s_match);
    s_can_start.store(in_plaza && !s_match, std::memory_order_relaxed);
    s_active.store(s_match != nullptr, std::memory_order_relaxed);
}
