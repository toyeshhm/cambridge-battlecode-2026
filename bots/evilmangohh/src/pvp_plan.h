#pragma once
// pvp_plan.h -- the one-turn planner behind pvp::turn (pvp.cpp): end tiles x action sets, scored as value dealt minus
// risk x damage the enemy can land on the tile before our next turn. Internal to the pvp module.
#include "fight.h"
#include "world.h"
#include <array>
#include <functional>
#include <vector>

namespace pvp::plan {

using fight::Fight;
using fight::State;
using fight::Unit;

constexpr int TILE_FUEL = 1800, EXEC_FUEL = 9000, SIM_UNIT_FUEL = 5000; // measured: per planned tile, actions, simulate(3) per unit
constexpr int FARM_FUEL = 20000; // a declined fight farms on: camp target, camp fight, movement (f-fuel profile)
constexpr int SIM_MAX_UNITS = 5;  // bigger fights: Lanchester verdict (pvp.cpp)
constexpr float NEXT = 0.5f; // committed: weight of next turn's damage on the focus for a pure move
constexpr bool STUN_LOCK = true;  // h-micro T1c: lethal check also with every foe one turn more when one can stun us
constexpr float LETHAL_PEN = 3000.f;
constexpr float TRAP_PEN = 0.f;   // h-micro T2: penalty for a tile with no escape tile next turn (0: off) // h-micro R1: score penalty for ending on a lethal tile without a kill (0: off)
constexpr int MAXF = 8;

inline bc::champion champ(unsigned id) { return bc::detail::handle_access::make_champion(static_cast<bc::handle_champion_t>(id)); }

struct Foe {
    int fi = -1, link = -1; // index in F, linked foe (index in foes)
    unsigned id = 0;
    vec2 pos;
    float hp = 0, shield = 0, taken = 1, kill_v = 0, stun_v = 0, w = 1, retal = 0;
    bool stunned = false, hidden = false; // hidden: h-micro G1 ghost (risk only, never a target)
    std::array<float, 2> sd{}; // hp my slot s would remove (damage_amount when it may decide a kill)
    // h-micro T1: turns before my next one (0..2); damage by distance on its first turn, its second turn, and its
    // second turn when the first is lost to a stun
    signed char turns = 0;
    std::array<float, fight::MAXD + 1> one{}, two{}, st2{}; // + 1: a foe acting twice walks in from 6
    int stun_r = -1; // h-micro T1c: farthest distance its first turn can stun us from (-1: no stun ready)
    std::array<float, fight::MAXD + 1> nxt{}; // h-micro T2: what it lands at distance d in the window after our next turn
    std::array<float, fight::MAXD> soon{}; // f-small R1: extra damage by distance once spells at cd 1 are back
};

struct MySpell { int slot = -1; bool dmg = false, cc = false, aoe = false, excl = false, stuns = false; int range = 0, radius = 0; float raw = 0; };

struct Plan {
    vec2 dest{}, dash{-1, -1};
    bool walk = false;
    int spell = -1, sp_foe = -1;
    vec2 ground{-1, -1};
    int hit0 = -1, hit1 = -1; // basic before / after the reset
    bool reset = false;
    // HIT_FIRST: the basics (and reset) from the start tile, before the dash / walk. No initialiser (it sits in padding, so the
    // off build stays v26's binary): pvp.cpp clears it on base / best when HIT_FIRST is on; nothing reads it otherwise.
    bool first;
    float value = 0, score = -1e30f;
};

struct Ctx {
    Fight F;
    State S;
    int mi = -1, nf = 0, focus = -1, arange = 1, reset_slot = -1, dash_slot = -1;
    std::array<Foe, MAXF> foes{};
    std::array<MySpell, 2> sp{};
    bool ranged = false, splash = false, commit = false;
    float str = 0, my_taken = 1, my_ehp = 1, lambda = 1, death = 0;
    int floor = 0; // the planner stops evaluating below this much fuel left (actions, or farming after a decline)
    vec2 p;
    std::vector<std::pair<vec2, float>> camps; // awake monsters: position, strength
    std::vector<vec2> allies;
    bc::bitboard occ;
    bool trap_on = false; // h-micro T2: some foe acts twice in the window after our next turn
};

// h-micro T1: raw damage foe f lands at distance d before my next turn (stun: stunned now or by this turn's plan).
float foe_dmg(const Foe& f, int d, bool stun);
// The same for k turns (a third turn repeats the second).
float foe_dmg_k(const Foe& f, int d, bool stun, int k);
// What foe j can land on us at q before our next turn (its best option at that distance, walking included).
float contrib(const Ctx& c, int j, vec2 q, bool stun_now = false);
float camp_danger(const Ctx& c, vec2 q);
// f-small R1: a spell back for the foe's turn after next that we cannot step out of next turn (distance + 1).
float soon_risk(const Ctx& c, int j, vec2 q);
constexpr float SOON_W = 0.5f;
bool free_tile(const Ctx& c, vec2 q);
// Every action set from pl.dest; keeps the best-scoring plan in `best`. mode 0: stayed, 1: walked, 2: dashed.
void eval_tile(const Ctx& c, Plan& best, Plan pl, int mode, float pos_term);
void execute(const Ctx& c, const Plan& pl);
// v7 carrier hunt (V8_HUNT, ported from bots/v8): a hurt (< 60% hp) visible enemy within 6 carrying 40+
// gems, or HUNT_FARMER+ in our half, with no other enemy within 4 of it, while 2+ allies are within 6 of us; ghosts and
// enemies on their spawn are no prey. Returns the best one's index in c.F (gems + 100 x missing hp fraction), -1 if none.
inline constexpr bool V8_HUNT = true;
constexpr int HUNT_FARMER = 20; // 8.2: an empty-bagged enemy in our half is no prey (swirl: 176 gemless hunts, 2x deaths)
int hunt_carrier(const Ctx& c);
// h-heart (HOR_PLAY, our HoR only): hor::strike / shield_v / sleepers for this turn; extra dash landings next to foes (1 tile,
// or walk 1 then dash 2; pvp.cpp's ring covers 2); and whether the end tile is in reach of a foe or awake camp (shield).
void hor_setup(const Ctx& c);
void hor_plans(const Ctx& c, Plan& best, const Plan& base, const std::function<float(vec2)>& pos);
bool hor_danger(const Ctx& c);
// CARRY (zreach.h): this turn we carry a carrier's bag (pvp.cpp sets it; a function static: nothing emitted when off).
inline bool& carry_flag() { static bool b = false; return b; }
// OPEN_GATE (zreach.h): pr this turn when out of contact, else 1e9 (no gate). LOW_RUN: the run cell holds this turn.
inline float& open_pr() { static float v = 1e9f; return v; }
inline bool& low_run() { static bool b = false; return b; }

} // namespace pvp::plan
