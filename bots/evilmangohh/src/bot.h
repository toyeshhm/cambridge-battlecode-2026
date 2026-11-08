#pragma once
#include <algorithm>
#include <limits>
#include <vector>
#include <cambc.h>
#include "world.h"
#include "actions.h"
#include "mono.h"
#include "zreach.h"

// Champions-needed gate (i-opening 4.6): the camps hit their nearest champion, so the damage a party of k takes while
// killing a group strongest-first must fit in GATE_HP of its tank's health (two tanks once 2+ monsters fight).
inline constexpr float GATE_HP = 0.75f;
// i-opening switches, one per rule (A/B variants: runs/i-opening/var.sh NAME FLAG=0 ..)
inline constexpr bool OP_GOLD = true;  // 4.7 gold worth 0.3 gems before r300, 0.15 after (was 0.08 / 0.05 side jobs)
inline constexpr bool OP_NEED = true;  // 4.6 need gate on the tank's health (was 850 hp per champion, 60 damage)
inline constexpr bool OP_CAPT = true;  // 4.6 a capture adds 7 rounds to a job (was 13)
inline constexpr bool OP_SIEGE = true; // 4.5 no range-4 siege of normal groups
inline constexpr bool OP_EST = true;   // 4.4 respawn estimated from an ally seen capturing
inline constexpr bool OP_TANK = true;  // 4.5 a ranged champion waits only for a melee that claims its group
inline constexpr bool OP_T2 = true;    // T2: no second capturer next to an ally on the reward tile
inline constexpr bool OP_WALK = true;  // a far "wait" walks the group field
inline constexpr bool OP_SHOP = true;  // 4.3 recall on the capture tile to shop: payout gold >= 150, or >= 90 / hp < 80% near home
inline constexpr bool OP_PRE = true;   // 4.4 a squad leaves a target it has not woken for one 1.5x better (a respawning near group)
inline constexpr bool OP_MARG = true;  // 4.5 a started group counts for side jobs only if the claimants can finish it; no wait reset by chip damage
inline constexpr bool OP_FAR = true;   // 4.1/4.8 elk pairs more than FAR_D steps from spawn are no target before FAR_R
inline constexpr int TRIB_FULL = 70, TRIB_NEAR = 20; // tribute at this bag / this bag when an altar is within TRIB_RAD
inline constexpr int TRIB_RAD = 10;   // altar distance (altars field) for the TRIB_NEAR tribute
// k-hold D1, standoff breaker: STUCK_N turns in a row held still by a declined fight's avoid mask (no attack, no cast)
// while not ahead on banked gems (or ahead while they still bank) -> tribute at another altar for ALT_R rounds, or
// skip the farm target for SKIP_R rounds. Corners: both teams' carriers sat 2-3 steps from the centre altar r200-r2000.
inline constexpr bool UNSTICK = true;
inline constexpr int STUCK_N = 20, ALT_R = 200, SKIP_R = 60;
// o-altar (notes/v9-ab.md "cheap extra" + W2 2b), one switch per fix:
// 4a: 20+ gems next to an unlocked altar: tribute before pvp::turn if the C1 test passes (dps x ticks < hp, enemies
//     within 4). nt4-corners: Ryth stood next to the open centre altar with 94 gems r522-529 while pvp "fought".
inline constexpr bool TRIB_FIRST = true;
// 4b: at an enemy-locked altar with an enemy within 7, walk to the nearest other altar (skip this one LOCK_R rounds)
inline constexpr bool LOCK_WALK = true;
inline constexpr int LOCK_R = 100;
// 4c: endgame bag cap END_BAG once they banked END_THEIRS or our banked + carried reaches END_OURS
inline constexpr bool END_CAP = true;
inline constexpr int END_THEIRS = 700, END_OURS = 900, END_BAG = 10;
// 4d: walk into submit range and start the tribute in the same turn. Platform v9 replays: the top teams do it on 66% of
//     their tributes (536 of 809 in 40 games), we never did (0 of 510): one idle turn next to the altar per tribute.
inline constexpr bool TRIB_STEP = true;
// 2b: on a reward tile at the end of last turn: no hp < 60% retreat, pvp plans stay on the group's tiles unless lethal
inline constexpr bool PEND_HOLD = true;
// v13 ports from bots/v8 (V8_HUNT is in pvp_plan.h; V8_GATE not ported: harmful locally, bots/v8/CHANGES.md)
inline constexpr bool EC_HERD = true;     // l-econ W2 2c: OP_SIEGE's HERD_R 150 -> 80 (elk herds sieged from r80)
// tribute_now: skip enemies of the turn-start list that left vision after TRIB_STEP's walk (else position() throws
// engine error 97 and the turn ends without the tribute). v8 gate: -0.9 SE; q-act gate: -1.73 SE; v13: +1.02 SE (1974
// paired games, 27 vs 20 flips). All three together: 43 vs 47 flips, noise; kept on as the bug fix.
inline constexpr bool TRIB_GUARD = true;
// q-act (action economy, platform v9 replays scout/ab/v6; scout/act_audit.py). A capture pays 6 rounds after the first
// round end with an ally on a spawn tile, so every round between the kill and stepping on delays the payout and the respawn.
// F2: a melee kill (the attack leaves the move) steps onto the freed reward tile in the same turn. Top teams are on the
//     tile at the end of the kill round after 52% of their melee kills, we after 16% (54% one round later).
inline constexpr bool CAP_STEP = true;
// F3: of the allies in the same gold bucket, the one nearest the reward tiles captures (was: lowest draft slot, so
//     Stellar 3 tiles out walked in while a melee stood next to it: our ranged kills pay 3+ rounds late 59% of the time).
inline constexpr bool CAP_NEAR = true;
// Keep CAP_STEP and CAP_NEAR together: CAP_STEP alone measured -32 of 1692 (z -1.55, banked by r400 -7 gems; cause not
// found), the pair with DASH_WALK +58 (z +2.58, banked by r400 +26). With CAP_NEAR the killer is the capturer far more often.
// u-squad CAP_ONE: one capturer per kill. A champion on a reward tile yields to an ally recalling on another tile of the same
// group (OP_T2 skipped the check whenever I was on a tile myself, so a passer-by split the payout: 100 split payouts in 752
// local games, 150 with ONE_SQUAD).
inline constexpr bool CAP_ONE = false;
// r-round1 R1_ACT (world.h): one step toward the target in round 1 when W.init left < 90k (Bot::first_step)
inline constexpr int R1_PLAN = 45000, R1_STEP = 25000;
// x-stamp (sprint2-plan #4 cut down by its critique 7.2). Both rules act on the stampede herd only: stamp_herd() is the
// elk group with str >= 240 on a herd map that is not contest (herd_map is also true there), -1 elsewhere; of the 17
// official and 30 generated maps only stampede has one (runs/x-stamp/tools/herdmaps.py).
// STAMP_HOLD: while my target is the live herd and no enemy champion is in my vision, the hp retreat (0.33, or 0.6 after
//   a skirmish) and the hp < 0.5 shop recall do not fire: I hold the ring at 4 from every live elk (farm's wait ring,
//   no melee) and cast from there; elks never hit at 4, so there is no hp floor. Debug vs a sparring bot: four of five left the
//   herd at 1.7k hp at r100-105.
inline constexpr bool STAMP_HOLD = true;
// c8 STAMP_FIN (v34 benchmark b8127061, 0-1120, 100 fuel-outs): we siege the herd from the ring (16080 hp at r480 -> 5908 at
//   r660), their Vesper walks in, our hurt ring-holders retreat and she takes 160 gems + 824 gold; the same at r960 and r1120
//   (7 of 7 herds theirs). Once the herd's live hp is <= STAMP_FIN % of its max: no ring hold (melee it) and no hp retreat
//   above 0.2 or skirmish shop recall while it is my target, so it dies to us. 0 = v34.
inline constexpr int STAMP_FIN = 0;
// c8 GEM_MAR (v34 benchmark: adgato_gemcombat 1-5 1/7; gemcombat4 e90ab384 0-1001 in 403 rounds): on maps with no gold
//   camps the only gold is kills (100) and Marcella's tribute (2 a gem). Their Marcella earned 1614 by r403 (gold@300 1402 vs
//   our 100); ours died at r58 holding 63 gems, the team 13 deaths holding 28-77. On those maps Marcella's bag cap is GEM_MAR
//   (gold, then shards, early). 0 = v34.
inline constexpr int GEM_MAR = 0;
// c8 UNBLOCK (local c7mx vs starter-cpp: 00vault 351-4 / 448-160 at r2000; four champions stand inside the 7x7 vault of gem
//   clusters (12-18,12-18) from r300 to r1900 waiting for its respawn, which no one can get while they stand on its tiles).
//   farm()'s unblock step only looks at the 8 neighbours, all spawn tiles inside the block, so no one moves. On: when it finds
//   none, walk the shortest way to a free tile next to the group (BFS from those tiles). 0 = v34.
inline constexpr bool UNBLOCK = true;
inline bool step_off(const bc::bitboard& spawns) {
    bc::bitboard roots;
    roots.clear();
    for (auto t : spawns.positions()) for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
        const vec2 q{t.x + dx, t.y + dy};
        if (bc::map::is_in_bounds(q) && !spawns.is_set(q) && !W.static_walls.is_set(q)) roots.set(q);
    }
    if (roots.is_empty() || bc::fuel_remaining() < 20000) return false;
    Field f;
    f.compute(W.static_walls, roots, W.visited);
    return step_field(f);
}
// c8 SPAWN_SIEGE (03homonoliths, a new ranked map: 3v3, all 1200 gems in two monolith triples, each in a corner touching a
//   spawn; local v34 vs starter-cpp 400-1000 / 0-1000, and starter wins because its Vesper and Varg stand on their spawn
//   tiles next to the triple and hit it, healed by the spawn, until it dies at r383: 600 gems). Contest-kind maps only
//   (W.boss_map): with a bag < 20 and no enemy within 6, a champion on a spawn tile hits a live monolith within its attack
//   range, or steps to the spawn tile nearest one. Of the 70 + 14 maps only 03homonoliths has such a monolith. 0 = v34.
inline constexpr bool SPAWN_SIEGE = true;
inline bool spawn_siege() {
    auto& me = bc::self();
    if (me.gem_count() >= 20) return false;
    const auto& sp = bc::map::spawn_region(team::ally);
    const int r = me.attack_range();
    std::vector<bc::camp> mono;
    std::vector<vec2> mp;
    for (const auto& c : W.camps) try {
        if (c.kind() != bc::camp_type::monolith || c.health() <= 0) continue;
        for (auto s : sp.positions()) if (cheb(s, c.position()) <= r) { mono.push_back(c); mp.push_back(c.position()); break; }
    } catch (...) {}
    if (mono.empty()) return false;
    const vec2 p = me.position();
    if (min_dist(p, mp) > r)
        best_step([&](vec2 q) -> float { return sp.is_set(q) ? static_cast<float>(min_dist(q, mp)) : std::numeric_limits<float>::infinity(); });
    cast_on_camps(mono, true);
    attack_camp(mono);
    return true;
}
inline bool gem_only() { static const bool v = std::none_of(W.groups.begin(), W.groups.end(), [](const Group& g) { return g.gold > 0; }); return v; }
// v20 ships EMB1 = 9 (Prospector; vs v19 z +2.70, +3.41 pooled over 8 fuel draws). v21: EMB3 = 6 (Bruiser, vs v20 z +4.23).
// c4 emblems (patch 2.0.0): the item family (cambc::item_family index) of the build's first / second five-shard step, same
// stat as before; -1 keeps v19's (Bruiser or Power, then Durability). 0 monster, 7 tribute, 8 capture, 9 prospector.
// EMB3: the same for the last, repeating step (v19: Power); 12 greed (25 shards combine into 35 shards' worth)
// v29 tried EMB1 = 4 (Jungle, 30% less damage from camps; the top 10's most common first shard). Local vs v28 even
// (138/280; Treasure 128/280). Platform: camp damage taken -21% but gold a champion 736 -> 693 without Prospector, 3/100
// wins (v28 5), paired deaths +0.29: worse, back to 9.
inline constexpr int EMB1 = 9;
inline constexpr int EMB2 = -1;
inline constexpr int EMB3 = 6;
// MAR_BUILD (notes/fights-oct30.md): the top teams' Marcella ends with Strength 15-17, Health 18-21, Focus 19-26 shards;
// ours Focus only. 0 = v28 (Bruiser Health, Focus, Focus forever); 1 = Bruiser Health, Strength, Focus, Strength, Health,
// then Focus forever.
inline constexpr int MAR_BUILD = 0;
// BUILD_F (notes/top5-econ-nov1.md s3): the damage stat first. The top 5's first 300 gold of shards: Marcella Focus 83%, HoR
// Focus 62%, Ren Focus 54%, Varg Strength 88%; v28 Health (Ren, HoR, Ryth 100%; Marcella 58%), so at r300 our Marcella has
// 60 focus vs 179. EMB1 / EMB3 still pick the families. 1: Marcella Focus, then Focus forever (was Health first). 2: also HoR
// Focus, Durability Focus, Focus forever; Ren Focus, Durability Health, Focus forever; Varg's second step Strength; Ryth's
// last step Health. 0 = v28.
inline constexpr int BUILD_F = 1;
// FUNNEL (notes/top5-econ-nov1.md s2): captures are solo, so the capturer takes the whole reward. a top team's Marcella takes 32% of
// the captures (10.4 kills a game), another's Vesper 38%; v28's Marcella 18% (the lowest-gold ally captures). On: a Marcella
// near a reward captures it ahead of every ally (an ally already on a tile still keeps it); the others leave it to her.
inline constexpr bool FUNNEL = true;
// c7e BUILD_R (top5-econ-nov1 s3; BUILD_F 2 without HoR, whose Focus-first tank build raised its deaths 1.37 -> 1.88 on
// c6e's screen): 1 = Ren Focus first (Prospector F > Durability H > Bruiser F; was Health first) and Varg Durability
// Strength on step 2 (was Health). 0 = v31.
inline constexpr int BUILD_R = 0;
// c7e LATE_BAG (losses-nov1 s5 #5): after round LATE_R the bag cap is at most LATE_BAG (their median tribute 25, ours 34;
// close races end with 46 gems carried vs their 20). 0 = v31.
inline constexpr int LATE_BAG = 25;
inline constexpr int LATE_R = 600;
// c7e WAIT_ALONE (losses-nov1 s3 #8; v31 benchmark: camp waits 127 champion-rounds a game, the top 10 63; 2.4 waits of 8+
// rounds a game). A wait at a group no other ally claims (a PAIR_N first claim nobody joins: 01the-farm's Marcella r7-r38
// while HoR and Ren took the need-2 group) gives the group up after WAIT_ALONE turns instead of 30. 0 = v31.
inline constexpr int WAIT_ALONE = 0;
// WAIT_ALL: WAIT_ALONE only at a group that needs every ally (need >= team size: the others must all drop their jobs)
inline constexpr bool WAIT_ALL = false;
// GATE (c7m, notes/losses-nov1.md s3): altars walled in by camps. 02the_pack's only altar sits in a ring of 48 wolves (one
// group, 64800 hp): the static fields ignore camps, so every bag walks to the ring and stands there (platform 9e53d2ef r400-
// r2000; local c6 vs v28 0-0 both sides). Dead members stay dead until the whole group is captured, so one dead wolf opens
// the ring for good. gate.cpp finds the cheapest gate (geometry only); a tribute trip hits it, then walks the opened path.
inline constexpr bool GATE = true;
inline constexpr float GATE_HP_MAX = 2400.f; // gates up to an elk; monolith seals stay SEAL_* / MONO_TAKE's (gate_detect)
// PUSH_R (c7m, losses-nov1 #7): UNSTICK's altar skip is a no-op with one altar (skip_altar finds no other), so a declined
// fight's avoid mask held our bags 2-4 from the only altar to r2000 (02the_pack with the gate open: Ren on the dead gate
// tile, 79 gems, enemies 5-7 away). Then (not ahead, or they banked meanwhile) the tribute walk ignores the mask for PUSH_R
// rounds; tribute_now's C1 test still decides the start in range. 0 = off.
inline constexpr int PUSH_R = 0; // c7m: DROPPED (dormant locally, see VERSION.md); 30 = the screened c7gp
inline int& push_until() { static int v = 0; return v; } // c7m PUSH: tribute walks ignore the avoid mask until then
// c18b (notes/bughunt-fresh-nov3.md, bughunt-ranked-nov3.md). All false = v58 (d8d976b0); v67 and v70 ship GATE_WALK and WAIT_FUEL.
// GATE_WALK: gate_exit walks out instead of recalling while an awake camp has me in reach, or within 2 rounds of a recall
// that was cut (the_pack local mirror: Marcella recalled r510-528 at (21,23), each cut by 3 awake wolves, died r529).
inline constexpr bool GATE_WALK = true;
// WAIT_FUEL: farm()'s wait ring dedupes dz with a bitboard and skips the ring step when already on the ring and no ring tile
// can score better (the step never moves there). Same decisions; the_pack's 46-wolf ring cost 130-165k a turn (fuel-outs).
inline constexpr bool WAIT_FUEL = true;
// The RING step with only the danger tiles that can be nearest for me or a neighbour (slack changes by <= 1 a step: one 2+
// past my own slack never is, so the scores are the same), and no step on the ring when no tile can beat mine.
inline bool wait_step(vec2 p, const std::vector<vec2>& dz, const std::vector<vec2>& sp, int ar, bool hit) {
    const int s0 = slack(p, dz);
    const int ds = hit ? min_dist(p, sp) : 0; // a ring tile scores -1 when hit and a soft member is in reach from it
    if (s0 + 3 == 4 && (ds <= ar || ds > ar + 1)) return false; // on the ring, no neighbour can score better: no move
    std::vector<vec2> nz;
    for (vec2 m : dz) if (cheb(p, m) - reach_at(m) <= s0 + 2) nz.push_back(m);
    return best_step([&](vec2 q) -> float {
        const int d = slack(q, nz) + 3;
        if (d < 4) return 1000.f - d;
        if (d > 4) return static_cast<float>(d);
        return hit && min_dist(q, sp) <= ar ? -1.f : 0.f;
    });
}
// SCARCE: the groups' gem_value sums to < 20 (03one_gem 7; next 03ultrastarvation 36, 00fragment 68, 01elk-arena 72,
// 01hooks 88: runs/nov6/c18b/bot-gem-totals.txt): every gem is the r2000 tiebreak, so plans ignore gold (gold_w 0.005)
// and every bag of 1+ goes to the altar. (< 100 also fired on fragment / elk-arena / hooks.)
inline constexpr bool SCARCE = false;
inline bool gem_scarce() { static const bool v = [] { int t = 0; for (const auto& g : W.groups) t += g.gems; return t < 20; }(); return v; }
// c19j (bughunt-fresh-nov3 #4, bughunt-ranked-nov3 #4). JACKPOT 0 = v67 (78e67a8c with GATE_WALK, WAIT_FUEL).
// JACKPOT: a bag of JACKPOT+ gems (600 = a 60-round channel) starts its tribute only at a safe moment (jp_start), and CANCEL
// cuts it only when the enemies within 4 would kill me within JP_WIN rounds, not within the whole channel (03homonoliths:
// Ren started 8 tributes with 600 in r356-516, each cancelled by CANCEL's dps x 60 rounds, and died with the bag at the altar).
inline constexpr int JACKPOT = 0;
inline constexpr int JP_SAFE = 8;  // safe: no enemy champion seen this close
inline constexpr int JP_WIN = 5;   // CANCEL's window for a jackpot (rounds)
inline constexpr int JP_WAIT = 10; // rounds after the first hold before an unsafe start
inline int g_bag = 0; // my gems at turn start (play_turn), so the off-jackpot turns query nothing new
// Safe = no enemy champion seen within JP_SAFE and, if an ally is seen within JP_SAFE, one within 3 (allies near if
// available). JP_WAIT rounds after the first hold (fights in between included: a hold counted in a row was reset by every
// pvp turn, and the bag never left vs starter-cpp on 01boss-battle), start anyway unless the enemies within 4 kill me
// within JP_WIN rounds.
inline bool jp_start() {
    auto& me = bc::self();
    static int from = -1;
    if (g_bag < JACKPOT) { from = -1; return true; }
    const vec2 p = me.position();
    bool enemy = false, ally = false, guard = false;
    float dps = 0;
    for (const auto& e : bc::vision::champions(team::enemy))
        try { if (e.is_visible() && e.is_alive()) { const int d = cheb(p, e.position()); enemy |= d <= JP_SAFE; if (d <= 4) dps += champ_dps(e); } } catch (...) {}
    for (const auto& a : bc::vision::champions(team::ally))
        try { if (a.is_visible() && a.is_alive()) { const int d = cheb(p, a.position()); if (d > 0) { ally |= d <= JP_SAFE; guard |= d <= 3; } } } catch (...) {}
    if (!enemy && (guard || !ally)) { from = -1; return true; }
    if (from < 0) from = W.round;
    return W.round - from >= JP_WAIT && dps * JP_WIN < me.health();
}
// SF_STARVE (actions.h): step_field's window BFS skipped for fuel sets sf_starved(); farm() then keeps its target (no ~17k
// re-plan) so the next turn affords the BFS, whose roots are now only window tiles reachable from me
void gate_detect();          // main.cpp: once, at a turn end with fuel to spare
bool gate_turn();            // bot.cpp tribute branch: the gate stands, go hit it (true = turn handled)
const Field* gate_field();   // combat.cpp tribute(): the walk through the dead gate, nullptr = the usual fields
bool gate_exit();            // bot.cpp farm branch, retreat(): inside the wall with the gate dead, recall out (true = handled)
template <class Plan> Plan emblems(Plan v) {
    auto fam = [](cambc::item_type it, int f) {
        const auto x = static_cast<std::uint32_t>(static_cast<cambc::item_type::enum_type>(it));
        return cambc::get_item_enum(static_cast<cambc::item_family>(f), static_cast<cambc::item_stat>(x % 6 / 2));
    };
    if constexpr (EMB1 >= 0) v[0].item = fam(v[0].item, EMB1);
    if constexpr (EMB2 >= 0) if (v.size() > 2) v[1].item = fam(v[1].item, EMB2);
    if constexpr (EMB3 >= 0) v.back().item = fam(v.back().item, EMB3);
    return v;
}
// STAMP_BACK: a herd last seen as a reward (and not seen captured by an ally, OP_EST) is taken as captured then: respawn
//   at last_seen + 6 + respawn, full hp. score_of's siege term used the reward's hp 0 from time_of but max hp itself, so
//   the herd scored < 0 for the rest of the game once its leader had seen a reward and walked off (debug vs
//   two sparring bots: herd -1.5 to -3.6 from r167/r183 to r450; gold-only groups 0.2-1.2), and a visible reward
//   scored < 0 too. Now both terms use the same hp: 0 for a reward in sight, max hp for an unseen one.
inline constexpr bool STAMP_BACK = true;
int stamp_herd(); // plan.cpp: the stampede herd's group index, -1 on every other map
// z-boss (notes/pool-oct19.md lever 2). A SEALED map, from geometry only: every standable tile in submit range of every
// altar is a monolith spawn tile (01boss_battle: the one altar (25,20) is walled and holed in; its one free neighbour (24,20)
// holds one of a 400-gem monolith pair, and the pair is the only link between the halves). rr-c1 walks every bag there and
// waits next to the monolith for the rest of the game (debug vs idle: 4 champions in `tribute` by r300, all 6 by r1500, 3995
// gold earned; platform 1/5, three gold-tiebreak losses). Of the 21 pool, 17 official and 30 generated maps only
// 01boss_battle is sealed (runs/z-boss/tools/sealed.py). seal_detect (seal.cpp) runs once, as the last code of a turn
// (main.cpp, after the ward init), where no decision of that turn sees its fuel; elsewhere every rule reads g_seal == 2.
// SEAL_NOPARK: while the seal stands (my last look at it, and no gems banked by either team since), no tribute trip and the
//   bag does not hold back the shop recall; once open (a sealing monolith seen dead with its group asleep, or its group dead,
//   or anyone banked since), every bag is banked. Groups reachable only through the seal are never targets (seal.cpp).
inline constexpr bool SEAL_NOPARK = true;
// SEAL_GOLD: target values are gold + SEAL_GEM_W x gems (the 2000-round tiebreak after banked gems is lifetime gold)
inline constexpr bool SEAL_GOLD = false;
inline constexpr float SEAL_GEM_W = 0.25f;
// SEAL_SIEGE: the sealing group is no squad's target; from round SEAL_R the squad leaders put the whole team on it
//   (range-4 siege from the ring, help flag), then capture and bank
inline constexpr bool SEAL_SIEGE = false;
inline constexpr int SEAL_R = 1200;
inline int g_seal; // 0 not detected yet, 1 no seal, 2 sealed (seal_detect); inline: no storage in the all-off build
void seal_detect();  // main.cpp: once, at the very end of a turn with fuel to spare
bool seal_closed();  // g_seal == 2 and the seal not known open (refreshes my knowledge from W: call after W.update)
int seal_group();    // group of the first sealing tile, -1 none
void seal_turn_end(); // main.cpp, sealed map: groups behind the seal never targets (NOPARK); the seal itself only for the whole team from SEAL_R (SIEGE)

