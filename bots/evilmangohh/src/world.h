#pragma once
// Per-champion world model: static map data, camp-group knowledge and cached distance fields.
#include <cambc.h>
#include <array>
#include <memory>
#include <cstdint>
#include <vector>

namespace bc = cambc;
using bc::vec2;
using bc::team;

constexpr std::uint16_t FAR = 65535;
// r-round1: W.init leaves < 90k of the 200k first turn on maps with 25+ groups (corners, expanse, lanes, monolith, pilgrimage,
// swirl: 100-153k of init), and play_turn gave round 1 up, so the whole team idled. R1_ACT: take one step toward the target
// instead (choose_target when R1_PLAN fuel is left, else the squad anchor's group), along a BFS cut at my tile; no step with
// an enemy within 8, within 6 of the group, or under R1_STEP fuel (bot.h). The >= 90k path is unchanged.
inline constexpr bool R1_ACT = true;
// u-squad F4 (opening tempo): by r100 top teams kill 4.3 gem groups to our 3.0, 46% of them with 4+
// hitters. ONE_SQUAD: a team that would field two squads plays as one (the whole team, squad 0) until round SPLIT_R, then
// takes the planned two squads (World::update). Every instance sees the round, so all switch together; squad 1's target
// byte is empty until then and its leader picks fresh (a squad-1 member in a fight in progress keeps it, plan.cpp).
inline constexpr bool ONE_SQUAD = true;
inline constexpr int SPLIT_R = 60;
// SQ_STACK: while the team plays as one squad, a member leaves the squad target for a side job only once SQ_STACK_X x its need
// is already there (top teams' parties at base stats: wolves 2, a single elk or Au+E 4-6; ours stop at the need, 1 and 2).
inline constexpr bool SQ_STACK = true;
inline constexpr int SQ_STACK_X = 2;
// SQ_MAX_OWN: one squad only on maps with at most SQ_MAX_OWN own gem groups (0: every two-squad map). Formal draws A+B of
// ONE_SQUAD + SQ_STACK: wins and s300 up on the 4-6 group maps (ambush, eight, gen-03, gen-21, interlock), down where
// two squads have many near jobs (corners 12 groups, gen-06 11, gen-05 9, monolith 7, gen-26 18). Chosen on A+B: judged on
// fresh draws C+D only (bots/u-squad/CHANGES.md).
inline constexpr int SQ_MAX_OWN = 6;
// SQ_FAR_D: one squad only on maps whose nearest own gem group is at least SQ_FAR_D (Chebyshev, spawn centre) away (0: every
// two-squad map; 20: ambush 39 and lanes 33, every other map <= 15). The screens lost early kills where two squads have near
// jobs (gen-06 2.3 vs 5.2 kills by r100, gen-05, corners, interlock) and gained them where the first jobs are far (ambush
// 2.0 vs 1.0, lanes 5.5 vs 3.4).
inline constexpr int SQ_FAR_D = 0;
// Screened and dropped (CHANGES.md), kept for A/B: SPLIT_CAP splits at the first change of the team target from SPLIT_MIN on
// (SPLIT_R = latest; an instance that acted before the leader splits a round later); SQ_PICK_K scores the one-squad leader's
// kill times as for SQ_PICK_K bodies (0: the whole squad); SQ_SIDE_R is the stacked phase's side-job range (0: SIDE_RANGE).
inline constexpr bool SPLIT_CAP = false;
inline constexpr int SPLIT_MIN = 15;
inline constexpr int SQ_PICK_K = 0;
inline constexpr int SQ_SIDE_R = 0;
// x-expanse: solo loops on race maps where one champion can farm its own groups (expanse: 0/11 on the ladder, r1000 at
// r600-640 unopposed vs the top teams' r446-506). W.solo_map (World::init, geometry only): <= EXP_BODIES bodies, area >=
// EXP_AREA, no boss/herd map, >= EXP_MIN_SOLO own-half gem groups one champion of mean damage tanks at base stats. Then
// every champion picks its own job (Bot::solo_target, expanse.cpp): no squad target, no anchor, claims keep them apart.
inline constexpr bool EXP_SOLO = true;
inline constexpr int EXP_BODIES = 4;
inline constexpr int EXP_AREA = 4000;
inline constexpr int EXP_MIN_SOLO = 0; // 3 = the 11 expanse-like maps only; 0 adds 5 big 3-pick maps (CHANGES.md 5)
// a lone range-4 caster sieges an own group it cannot tank (elk pairs; on expanse top teams send Stellar or Cassi
// alone on the 120-gem elk pair from r53-68), from the ring, shooting its gem clusters from outside elk range
inline constexpr bool EXP_SIEGE = true;
inline constexpr float EXP_SIEGE_W = 1.0f; // siege value weight (plan.cpp SIEGE_W 0.5: a siege also costs no hp, no heal trip)
inline constexpr float EXP_GOLD_W = 0.3f; // gems per gold in solo job values (gold-only groups: EXP_GOLD0_W)
inline constexpr float EXP_GOLD0_W = 0.3f;
// a lone champion on a group it was sieging (need > the champions there) steps into monster range only when the rest
// fits in EXP_HOLD_HP of its CURRENT hp at EXP_HOLD_DPS of its measured camp damage (s2 debug: Stellar left the elk-pair
// ring at r119 with one elk alive, 750 -> 210 hp, recalled and walked 48 rounds back)
inline constexpr bool EXP_HOLD = true;
inline constexpr float EXP_HOLD_HP = 0.5f, EXP_HOLD_DPS = 0.6f;
// rounds a solo champion may wait at a respawning group after arriving (plan.cpp MAX_WAIT 3); the wait counts in the job time
inline constexpr int EXP_WAIT = 3;
// Cassi follows Issac's claim (one pick, two bodies) and Issac plans with their summed damage: solo Cassi/Issac took 33-34
// rounds a wolf and Cassi retreated off her first one (h1 debug r36, wolf alive); Issac takes a group the pair kills
// before it costs EXP_DUO_HP of his current hp
inline constexpr bool EXP_DUO = false;
inline constexpr float EXP_DUO_HP = 0.75f;
// a solo champion keeps its job unless another scores EXP_KEEP times better (choose_target 1.3)
inline constexpr float EXP_KEEP = 1.3f;
// skip a job whose damage share would leave me under EXP_FLOOR of max hp (h1 debug: Cassi re-took a wolf at 473/1056 hp
// and retreated at r182 with it alive); with no job left that way, recall to heal (farm)
inline constexpr bool EXP_HPCHK = false;
inline constexpr float EXP_FLOOR = 0.35f;
// tribute bag on solo maps (bot.cpp bag_cap: TRIB_FULL 70 on big maps; the enemy-seen and endgame caps still apply).
// exp-07 h1 debug: 4 altars 32-38 steps from spawn, 101-136 of 620 rounds in tribute walks for Issac/Stellar/Varg. 0 = off
// Judge (runs/jx-expanse): 140 lost on 19 fresh class maps, -26 / -33 flips in two draws (pooled z -4.57; s300 -21,
// s400 -28); it helps only on expanse (race -15.7 rounds, 2/108 games). DROPPED: off.
inline constexpr int EXP_BAG = 0;
// y-solo: solo loops on more race maps (bots/y-solo/CHANGES.md). Rounds to 1000 vs idle with solo forced on every map
// (77 maps): faster on big maps whatever the body count and on mid maps with an own job per champion, slower on small
// fight maps. Geometry only (World::init); each band is its own switch, all off = rr-c1's classifier (and binary ed9fab75).
// Y_BIG: area >= EXP_AREA with any number of bodies (adds ambush, corners and 5-6-body generated maps). Race -25 rounds
//   (18 maps), -53 on 7 fresh test maps; formal +6 / +27 flips (z +1.92 pooled), fresh test maps +174 of 840 (z +10.3).
// Y_MID: Y_MID_AREA <= area < EXP_AREA and at least one own-half gem group per body (lanes, monolith, rushbait-v2).
//   Race -26 rounds (9 maps and 7 fresh); formal +56 / +70 flips (z +7.11 pooled; fresh maps +109 of 840, z +6.5).
// Y_SMALL: area < Y_MID_AREA and at least one own-half group per body that one champion tanks alone (collision only of the
//   77+30 maps). Race -142 rounds; formal +20 / +9 flips (z +4.64 pooled).
inline constexpr bool Y_BIG = true;
inline constexpr bool Y_MID = true;
inline constexpr bool Y_SMALL = true;
inline constexpr int Y_MID_AREA = 2000;
// Y_BIG_OWN: Y_BIG also needs Y_MID's one own-half gem job per body, else the squads play it. Solo loops send the champions
// one by one to gems that all sit on the midline (ribs: own 1.5 jobs for 5, 0 gems @700 vs 832; the top 10 walk there as
// five, notes/jackpot-oct27.md). Flips ribs, galaxy, ambush, famine. false = v26's binary 979b4a0a. v27 shipped true:
// big-map deaths +2.25 a game (famine 2-4 -> 16-20), ribs still 0 gems (the squad arrives together and loses the 5v5);
// off again, v26 re-activated.
inline constexpr bool Y_BIG_OWN = false;
// Y_LIVE (expanse.cpp solo_target): a started group counts as doable by 1 + claimants when what is left of it is tankable
// (live member hp), so allies join a lone caster's siege instead of waiting for the design need. Race +0.7 +- 1.6 rounds on
// the 39 solo maps, worse than Y_* alone on the fresh test maps: NOT CONFIRMED, off (the off build is rr-c1's binary).
inline constexpr bool Y_LIVE = false;
// PAIR_N (expanse.cpp solo_target): a group of need <= PAIR_N may be claimed before it has need - 1 claimants, scored as one
// share, value / (need x party time); the claimant waits there and the next champion sees need <= 1 + claims. Without it no
// one claims an elk group first on solo maps: v23 takes them at r250-675 unopposed, the top 10 at a median r80-88
// (scout/meta26/farm.py, notes/farm-oct27.md). 0 = off (v23 binary d196d0fd); v25 ships 3.
inline constexpr int PAIR_N = 3;
// PAIR_D: PAIR_N's first claim also needs need - 1 - claimants other alive allies within PAIR_D (Chebyshev) of the group
// centre (a job already held is kept), so the claimant does not stand alone at it: on v25's 20 big-map benchmark games
// 45 of 149 deaths were a champion alone near an elk / golem group (v23: 26 of 135). 0 = off (v25's binary 31a2e085);
// v26 ships 14.
inline constexpr int PAIR_D = 14;
// PAIR_FREE (c7m, losses-nov1 #8): PAIR_D does not count mates busy elsewhere. 01the_farm (3v3, 0/32 ranked): HoR and Ren
// claimed the need-2 wolf group (2-4,12) at r2, then Marcella claimed the need-3 elk group (9-13,2) on their positions alone
// and waited at (8,6) r7-r38 (wait_turns timeout) while they cleared the wolves; first capture r39. 1: a mate with any other
// claim is busy; 2: only a mate on a party job (need >= 2: it cannot leave it); 0 = off.
inline constexpr int PAIR_FREE = 2;

