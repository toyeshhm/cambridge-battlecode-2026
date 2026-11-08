#pragma once
// p-monolith: monoliths on the Week 2 engine (bots/p-monolith/CHANGES.md). Probe facts (cambc 3.0.1, runs/p-monolith/probe):
// a monolith hits the nearest champion within Chebyshev 5 (through walls) for 200 at each round start, ties to that round's
// turn order (priority, then draft order); proximity never wakes it, damage does; asleep at the 2nd round start with nobody
// within 5; no regeneration. Every stun lands on it and skips exactly its next hit (Ryth AoE, Ren / Tavi / Prism cc, Heart of
// Ruin's dash), back-to-back stuns chain (4 casts = 4 skipped hits); stun buff duration 2 = cast this round, 1 = spent.
// Shields absorb its hits (Varg 2.5 x focus, HoR 1.5 x focus) and Willow heals 3 x focus.
#include "world.h"
#include "zreach.h" // p-fix ring set (MONO_NOSIEGE, ESC_RANGE: escape_danger leaves to range + 1)
#include <vector>

// MONO_TAKE: a deliberate full-team take (mono.cpp). The squad-0 leader starts it when the model (team damage, stuns, shields,
// heals vs the live monolith hp and the team's hp above each champion's reserve) finishes with margin and no enemy is near;
// everyone stages at 6 and walks in while it sleeps (no hit before the team is in), adjacent (bags and fragile casters one or
// two back), the healthiest of the nearest takes each hit (priority by hp), stuns are staggered, the hurt leave to 6 (the
// healthiest last) and recall. Abort (everyone out to 6) on enemies near or a lost margin; the live model is re-checked at
// the stage, before anyone wakes it. Details and numbers: bots/p-monolith/CHANGES.md.
inline constexpr bool MONO_TAKE = true;
// MONO_TAKE_FARM (with MONO_TAKE): also take monoliths where the farm is an alternative (00monolith, 01gold-rush, ...), when
// the take's value a round beats the farm's (TAKE_ALT_X); off = only where it is the only way to bank (boss maps, the seal),
// and then (MONO_DENY off) every other map runs none of this code, so it replays the base game for game
inline constexpr bool MONO_TAKE_FARM = false;
// MONO_DENY (mono_deny.cpp): an awake, wounded monolith with enemy champions in its range pulls us there (their tank is low
// and pinned; pvp::turn fights once an enemy is within 7).
inline constexpr bool MONO_DENY = false;
// MONO_GOLD (contest.cpp): boss maps: the enemy half's gold groups too while no enemy was near me for 40 rounds (tiebreak).
inline constexpr bool MONO_GOLD = false;

inline constexpr float TAKE_MARGIN = 1.15f; // go in: hp pool >= need x margin (live stats, everyone at the stage)
inline constexpr float TAKE_KEEP = 1.0f;    // continue while pool >= need x keep
// TAKE_PART (0 = off): where the take is the only way to bank (boss maps; the monolith sealing every altar), also go in when
// the pool covers this share of the need; the session then runs until the team's hp above the reserves is spent (no
// regeneration: what it dealt stays), everyone heals, and the next session finishes it
inline constexpr float TAKE_PART = 0.6f;
inline constexpr float TAKE_RES = 0.35f;    // a champion leaves below max(TAKE_RES x max hp, TAKE_RES_ABS)
inline constexpr float TAKE_RES_ABS = 450.f;
inline constexpr int TAKE_STAGE = 6;        // staging ring (out of range 5)
// TAKE_FINISH: an engaged take a few rounds from the kill (FINISH_T) goes on with an enemy near (participants still step out
// on their own when one is within 8 of them)
inline constexpr bool TAKE_FINISH = false;
inline constexpr float TAKE_ALT_X = 1.25f;  // off boss maps / the seal: the take's value a round must beat the farm's by this
// SEAL_TAKE (notes/losses-nov1.md s3): a monolith that seals the only altar(s) (02guardians, 01boss-battle; not boss maps) is the
// team's job. v28 goes 26/50 there; its first tribute is r1087-1235. Debug (hf vs hf, guardians): the first take starts at
// r744 (TAKE_PART's 0.6 of the need reached only then), and everyone with 30+ gems (on a sealed map: everyone) stands one
// back, so only HoR hits it (123 damage a round, 6 sessions). On: carriers stand adjacent there too (the healthiest takes
// each hit; the reserve still sends the hurt out), and from STAKE_R a session starts at STAKE_PART of the need.
inline constexpr bool SEAL_TAKE = false;
inline constexpr float STAKE_PART = 0.3f;
inline constexpr int STAKE_R = 250;

namespace mono {
int take_group();                 // team take target (squad 0 byte with the help flag on a monolith group), -1 none
int prio();                       // turn priority while engaged in a take (-1: not engaged)
std::vector<vec2> mono_tiles(int gi);                       // spawn tiles of group gi's monoliths
bool enemy_near(const std::vector<vec2>& pts, int r);       // a visible enemy champion within r of pts
bool move_ring(int gi, const std::vector<vec2>& mt, int want); // walk the monolith field down to `want` steps
const char* deny(const std::vector<cambc::champion>& enemies, int& target); // MONO_DENY (mono_deny.cpp)
// mono_act.cpp
bool get_out(const std::vector<vec2>& mt);   // out to 6 down a static escape field; true = the turn is handled
bool camp_threat(vec2 p, int skip_gi);       // an awake camp of another group can hit me at p
void spells(const std::vector<cambc::camp>& ms, bool moved, bool tank); // stuns staggered, damage, shield, heal
}
