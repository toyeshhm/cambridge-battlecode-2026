#pragma once
// p-fix: the Week 2 engine (cambc 3.0.1) correctness switches of notes/patch-audit.md and one engine camp-stat helper.
// Elk (4) and golem (4) now hit from 4, monoliths (5) from 4-5; every spell has range 4 and only an AoE (range 4 +
// radius 1) reaches 5. Every switch false builds rr-c2's binary (bots/p-fix/CHANGES.md).
#include "world.h"
#include <vector>

inline constexpr bool CAMP_STATS = true;   // F6: camp hp / str from the engine (clusters 120 hp, golem 5000 / 120)
inline constexpr bool RING = true;         // F1: farm wait ring = each member's own range + 1; cast / shoot only from it
inline constexpr bool ESC_RANGE = true;    // F4: retreat recall, R5 escape zones and escape_danger use each monster's range
inline constexpr bool REACH_ALL = true;    // F5: ranged_reposition, fight() tanking / step-out, safe, camp_danger, reveal()
inline constexpr bool MONO_NOSIEGE = true; // F2: no monolith siege: a monolith is a target only as a reward
inline constexpr bool SIEGE_REACH = true;  // F3: siege dps per target: abilities with range + radius >= camp range + 1
inline constexpr bool WARD_KEEP = false;    // F9: no follow re-wards; a new cluster only when uncovered; keep it at 20+ gems
inline constexpr bool DASH_CAMP = true;    // F15: no camp tile as a dash intermediate; a failed pvp dash still walks
inline constexpr bool SIEGE_HERD = true;   // A/B knob for F3: false = no siege of elk herds (str >= 180) at all
inline constexpr bool REACH_CACHE = true; // c4: group_reach() cached per group (fuel only; same answers)
// v22 fight fixes (notes/fights-oct26.md; 64% of our deaths vs the top 10 start with our hit, poke + bait 46%):
// PV_DECLINE: an uncommitted turn values non-killing damage at 0 unless its end tile is out of every foe's reach (a kite).
// PV_POWER: no commit when the foes within 7 have >= 2x our ehp x threat (pvp.cpp's Lanchester ratio < 0.5).
// PV_RECALL: a recall is not a channel (prey / stun / focus bonuses) until its timer has run to <= 3 (several top teams
// restart recall every turn as bait); tributes still count.
inline constexpr bool PV_DECLINE = true;
inline constexpr bool PV_POWER = true;
inline constexpr bool PV_RECALL = true;
// PV_RUN (v23 candidate): a declined fight that is going badly (sim score < STAY or hp < 60%) triples the home pull
inline constexpr bool PV_RUN = false;
// PV_QUIET (v23): 8 rounds in reach with no damage either way (committed or not) farm on behind the avoid mask instead of
// standing in reach forever (sw3 vs starter-cpp on 00rushbait-v2: our 4 at spawn 2000 rounds, 0 banked)
inline constexpr bool PV_QUIET = true;
// HIT_FIRST (pvp_plan.cpp eval_tile / execute): a melee champion's walk or dash plan also tries its basic (and attack reset
// + basic) from the start tile BEFORE moving; plans were always move-then-attack, so a step away from an adjacent foe never
// hit it. v26 benchmark (98 games): our melee turns that start next to a foe hit a champion 50% (theirs 83%) and walk out
// of contact without a hit 35% (theirs 1%). Same end tile, same risk; under PV_DECLINE (no value for a poke) it wins only
// the tie with the plain walk. false = v26's binary 979b4a0a; v28 ships true.
inline constexpr bool HIT_FIRST = true;
// c6f fight switches (notes/top5-fights-nov1.md; all false = v28's binary 1414a319). Power = pvp.cpp's ehp x threat sums
// within 7 (PV_POWER's): a = ours, b = theirs.
// FINISH: a foe under 30% hp within 4 (off its spawn), or under 35% and recalling at any timer and reachable before it
// ends, is prey while we have >= 40% hp and < 30 gems: team focus, commit unless b > 1.5a (PV_POWER still applies);
// any foe under 30% hp is worth +200 on a kill, and every foe's target weight grows with its missing hp (+0.6 x fraction)
// so basics and spells pile on the hurt one. v28: we finish 25% of such foes within 8 rounds, the top 5 58%.
// Local vs v28: 141/280, deaths -0.25, kills +0.25, walk-aways from a low foe 25% -> 16%: ships (VERSION.md).
inline constexpr bool FINISH = true;
inline constexpr float FIN_PULL = 20.f; // FINISH, committed: per tile from the prey (APPROACH is 5 beyond reach only)
// PRESS: >= 60% hp, < 30 gems, a foe within 3 and a >= 1.5b (and no FINISH prey): commit, risk weight x PRESS_L and
// PRESS_PULL per tile from the focus (v28 hits 39% / steps away 26% there; top 5 53 / 14; nobody dies there, 0-1%).
// Committing alone changed nothing locally (in contact we already commit above STAY): the planner stepped out on risk.
// Dropped: commit alone was a no-op; with L / PULL, 15/40 on fight maps with deaths +2.75 a game.
inline constexpr bool PRESS = false;
inline constexpr float PRESS_L = 0.6f, PRESS_PULL = 10.f;
// REGROUP: declined under threat with the nearest ally > 4 away: -8 per tile of ally distance gained, on top of HOME.
// 39% of v28's deaths are lone (top 5 23%).
// Dropped: the alone share did not move (25%) and fight-map deaths rose 10.1 -> 14.65 a game.
inline constexpr bool REGROUP = false;
// CARRY: a 15+ gem carrier (was 30+) commits only on a sim score >= 1, and uncommitted gives a non-killing hit no value even
// from a kite tile. Losses: carrier deaths 2.2 a game (91 gems), 23 of 47 started with our hit.
// Dropped: carrier first hits 16% -> 4% but carrier deaths did not fall (0.95 -> 1.18); FINISH + CARRY z -1.31.
inline constexpr bool CARRY = false;
// c7f fight switches (notes/top5-fights-nov1.md; all off = v31's binary 28f1dcef). pr = a / b as FINISH's.
// OPEN_GATE: out of contact (no contact for 3 rounds) and uncommitted, a non-killing hit has no value unless pr >= OPEN_PR
// and an ally is within 4 of the end tile (drops PV_DECLINE's kite exception for openings). v28: 37% of our openings at
// local power < 0.67 (top 5 16%), 53% alone, 95% range-4 spells. Local vs v31: 139/280, openings 33.7 -> 28.5 a game: ships.
inline constexpr bool OPEN_GATE = false;
inline constexpr float OPEN_PR = 1.f;
// LOW_RUN: below 30% hp with a foe within 4 and pr < 0.67: no value for non-killing damage (kite tiles too), risk weight x2,
// home pull x2. v28 there: hit 51% (top 5 30%), died within 5 rounds 65% (51%). Local vs v31: 148/280, deaths -0.55: ships.
inline constexpr bool LOW_RUN = false;
// LOW_B: a basic's target choice (not its value) adds LOW_B x our strength for the lowest hp + shield foe in range, so a
// kill still comes first, then the lowest, over the focus (+0.5) / channel / stun weights. v31 benchmark: we basic the
// lowest of 2+ adjacent foes 66% (FINISH's +0.6 x missing hp did not move it from v28's 67%); their bots 86%, top 5 83%.
// Local vs v31 (LOW_B 1): 145/280, lowest 60% -> 75%, fuel-outs +0.3 (VERSION.md).
inline constexpr float LOW_B = 0.f;
// F4 extension: retreat once last round's hp loss x the rounds needed to walk out of the awake monsters' range (range + 1
// - distance) reaches my hp. A tank next to an elk pair needs 4 rounds (was 3) and died walking out at 33% hp (00eight).
inline constexpr bool ESC_EXIT = false;
// Extra, found in testing: a monolith (range 5) behind a wall hits while our vision (8, occluded by walls and bushes) cannot
// see it (00contest: the gold triples and the corridor around each walled monolith room are at 5; 19 monolith deaths in 2
// games vs a sparring bot that keeps it awake). UNSEEN: inside the reach of a monolith believed alive and awake (seen awake in
// the last 10 rounds, or an hp loss with no visible cause), leave its reach and skip a target within it for 30 rounds.
inline constexpr bool UNSEEN = false;