inline int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

// Static distance field kept in guest memory: get() is an array read, the engine's get() a 43-fuel host call
// (planners and best_step read hundreds of tiles a turn). Off-map reads FAR.
struct Field {
    std::unique_ptr<std::uint16_t[]> d;
    int w = 0, h = 0;
    void compute(const bc::bitboard& walls, const bc::bitboard& roots, bc::bitboard& visited);
    std::uint16_t get(vec2 p) const {
        return static_cast<unsigned>(p.x) < static_cast<unsigned>(w) && static_cast<unsigned>(p.y) < static_cast<unsigned>(h) ? d[p.y * w + p.x] : FAR;
    }
};

struct Group {
    std::vector<vec2> tiles;      // original spawn tiles of members
    std::vector<bc::camp> members; // engine camp handles, same order as tiles
    std::vector<float> member_hp; // design max hp per member
    std::vector<float> member_str;
    std::vector<std::pair<float, float>> by_str; // (hp, str) per member, strongest first (update_needs)
    bc::bitboard spawns;
    vec2 center;
    int gems = 0, gold = 0, respawn = 0;
    std::uint32_t handle = 0; // engine group handle (raw state query: map::camp_group::state() re-reads vision, ~920 fuel)
    float max_hp = 0, str = 0;
    bool harmless = false, monolith = false;
    bool enemy_side = false; // centre closer to the enemy spawn than to ours
    int need = 1;          // champions needed to fight it safely (Bot::update_needs, before any use)
    // knowledge (this champion's own observations)
    int respawn_at = 0;    // round when alive again; 0 = alive/unknown
    int est_respawn = 0;   // i-opening: respawn estimated when an ally was first seen capturing (0 none)
    bool reward = false;   // reward available when last seen
    bool awake = false;    // some member awake when last seen
    int last_seen = -1;
    int skip_until = 0;    // personal skip after failed waiting
    float hp_seen = -1;    // total remaining hp when last seen
    std::vector<float> hp_member; // last known hp per member (design max until seen)
    bool field_ready = false;
    Field field;
};