// y-contest (sprint2-plan #6 as cut down in 7.5 #5). Contest only (W.boss_map: every gem group is a monolith). Off values
// MONO_R 0, BOSS_GOLD_W 0.25f, MONO_SAFE false build rr-c1's binary (md5 ed9fab75); set: the judged build, MONO_R only (a0fda645). Platform: we capture the first monolith at r240-328 and feed 2.2 bags a game to
// their killers; top teams take monoliths from r976 and many games end on the gold tiebreak.
// MONO_R (0 = off): a monolith is a siege target only from round MONO_R, or while its live hp is under 30% (a kill someone
//   else prepared). A monolith reward is always a target. Before MONO_R the team farms the gold groups.
inline constexpr int MONO_R = 900;
// BOSS_GOLD_W: gems per gold in contest target values (rr-c1 0.25; gold is the tiebreak and the only income before MONO_R).
//   Judge (runs/yc-judge): 1.0 on top of MONO_R 900 loses monoliths after r900 (ours -0.48, theirs +0.46 a game on fresh
//   boss maps, -10 and -17 flips in two draws; 00contest 0 flips, theirs +0.38). DROPPED: back to rr-c1's 0.25.
inline constexpr float BOSS_GOLD_W = 0.25f;
// MONO_SAFE: step onto a monolith reward tile (start the capture) only with min(2, alive - 1) allies within 4 of it and no
//   visible enemy champion within 7 of it or of me. Otherwise every champion near it holds (no capture, no moving on: debug
//   showed the siege party walking off to the gold right after the kill, so the condition never came back and a sparring bot took
//   our kill) for up to MONO_HOLD rounds, then leaves it. A capture already started (an ally on the tile) is rr-c1's.
inline constexpr bool MONO_SAFE = false;
inline constexpr int MONO_HOLD = 10;
bool boss_skip(const Group& g); // contest.cpp: time_of's contest filter with MONO_R