struct CampStat { float hp, str; int range; };
// Engine base stats of a camp kind (sdk camp_design), cached once per kind.
const CampStat& camp_stat(bc::camp_type k);
inline int reach(bc::camp_type k) { return camp_stat(k).range; }
int reach_at(vec2 tile); // range of the camp spawned on `tile` (3 off camp tiles: champions); a cached tile table
// min over pts of cheb(q, pt) - reach_at(pt): < 1 inside someone's range, 1 = on the ring. Callers write
// `SWITCH ? slack(q, pts) + 3 : min_dist(q, pts)`, which is the old distance whenever every range is 3.
int slack(vec2 q, const std::vector<vec2>& pts);
// escape_danger's reach: ESC_RANGE each camp's own range; MONO_NOSIEGE alone only the monolith's 5; else 3
inline int esc_reach(vec2 t) { const int r = reach_at(t); return ESC_RANGE || r >= 5 ? r : 3; }
int esc_slack(vec2 q, const std::vector<vec2>& pts); // slack with esc_reach
bool camp_live_at(vec2 tile);                           // a camp believed alive on its spawn tile (W's knowledge)
int group_reach(int gi);              // largest range of the group's attacking members (3 if none)
bool exit_due(vec2 p, float hp);      // ESC_EXIT: hp loss since my last turn x rounds to leave the awake camps' range >= hp
bool unseen_escape(vec2 p, float hp, bool no_enemy, int target); // UNSEEN: true when it moved me out of a hidden reach
float siege_vs(int slot, int gi);     // design damage per round on group gi from outside its range (0: monolith)
float siege_of_vs(int squad, int gi); // alive members of squad (-1: whole team)
