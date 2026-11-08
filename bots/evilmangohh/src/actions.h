#pragma once
// Movement and combat primitives shared by all champion behaviours.
#include "world.h"
#include <functional>
#include <optional>
#include <string>

extern std::string g_acts; // debug: actions taken this turn

bool is_ranged();
bool exclusive(const bc::ability& a);
float ability_range(const bc::ability& a);

// Step to the neighbouring tile (or stay) minimising score(q); +inf excludes a tile.
// Returns true if a move happened.
bool best_step(const std::function<float(vec2)>& score);
// Follow a distance field downhill, avoiding tiles in `avoid`.
bool step_field(const Field& f, const bc::bitboard* avoid = nullptr);
// Walk towards the free tiles within `range` of t, routing around walls and visible units.
bool approach(vec2 t, int range);
// Inside monster range (Chebyshev < 4 of any of `danger`): walk (BFS around walls and units) to the nearest tile
// at 4+. Greedy steps froze champions in wall pockets at distance 3 of the monolith (200 dmg/round).
bool escape_danger(const std::vector<vec2>& danger);
// q-act F1 DASH_WALK: a dash spends only the cast, so walk one more step down the field after it (3 tiles a turn).
// Platform v9 replays: 70 of our ~82 dashes a game were dash-only; the top teams walked on 72 of 74.
inline constexpr bool DASH_WALK = true;
// c8 STEP_STICK (local c7mx vs starter-cpp: 01the-farm 75-0 / 0-105 at r2000, all three bags frozen r450-r2000 4 from the
// centre altars). Static fields ignore camps: above the y=19 wall the altar path crosses the wolf / gold group on (23-26,19).
// Blocked, step_field's window BFS steps one tile back toward the west gap (uphill on the field); the next turn the static
// step walks forward again: A-B-A for good (debug: SFBFS dyn 15 -> 14, then the static step 4 -> 3). After an uphill BFS
// step, the next STEP_STICK rounds on that field skip the static step and the dash (window BFS only; no move when the turn
// cannot afford it). Allies count as blockers too: on the_farm Marcella's way down is Ren, Ren's is the camp row. Never with
// an enemy seen in the last 2 rounds (a frozen or slow walk in a fight dies). 0 = v34.
inline constexpr int STEP_STICK = 3;
// c18b SF_STARVE (notes/bughunt-ranked-nov3.md walk freeze): the window BFS was skipped for fuel (farm keeps its target).
// HoR froze r145-347 on road_of_riches at (28,22): a camp on the static step, then the BFS skipped every turn (~42k left).
inline constexpr bool SF_STARVE = false;
inline bool& sf_starved() { static bool v = false; return v; }
inline bc::distance_field& sf_from() { static bc::distance_field v; return v; } // BFS from me (step_field)
// Dash along a distance field when it gains at least 2 steps (then walk, DASH_WALK). Returns true if dashed.
bool dash_field(const Field& f);

// Basic attack + attack reset follow-up on the best target among `targets` (camps or champions).
bool attack_camp(const std::vector<bc::camp>& camps);
// No member of `camps` is alive (all dead, or none visible).
bool camps_dead(const std::vector<bc::camp>& camps);
bool attack_champion(const std::vector<bc::champion>& enemies);
// Cast damaging / cc abilities. allow_exclusive=false skips spells that forbid walking.
bool cast_on_camps(const std::vector<bc::camp>& camps, bool allow_exclusive);
bool cast_on_enemies(const std::vector<bc::champion>& enemies, bool allow_exclusive);
// Heals, shields and other self/ally support. Returns true if something was cast.
bool cast_support(bool in_danger);
// Attack reset if it lets us strike again right now.
bool try_reset_and_attack(const std::function<bool()>& attack_again);

float champ_dps(const bc::champion& c);
bool is_channeling(const bc::champion& c);

float hp_frac(const bc::champion& c);
int min_dist(vec2 q, const std::vector<vec2>& pts);