// x-expanse EXP_HPCHK: solo_target found jobs only my hp ruled out (expanse.cpp). A global, not a Bot member: a member
// initialiser changes Bot's constructor, so the all-off build would no longer be rr-r1's binary.
extern bool g_heal_want;

// Per-champion state survives between turns (each champion has its own bot instance).
class Bot {
    bool inited = false;
    int target = -1;
    int wait_turns = 0;
    float last_hp = -1; // target group's hp last turn (siege progress)
    int build_step = 0;
    int bag_since = -1; // round the current gem bag got its first gems
    bool trip = false;  // decided to tribute this bag
    int prio = 0;         // turn priority currently set (farm or combat order)
    int commit_until = 0; // keep a side job (target != squad target) until this round
    int cap_blocked = 0;  // turns a capture walk made no progress; then that group is left for 40 rounds
    int cap_skip_group = -1, cap_skip_until = 0;
    float need_scale = -1; // stats scale the cached group needs were computed for
    float dps_scale = 1, hp_scale = 1; // our strength / health relative to base stats
    int stuck = 0, stuck_eb = 0;    // k-hold D1: stuck turns in a row, enemy banked gems when it started
    unsigned alt_mask = 0;          // altars (bit = index in W.altars) the breaker skips until alt_until
    int alt_until = 0;
    Field alt_field;
    void unstick(vec2 p0, bool trib);
    bool skip_altar(int bi, int rounds); // mask altar bi, alt_field = the rest; false if none is left
    bool tribute_now(const std::vector<cambc::champion>& enemies); // o-altar 4a / 4d
    const std::vector<cambc::champion>* turn_enemies = nullptr;      // this turn's visible enemies (play_turn)

