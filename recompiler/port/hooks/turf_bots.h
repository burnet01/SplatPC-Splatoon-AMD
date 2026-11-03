// turf_bots.h - offline Turf War with CPU players.
#pragma once
#include "../runtime/rebrewu_runtime.h"

// Arms the guest hooks (manifest [hooks] 0216F8E0 and 025A96A4).
void turf_bots_register();
// Once per game frame from VPADRead (game thread).
void turf_bots_tick(CPUState* cpu);
// Requests a 4 vs 4 match against CPU players on a Turf War stage file
// (e.g. "Fld_Crank00_Vss"); any thread, applied on the next game frame.
void turf_bots_start(const char* stage);
// A bot match is starting or running.
bool turf_bots_active();
// The game is in the Plaza with no bot match, so turf_bots_start applies.
bool turf_bots_can_start();

// Turf War stages offered in the menu (launch stages of v16).
struct TurfBotsStage {
    const char* name; // as shown
    const char* file; // stage file
};
inline constexpr TurfBotsStage kTurfBotsStages[] = {
    {"Urchin Underpass", "Fld_Crank00_Vss"},         {"Walleye Warehouse", "Fld_Warehouse00_Vss"},
    {"Saltspray Rig", "Fld_SeaPlant00_Vss"},         {"Arowana Mall", "Fld_UpDown00_Vss"},
    {"Blackbelly Skatepark", "Fld_SkatePark00_Vss"},
};