struct World {
    int me_idx = 0, n_allies = 1, round = 0;
    std::vector<Group> groups;
    bc::bitboard static_walls;   // walls | holes | altars | enemy spawn
    bc::bitboard visited;
    Field altar_field, spawn_field;
    bool altar_ready = false, spawn_ready = false;
    std::vector<vec2> altars;

    void init();
    void place_anchors();
    std::array<vec2, 2> anchor{};     // per squad home area
    vec2 home_c{}, away_c{};          // centres of our / the enemy spawn (place_anchors; V8_HUNT)
    void update();                       // refresh knowledge from vision
    void precompute();                   // static distance fields with spare fuel (end of turn)
    int group_of_spawn(vec2 p) const;    // group index by member spawn tile, -1 if none
    const Field& group_field(int gi);
    const Field& altars_field();
    const Field& home_field();
    int dist_to_group(int gi, vec2 p) const; // BFS steps from p to nearest member tile
    // Spawn tiles of the group's fighting members believed alive (monsters never move; bushes can hide them).
    std::vector<vec2> danger_tiles(int gi) const;
    std::vector<std::int8_t> tile_group; // spawn tile -> group index, -1 elsewhere (<= 42 groups per map)
    std::vector<bc::camp> camps;          // live visible camps, refreshed by update()
    // shared memory: bytes 0..5 = slot claim (low 6 bits: target + 1, 0 none, 63 away; high 2 bits: gold bucket),
    // bytes 6/7 = squad 0/1 target + 1 (bit 7 of byte 6: help flag)
    // Shared memory snapshot: one bulk read per turn (each byte read is a 40-fuel host call); writes go through.
    mutable std::array<std::uint8_t, 8> sm{};
    void read_sm() { bc::shared_memory.read(0, std::span<std::uint8_t>(sm)); }
    void write_sm(int i, std::uint8_t v) const { if (sm[i] != v) sm[i] = v, bc::shared_memory[i] = v; }
    std::vector<bc::champion> ally_h; // ally handles in slot (draft) order
    std::vector<bc::champion> enemy_h;
    std::array<bool, 8> alive{};      // per slot, refreshed each turn
    // enemies: also track enemy deaths this turn (k-hold H1 asks only near fights: ~300 fuel a turn otherwise)
    void refresh_alive(bool enemies);
    // k-hold H1: last round each ally / enemy slot was seen to die (is_alive works map-wide), for the recent trade
    std::array<bool, 8> enemy_alive{};
    int enemy_tracked = -100; // last round enemy deaths were tracked (after a gap the first look records none)
    std::array<int, 8> ally_died{-999, -999, -999, -999, -999, -999, -999, -999}, enemy_died{-999, -999, -999, -999, -999, -999, -999, -999};
    int trade(int rounds) const;      // enemy deaths - ally deaths in the last `rounds` rounds
    bool slot_alive(int i) const;
    bool is_leader() const;           // lowest active (claiming) alive slot of my squad
    int alive_count() const;
    // Squads (small farming parties): 2 when we field 5+ bodies, else 1.
    int n_squads = 1;
    bool herd_map = false; // i-opening: >= 80% of the gems in monoliths / 4+ elk herds (stampede, draft MapKind::boss)
    bool boss_map = false; // every gem group is a monolith (contest): rally/escort bags, siege + farm our half
    bool solo_map; // x-expanse EXP_SOLO: solo loops (World::init). No initialiser: W is static (zero-initialised), and one
                   // would change World's constructor, so the all-off build would no longer be rr-r1's binary
    std::array<int, 8> squad_of{};
    // ONE_SQUAD: the split taken at SPLIT_R, the round it happened in this instance (0 = not yet) and the target a squad-1
    // member kept at the split (debug). No initialisers: W is static (zeroed), so the switch-off build runs no extra code.
    int planned_squads;
    std::array<int, 8> planned_of;
    int split_round, split_keep;
    int seen_t0p; // SPLIT_CAP: team target + 1 seen at my last update (0: none yet)
    int squad_alive(int s) const;
    int squad_active(int s) const;    // alive members of squad s not away on a trip (claim set)
    int claims(int gi) const;         // other alive allies claiming group gi
    int claim_of(int slot) const;     // group claimed by slot, -1 none
    int slot_of(const bc::champion& c) const; // draft slot of an ally handle, -1 unknown
    int spawn_altar = FAR;            // BFS steps from our spawn to the nearest altar tile
    void set_claim(int gi) const;
    void set_away() const;            // on a trip (tribute, recall, retreat) or dead: claim byte 255
    bool slot_away(int slot) const;
    int my_bucket = 0;                // own gold bucket (0 <150, 1 more), bit 6 of my claim byte
    int gold_bucket(int slot) const;
    // bit 7 of my claim byte: I am at my target (within 6 of it). Waiting allies on opposite sides of a camp
    // are often out of each other's vision; without this a full squad stood at an elk pair for 145 rounds.
    bool my_arrived = false;
    int arrived_claims(int gi) const; // other alive allies at group gi
    int squad_target(int s) const;
    std::array<float, 8> siege_dps{}; // per slot: design damage/round from range-4 abilities (safe vs camps)
    // per slot: measured camp damage per champion-round at base stats (platform replays) and design health;
    // team mean damage and the two highest healths (i-opening need gate)
    std::array<float, 8> camp_dps{}, base_hp{};
    float team_dps = 60;
    std::array<float, 2> top_hp{};
    float siege_of(int squad) const;  // alive members of squad (-1: whole team), range-4 damage per round
    bool squad_help() const;          // squad 0 asked squad 1 to join its target
    void set_squad_target(int gi, bool help) const;
};

extern World W;