    void shop();
    void update_needs();
    void choose_target();
    void solo_target(); // x-expanse EXP_SOLO (expanse.cpp)
    bool hold_ok(const std::vector<cambc::camp>& members, int k) const; // x-expanse EXP_HOLD (expanse.cpp)
    void first_step(); // R1_ACT
    int cap_group = -1, cap_round = -1; // reward group whose tile I stood on at the end of turn cap_round
    bool capture(const std::vector<cambc::champion>& allies, bool safe);
    bool home_after_capture(int gi) const;
    int bag_cap() const; // tribute at this bag (f-bank B1)
    void retreat(const std::vector<cambc::champion>& enemies, const std::vector<cambc::camp>& camps);
    bool tribute();
    void farm(const std::vector<cambc::camp>& camps, const std::vector<cambc::champion>& allies);
    bool feasible_now(const std::vector<cambc::camp>& members, int k) const;
    bool tankable(const std::vector<std::pair<float, float>>& hs, int k) const; // (hp, str), strongest first
    int ranged_reposition(const std::vector<cambc::camp>& members, const std::vector<cambc::champion>& allies, cambc::vec2 npos);
    void fight(const std::vector<cambc::camp>& members, const std::vector<cambc::champion>& allies);
    // boss maps (contest.cpp): the carrier of a big bag waits at spawn for an escort; allies answer the call
    const char* boss_bag(int gems, bool at_spawn, bool safe, const std::vector<cambc::champion>& allies);
    const char* boss_escort(bool at_spawn, bool safe);
    int mono_gate(int gi, const std::vector<cambc::champion>& allies) const; // y-contest MONO_SAFE (contest.cpp)
    static bool mono_hold(int gi); // y-contest MONO_SAFE: still within MONO_HOLD rounds of waiting at gi (contest.cpp)
    bool mono_lead();                                                   // p-monolith MONO_TAKE (mono.cpp): a running take = target
    const char* mono_turn(const std::vector<cambc::champion>& enemies); // p-monolith MONO_TAKE: the take's turn, nullptr = none

public:
    void play_turn();
    void mono_end(); // p-monolith MONO_TAKE (mono.cpp): squad 0's leader starts a take, after every decision of the turn
};
