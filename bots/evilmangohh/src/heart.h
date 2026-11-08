#pragma once
// h-heart: Heart of Ruin (Week 2 patch, 12th champion, melee; engine values checked by runs/h-heart/probe, CHANGES.md).
// S1 (slot 0): self shield 1.5 x focus for 5 turns. S2 (slot 1): dash up to 2 tiles (after a walk too); the landing deals
// 1.75 x focus to every enemy and camp within 1 and stuns them 1 turn. Passive: all damage x (1 + 0.5 x missing hp share).
// Every switch off builds rr-c2's binary (release md5 d9ffc674): all HoR code sits behind `if constexpr`.
#include <cambc.h>
#include <array>
#include <vector>

// Draft (draft.cpp, normal maps only; contest keeps its range-4 table). Each its own switch; one on per build.
inline constexpr bool HD_VARG = false;  // HoR in place of Varg (5-pick maps)
inline constexpr bool HD_RYTH = false;  // HoR in place of Ryth (4- and 5-pick maps)
inline constexpr bool HD_CI = true;    // HoR in place of Cassi & Issac (every normal map)
inline constexpr bool HD_FIRST = false; // HoR always: drops the last pick (Varg on 5, Ryth on 4, C&I on 3-pick maps)
inline constexpr bool HD_SMALL = false; // HoR in place of C&I on 3-pick maps only (HD_FIRST's 3-pick part)
// Our HoR's behaviour (heart.cpp; hooks in bot.cpp, combat.cpp, actions.cpp, world.cpp, pvp*.cpp): on with any draft switch.
inline constexpr bool HOR_PLAY = HD_VARG || HD_RYTH || HD_CI || HD_FIRST || HD_SMALL;
// Enemy HoR in the fight model (fight.h tabulate, fight_sdk.h): its dash is a damage + stun option out to distance 4 (walk 1,
// dash 2, radius 1) and its passive scales its strength and focus. On with HOR_PLAY (our own HoR is the same model).
inline constexpr bool HOR_ENEMY = false;
inline constexpr bool HOR_MODEL = HOR_PLAY || HOR_ENEMY;
inline constexpr int HOR_TRAVEL = 12; // our HoR dashes for travel only this far (field steps) from its goal: the dash is back by arrival
// c12h HOR_PRESHIELD (0 = v55). 1: a pvp turn with a foe champion within 4 opens with S1 (hor::preshield) before the planner
// can spend the one cast on the dash; not while hunting (a channel / FINISH prey: the stun) or below 40% hp (the escape dash).
// 2: 1 + no engage dash landing within 2 of 2+ foes with no ally within 3 of the landing (pvp_plan.cpp eval_tile).
// 3: before contact only: the nearest foe champion at 5-7 (out of walk + dash + landing reach 4, so the cast costs no hit).
inline constexpr int HOR_PRESHIELD = 3;

namespace hor {
// Am I Heart of Ruin (each champion runs its own instance).
inline bool me() {
    static const bool v = cambc::self().champion_name() == cambc::champion_type::heart_of_ruin;
    return v;
}
inline float passive_mul(float hp, float max_hp) { return 1.f + 0.5f * (1.f - hp / (max_hp > 1.f ? max_hp : 1.f)); }
extern bool escaping;     // retreat(): a travel dash toward home is allowed however near
extern float strike;      // pvp: my dash's landing damage this turn (0: not ready / not HoR)
extern float shield_v;    // pvp: my shield if a turn without another cast would cast it (0: not ready)
// pvp: sleeping dangerous camps (a landing next to one wakes it); a function static, so the all-off build has no static init
inline std::vector<cambc::vec2>& sleepers() { static std::vector<cambc::vec2> v; return v; }
// Camp fight: walk 0-1 + dash next to the most live members of the group (awake attackers not stunned first), never next to
// another group's camp. Returns true if it dashed (the caller then attacks).
bool engage(const std::vector<cambc::camp>& members);
// Landing tile for a travel dash: no visible camp within 1 (the landing would hit and wake it).
bool quiet(cambc::vec2 q);
// A dash from a to b (1-2 tiles) the engine accepts: b free, and for 2 tiles a non-wall, non-camp tile next to both.
bool dash_path(cambc::vec2 a, cambc::vec2 b, const cambc::bitboard& camps);
// HoR joins squad 1 while it has fewer than 3 bodies (it replaces Varg's or C&I's place as their tank).
void squad(const std::vector<cambc::champion>& allies, std::array<int, 8>& squad_of);
// Shield wanted now: in danger and no shield buff up (any hp: it lasts 5 turns, back in 8).
bool want_shield(bool in_danger);
// HOR_PRESHIELD: cast S1 now if it is ready, no shield is up and hp >= 40%.
void preshield();
} // namespace hor
