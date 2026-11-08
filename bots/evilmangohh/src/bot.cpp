// Champion turn logic: emblem builds bought on spawn or while dead,
// tank-first turn priority, claim-based camp farming with one capturer per kill, small-bag tributes (f-bank).
#include "bot.h"
#include "zreach.h"
#include "draft.h"
#include "actions.h"
#include "world.h"
#include "pvp.h"
#include "prof.h"
#include "vision.h"
#include "heart.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <iostream>
#include <string>
static const char* g_mode = "";
static bool g_ring = false; // STAMP_HOLD: hold the herd's ring this turn (no melee); read only behind the switch
// Banking (f-bank B1): the top teams bank ~44-gem bags within ~26 rounds of the first capture; our losses are carriers dying near
// altars with 44-150 gems. Smaller bags on small maps / after enemy sightings, no small-map bag older than BAG_AGE (0 = off).
static constexpr int SMALL_AREA = 2000, SMALL_CAP = 44; // maps up to ~45x45 (elkring .. collision)
static constexpr int HOT_ROUNDS = 15, HOT_CAP = 30;     // an enemy was within 7 in the last HOT_ROUNDS rounds
static constexpr int BAG_AGE = 40, BAG_AGE_MIN = 20;
static constexpr bool STICKY = true; // once a tribute trip starts, finish it
static constexpr bool CANCEL = true; // C1: cancel a tribute that enemies in reach would kill us through

namespace {
using IT = bc::item_type;
using CT = bc::champion_type;
struct Step { IT item; int n; }; // n = 0: repeat forever
std::vector<Step> build_for(CT t) { // builds read from the #1 team's match summaries
    if constexpr (BUILD_F > 0) switch (t) { // bot.h
    case CT::marcella: return {{IT::bruiser_shard_of_focus, 5}, {IT::bruiser_shard_of_focus, 0}};
    case CT::heart_of_ruin: if (BUILD_F > 1) return {{IT::bruiser_shard_of_focus, 5}, {IT::durability_shard_of_focus, 5}, {IT::power_shard_of_focus, 0}}; break;
    case CT::ren: if (BUILD_F > 1 || BUILD_R > 0) return {{IT::bruiser_shard_of_focus, 5}, {IT::durability_shard_of_health, 5}, {IT::power_shard_of_focus, 0}}; break;
    case CT::varg: if (BUILD_F > 1 || BUILD_R > 0) return {{IT::power_shard_of_strength, 5}, {IT::durability_shard_of_strength, 5}, {IT::power_shard_of_strength, 0}}; break;
    case CT::ryth: if (BUILD_F > 1) return {{IT::bruiser_shard_of_health, 5}, {IT::durability_shard_of_health, 5}, {IT::power_shard_of_health, 0}}; break;
    default: break;
    }
    if constexpr (HOR_PLAY) // h-heart: the tank's health (+100 a shard), then focus (dash 1.75 x, shield 1.5 x; +15 a shard)
        if (t == CT::heart_of_ruin) return {{IT::bruiser_shard_of_health, 5}, {IT::durability_shard_of_health, 5}, {IT::power_shard_of_focus, 0}};
    if constexpr (META_KIT) { if (t == CT::vesper_vale || t == CT::vendetta) return {{IT::bruiser_shard_of_strength, 5}, {IT::bruiser_shard_of_health, 5}, {IT::bruiser_shard_of_strength, 0}};
        if constexpr (MAR_BUILD == 1) if (t == CT::marcella) return {{IT::bruiser_shard_of_health, 5}, {IT::bruiser_shard_of_strength, 5}, {IT::bruiser_shard_of_focus, 5}, {IT::bruiser_shard_of_strength, 5}, {IT::bruiser_shard_of_health, 5}, {IT::bruiser_shard_of_focus, 0}};
        if (t == CT::marcella) return {{IT::bruiser_shard_of_health, 5}, {IT::bruiser_shard_of_focus, 5}, {IT::bruiser_shard_of_focus, 0}}; } // (notes/meta-oct26.md s2)
    switch (t) {
    case CT::ren: return {{IT::bruiser_shard_of_health, 5}, {IT::durability_shard_of_health, 5}, {IT::power_shard_of_focus, 0}};
    case CT::cassi: return {{IT::bruiser_shard_of_health, 5}, {IT::durability_shard_of_focus, 5}, {IT::power_shard_of_focus, 0}};
    case CT::ryth: return {{IT::bruiser_shard_of_health, 5}, {IT::durability_shard_of_health, 5}, {IT::power_shard_of_strength, 0}};
    case CT::stellar:
    case CT::prism:
    case CT::tavi:
        return {{IT::power_shard_of_focus, 5}, {IT::durability_shard_of_health, 5}, {IT::power_shard_of_focus, 0}};
    // Issac, Varg and the other melee/basic-attack champions (Willow: top teams buy Power Strength 63%)
    default: return {{IT::power_shard_of_strength, 5}, {IT::durability_shard_of_health, 5}, {IT::power_shard_of_strength, 0}};
    }
}

// Tanks act first so they win monster target ties (monsters hit the first champion in turn order).
int priority_for(CT t) {
    if constexpr (HOR_PLAY) if (t == CT::heart_of_ruin) return 35; // h-heart: the tank (1000 hp, shield), ahead of Ren
    if constexpr (META_KIT) if (t == CT::marcella || t == CT::vendetta) return t == CT::marcella ? 30 : 20; // META: Marcella 1200 hp
    switch (t) {
    case CT::ren: return 30;
    case CT::varg:
    case CT::ryth: return 20;
    case CT::issac: return 10;
    default: return 0;
    }
}

// x-rows ROWS_BUILD (rows map only): Monster x5, then Power of the same stat forever. The rows test (draft.h's
// fingerprint) runs only on a turn that buys (30-gold shards), so off rows non-buying turns run rr-r1's code (no fuel chaos).
bool rows_map() {
    static const bool rows = [] {
        int total = 0, mono = 0;
        for (const auto& g : W.groups) {
            if (std::find(g.member_str.begin(), g.member_str.end(), 60.f) != g.member_str.end()) return false; // elk
            total += g.gems;
            if (g.monolith) mono += g.gems;
        }
        // x-rows judge: rows' exact fingerprint (draft.h); the broad signature lost on unseen maps
        if (total != 844 || mono != 448 || bc::map::width() != 50 || bc::map::height() != 50) return false;
        int picks = W.n_allies;
        for (const auto& a : W.ally_h) try { picks -= a.champion_name() == CT::issac; } catch (...) {}
        return picks == 4;
    }();
    return rows;
}

IT rows_item(CT t, int step) {
    const bool focus = t == CT::ren || t == CT::stellar || t == CT::cassi || t == CT::prism || t == CT::tavi;
    if (step < 5) return focus ? IT::monster_shard_of_focus : IT::monster_shard_of_strength;
    return focus ? IT::power_shard_of_focus : IT::power_shard_of_strength;
}

} // namespace

void Bot::shop() {
    auto& me = bc::self();
    const auto plan = emblems(build_for(me.champion_name()));
    for (int guard = 0; guard < 60; ++guard) {
        size_t i = 0;
        int s = build_step;
        while (i + 1 < plan.size() && s >= plan[i].n) { s -= plan[i].n; ++i; }
        const IT plan_item = plan[i].item;
        if (me.gold_count() < plan_item.cost()) return;
        const IT item = ROWS_BUILD && rows_map() ? rows_item(me.champion_name(), build_step) : plan_item;
        try {
            if (!me.can_buy(item, 1)) return;
            me.buy(item, 1);
            ++build_step;
        } catch (...) { return; }
    }
}

void Bot::farm(const std::vector<bc::camp>& camps, const std::vector<bc::champion>& allies) {
    auto& me = bc::self();
    const vec2 p = me.position();
    PROF("farm0");
    // re-planning scores every group several times (lanes, 42 groups: ~25k); after a costly fight turn keep the target
    if (target < 0 || (bc::fuel_remaining() > 40000 && !(SF_STARVE && sf_starved()))) choose_target(); // c18b SF_STARVE
    else W.set_claim(target);
    PROF("target");
    if (target < 0) {
        if (me.gem_count() > 0 && !(SEAL_NOPARK && g_seal == 2 && seal_closed())) { tribute(); return; }
        // x-expanse EXP_HPCHK: every job left needs more hp than I have: heal by recall rather than the walk home
        if (EXP_HPCHK && W.solo_map && g_heal_want && W.home_field().get(p) > 6) try { me.recall(); W.set_away(); g_mode = "healrecall"; return; } catch (...) {}
        step_field(W.home_field());
        return;
    }
    auto& g = W.groups[target];
    const auto& gf = W.group_field(target);
    PROF("gfield");
    if (g.reward) {
        if (!dash_field(gf)) step_field(gf);
        return;
    }
    // p-monolith: the take's group outside the take's own turn (enemy near, a bag): hold at its stage, not the ring code's monolith
    // wait (95k fuel a turn in vr1 debug: fuel-outs 3 -> 10-14 a game)
    if constexpr (MONO_TAKE) if (g.monolith && target == mono::take_group()) { mono::move_ring(target, mono::mono_tiles(target), TAKE_STAGE + 1); g_mode = "mhold"; return; }
    std::vector<bc::camp> members;
    for (const auto& c : camps)
        if (g.spawns.is_set(c.spawn_position())) members.push_back(c);
    if (members.empty()) {
        if (g.spawns.is_set(p) || (g.respawn_at > W.round && gf.get(p) <= 1)) { // never block a respawn by standing on its spawn tiles
            g_mode = "unblock";
            best_step([&](vec2 q) -> float {
                if (g.spawns.is_set(q)) return std::numeric_limits<float>::infinity();
                return q == p ? 50.f : static_cast<float>(gf.get(q));
            });
            if constexpr (UNBLOCK) if (g.spawns.is_set(me.position())) step_off(g.spawns); // c8 UNBLOCK (bot.h): from inside the block
            return;
        }
        if (!dash_field(gf)) step_field(gf);
        return;
    }
    PROF("members");
    // p-fix F1 RING: the ring moved out with the range (elk 5, monolith 6 from the member): "near" / "arrived" / "far" too
    const int nr = RING ? group_reach(target) + 3 : 6;
    int near_allies = 1;
    for (const auto& a : allies) if (cheb(a.position(), g.center) <= nr) ++near_allies;
    if (cheb(p, g.center) <= nr) { W.my_arrived = true; W.set_claim(target); }
    near_allies = std::max(near_allies, 1 + W.arrived_claims(target));
    // No regeneration: judge the group by its live hp. A group the full team cannot tank (need > team size) is never fought just
    // because the allies left alive are all here (3 survivors meleed a monolith, 1 death per siege). Monoliths: range 4 only.
    const int bar = g.need > W.n_allies ? g.need : std::min(g.need, W.alive_count());
    const bool can_fight = g.harmless || (!g.monolith && !(STAMP_HOLD && g_ring) && (near_allies >= bar || (feasible_now(members, near_allies)
        && (!EXP_HOLD || !W.solo_map || hold_ok(members, near_allies)))));
    PROF("feasible");
    if (can_fight) {
        wait_turns = 0;
        g_mode = "fight";
        fight(members, allies);
        return;
    }
    g_mode = "wait";
    // the siege is landing: keep at it (i-opening: only where sieging is allowed, monoliths and elk herds)
    if (g.hp_seen >= 0 && g.hp_seen < last_hp && (!OP_MARG || g.monolith || g.str >= 180 || (EXP_SIEGE && W.solo_map))) wait_turns = 0;
    last_hp = g.hp_seen;
    if (++wait_turns > (WAIT_ALONE > 0 && W.claims(target) == 0 && (!WAIT_ALL || g.need >= W.n_allies) ? WAIT_ALONE : 30)) { g.skip_until = W.round + 30; target = -1; wait_turns = 0; return; } // c7e WAIT_ALONE
    // Wait just outside monster range (4): range-4 abilities from there are free, permanent damage.
    std::vector<vec2> mp, dz = W.danger_tiles(target);
    if constexpr (WAIT_FUEL) { bc::bitboard in; in.clear(); for (auto t : dz) in.set(t); // c18b (bot.h): O(1) membership (46 wolves: 22-43k)
        for (const auto& c : members) { mp.push_back(c.position()); try { if (c.strength() > 0 && !in.is_set(mp.back())) in.set(mp.back()), dz.push_back(mp.back()); } catch (...) {} }
    } else for (const auto& c : members) { mp.push_back(c.position()); try { if (c.strength() > 0 && min_dist(mp.back(), dz) > 0) dz.push_back(mp.back()); } catch (...) {} }
    if (dz.empty()) dz = mp;
    PROF("w.dz");
    // Contest: the spells come back every 8 turns; in between, ranged champions on the ring shoot gold clusters of
    // any group in reach (the gold triples sit on the monolith's ring), so prefer ring tiles that reach one.
    std::vector<bc::camp> soft;
    for (const auto& c : (W.boss_map ? camps : members)) try { if (c.strength() <= 0 && c.health() > 0) soft.push_back(c); } catch (...) {}
    std::vector<vec2> sp;
    for (const auto& c : soft) sp.push_back(c.position());
    const int ar = me.attack_range();
    // i-opening: still far (members seen through a ward): follow the group's field; greedy steps toward the ring
    // stuck Ren behind a wall for 9 rounds on ambush until the 30-turn wait gave the group up
    const bool far = OP_WALK && gf.get(p) > nr && min_dist(p, dz) > nr;
    if (far) wait_turns = 0;
    // p-fix F1 RING: 1 outside each member's own range: d = min over members of cheb - range, + 3 (the old ring at 4)
    const bool moved = (far && (dash_field(gf) || step_field(gf))) || (g.monolith && escape_danger(dz)) || (WAIT_FUEL && RING // c18b
        ? wait_step(p, dz, sp, ar, W.boss_map || (EXP_SIEGE && W.solo_map)) : best_step([&](vec2 q) -> float {
        const int d = RING ? slack(q, dz) + 3 : min_dist(q, dz);
        if (d < 4) return 1000.f - d;
        if (d > 4) return static_cast<float>(d);
        return (W.boss_map || (EXP_SIEGE && W.solo_map)) && min_dist(q, sp) <= ar ? -1.f : 0.f;
    }));
    PROF("w.step");
    // RING: cast only from the ring (what reaches from there: an AoE r4 rad1 from 5 for elk / golem, nothing for monoliths);
    // a cast from inside a range wakes the group with us in it
    if (!RING || slack(me.position(), dz) >= 1) cast_on_camps(members, !moved);
    PROF("w.cast");
    // from the ring, ranged champions also shoot harmless members (gem/gold clusters) in reach
    if (!moved && (RING ? slack(me.position(), dz) + 3 : min_dist(me.position(), dz)) >= 4) attack_camp(soft);
}
// f-bank B1 bag cap; also used by f-tempo's home_after_capture so recall-on-capture predicts the same tribute.
int Bot::bag_cap() const {
    int cap = SMALL_CAP && bc::map::width() * bc::map::height() <= SMALL_AREA ? SMALL_CAP : TRIB_FULL;
    if (EXP_BAG > 0 && W.solo_map) cap = EXP_BAG;
    if (LATE_BAG > 0 && W.round > LATE_R) cap = std::min(cap, LATE_BAG); // c7e LATE_BAG
    if constexpr (SCARCE) if (gem_scarce()) return 1; // c18b SCARCE (bot.h): every gem banks
    if (HOT_CAP && W.round - pvp::last_seen <= HOT_ROUNDS) cap = std::min(cap, HOT_CAP);
    if constexpr (GEM_MAR > 0) if (bc::self().champion_name() == bc::champion_type::marcella && gem_only()) cap = std::min(cap, GEM_MAR); // c8 GEM_MAR
    if (END_CAP && (bc::global_submitted_gems(team::enemy) >= END_THEIRS // o-altar 4c: 7 of 79 platform losses ended with bank + carried >= 1000
                    || bc::global_submitted_gems(team::ally) + bc::self().gem_count() >= END_OURS)) cap = std::min(cap, END_BAG);
    return cap;
}
// o-altar 4a: tribute at an unlocked altar in reach unless the enemies within 4 out-damage our hp first (C1)
bool Bot::tribute_now(const std::vector<bc::champion>& enemies) {
    auto& me = bc::self();
    const vec2 p = me.position();
    const int gems = static_cast<int>(me.gem_count());
    float dps = 0;
    for (const auto& e : enemies) {
        if (TRIB_GUARD) { try { if (e.is_visible() && e.is_alive() && cheb(p, e.position()) <= 4) dps += champ_dps(e); } catch (...) {} }
        else if (cheb(p, e.position()) <= 4) dps += champ_dps(e);
    }
    const bool jp = JACKPOT && gems >= JACKPOT; // c19j JACKPOT: jp_start decides, asked in altar range only
    if (!jp && dps * static_cast<float>((gems + 9) / 10) >= me.health()) return false;
    for (auto a : W.altars)
        if (cheb(p, a) <= bc::constants::submit_gems_range && vis::enemy_lock_hits(a) == 0 && (!jp || jp_start()))
            try {
                me.submit_gems(a);
                debug_block({ int nd = 99; for (const auto& e : enemies) if (e.is_visible()) nd = std::min(nd, cheb(p, e.position()));
                    std::cout << "TRIBNOW R" << W.round << " i" << W.me_idx << " gems=" << gems << " near=" << nd << "\n"; });
                return true;
            } catch (...) {}
    return false;
}
// R1_ACT: the rest of a first turn that W.init left under 90k: one step toward the target, nothing else (pvp costs 30-60k).
// choose_target scores on its distance estimate (update_needs is cached for round 2); the step follows a cut BFS (5-10k).
void Bot::first_step() {
    auto& me = bc::self();
    if (bc::fuel_remaining() < R1_STEP || !me.is_alive() || !me.can_move()) return;
    const vec2 p = me.position();
    for (const auto& c : W.enemy_h) {
        bc::detail::champion_info ci{};
        if (bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)), &ci) == 0 && ci.x >= 0
            && (ci.state & 1) != 0 && cheb(p, {ci.x, ci.y}) <= 8) return;
    }
    W.round = static_cast<int>(bc::round_number());
    int gi = -1;
    if (bc::fuel_remaining() >= R1_PLAN) {
        W.refresh_alive(false); // no W.update(): every group is unseen-equivalent at full hp (~11k on swirl)
        choose_target();
        gi = target;
        g_mode = "r1plan";
    } else {
        const vec2 a = W.anchor[W.squad_of[W.me_idx] & 1];
        for (int i = 0; i < static_cast<int>(W.groups.size()) && gi < 0; ++i)
            if (W.groups[i].gems > 0 && W.groups[i].center == a) gi = i;
        g_mode = "r1anchor";
    }
    PROF("r1.target");
    debug_block({ std::cout << "R1STEP i" << W.me_idx << " " << g_mode << " g" << gi << " left=" << bc::fuel_remaining() << "\n"; });
    if (gi < 0 || bc::fuel_remaining() < R1_STEP || min_dist(p, W.groups[gi].tiles) <= 6) return;
    bc::distance_field f;
    bc::bitboard at;
    at.set(p);
    f.compute_to(W.static_walls, W.groups[gi].spawns, at, W.visited);
    const auto here = f.get(p);
    best_step([&](vec2 q) -> float {
        const auto d = f.get(q);
        return d >= here + 1 || d == bc::distance_field::unreachable ? std::numeric_limits<float>::infinity() : static_cast<float>(d);
    });
    PROF("r1.step");
}

void Bot::play_turn() {
    auto& me = bc::self();
    const auto fuel0 = bc::fuel_remaining();
    g_mode = "?";
    struct Log { Bot* b; std::uint64_t f0; ~Log() {
        PROF("body");
        // turn priority for next round, from the cooldowns this turn left (h-micro: set at turn end, not start)
        if (b->inited) try {
            auto& m = bc::self();
            int want = W.round - pvp::last_seen <= 3 ? pvp::combat_priority() : priority_for(m.champion_name());
            if constexpr (MONO_TAKE) if (mono::prio() >= 0) want = mono::prio(); // p-monolith: the healthiest takes the hit
            if (want != b->prio) { m.set_priority(want); b->prio = want; }
        } catch (...) {}
        if (b->inited) try { W.precompute(); pvp::note_end_hp(); } catch (...) {}
        PROF("pre");
        debug_block({ try { auto& m = bc::self(); std::cout << "R" << W.round << " i" << W.me_idx << " " << bc::to_string(m.champion_name())
            << " " << g_mode << (g_ring ? "+ring" : "") << " t=" << b->target << " need=" << (b->target>=0? W.groups[b->target].need:0) << " hp=" << (int)m.health() << "/" << (int)m.max_health()
            << " gems=" << m.gem_count() << " gold=" << m.gold_count() << " sa=" << W.spawn_altar << " ad=" << (W.altar_ready ? W.altar_field.get(m.position()) : -1) << " pos=" << bc::to_string(m.position()) << " fuel=" << (f0 - bc::fuel_remaining()) << " act=" << g_acts;
            for (const auto& hg : W.groups) if (!hg.monolith && hg.str >= 240 && W.herd_map && !W.boss_map) std::cout << " hh=" << (int)hg.hp_seen << " hrw=" << hg.reward << " hra=" << hg.respawn_at << " hls=" << hg.last_seen;
            for (const auto& hg : W.groups) if (hg.monolith && W.boss_map) std::cout << " mh=" << (int)hg.hp_seen << " mrw=" << hg.reward << " mls=" << hg.last_seen; // y-contest
            std::cout << "\n"; g_acts.clear(); } catch (...) {} });
        debug_block({ if (W.split_round > 0 && W.split_keep >= 0) { std::cout << "SPLITKEEP R" << W.round << " i" << W.me_idx << " g" << W.split_keep << "\n"; W.split_keep = -1; } });
        prof_end(W.me_idx);
    } } log{this, fuel0};
    PROF("start");
    W.read_sm();
    if (!inited) {
        W.init();
        PROF("init");
        inited = true;
        try { me.set_priority(prio = priority_for(me.champion_name())); } catch (...) {}
        // W.init takes 70-140k of the 200k first turn (big maps); the first decisions need up to ~80k more. Running
        // out would suspend into round 2 anyway, so give round 1 up and start fresh (R1_ACT: after one cheap step).
        if (bc::fuel_remaining() < 90000) { if (R1_ACT) first_step(); return; }
    }
    pvp::avoid_on = false;
    if (!me.is_alive()) { g_mode = "dead"; W.round = static_cast<int>(bc::round_number()); W.set_away(); shop(); return; }
    PROF("prio");
    // ward moves first: camps W.update() caches from the old ward's tiles would throw 97 later this turn
    if (!me.is_recalling() && !me.is_submitting_gems() && !me.is_teleporting()) {
        W.round = static_cast<int>(bc::round_number());
        pvp::reveal();
        vis::scout_altar();
        vis::ward(me.gem_count() >= TRIB_NEAR);
    }
    W.update();
    PROF("update");
    W.refresh_alive(pvp::HOLD && W.round - pvp::last_seen <= pvp::TRADE_R);
    {
        const auto gold = me.gold_count();
        W.my_bucket = gold < 150 ? 0 : 1;
        W.my_arrived = false;
    }
    const vec2 p = me.position();
    const bool at_spawn = bc::map::spawn_region(team::ally).is_set(p);
    const auto bag0 = me.gem_count(); if constexpr (JACKPOT > 0) g_bag = static_cast<int>(bag0); // c19j: no extra engine query
    if (bag0 == 0) bag_since = -1, trip = false; else if (bag_since < 0) bag_since = W.round;
    if (at_spawn || me.passive() == bc::passive_type::remote_shop) shop();
    PROF("shop");
    if (CANCEL && me.is_submitting_gems() && me.gem_count() >= 20) {
        // C1: enemies in reach that out-damage our hp before the tribute completes take the whole bag on the kill:
        // cancel and play the turn (fight or run). Damage alone never interrupts a tribute.
        float dps = 0;
        for (const auto& e : bc::vision::champions(team::enemy))
            try { if (e.is_visible() && e.is_alive() && cheb(p, e.position()) <= 4) dps += champ_dps(e); } catch (...) {}
        try {
            const int left = static_cast<int>(me.remaining_channel_time()) + 1;
            const int win = JACKPOT && g_bag >= JACKPOT ? std::min(left, JP_WIN) : left; // c19j JACKPOT
            if (left >= 2 && dps * win >= me.health()) { me.cancel_submit_gems(); debug_block({ std::cout << "CANCEL R" << W.round << " dps=" << dps << " left=" << left << " gems=" << me.gem_count() << "\n"; }); }
        } catch (...) {}
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) { g_mode = "chan"; return; }
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;

    // Visible living champions from one raw engine record per handle (vision::champions plus the per-champion
    // is_visible/is_alive/position queries cost ~6k a turn).
    std::vector<bc::champion> enemies, allies;
    const auto& enemy_spawn = bc::map::spawn_region(team::enemy);
    auto seen = [](const bc::champion& c, vec2& at) {
        bc::detail::champion_info ci{};
        if (bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)), &ci) != 0 || ci.x < 0 || (ci.state & 1) == 0) return false;
        at = {ci.x, ci.y};
        return true;
    };
    vec2 at;
    for (const auto& c : W.enemy_h)
        // enemies resting on their own spawn are unreachable and heal fully: ignore unless they can hit us
        if (seen(c, at) && !(enemy_spawn.is_set(at) && cheb(p, at) > 1)) enemies.push_back(c);
    for (int i = 0; i < W.n_allies; ++i)
        if (i != W.me_idx && seen(W.ally_h[i], at)) allies.push_back(W.ally_h[i]);
    const auto& camps = W.camps;
    turn_enemies = &enemies;
    PROF("scan");

    // enemy_near (6) makes tributes/heals unsafe; pvp::turn decides fights (enemies within 7).
    bool enemy_near = false;
    for (const auto& e : enemies) enemy_near |= cheb(p, e.position()) <= 6;
    if constexpr (SPAWN_SIEGE) if (at_spawn && !enemy_near && W.boss_map && spawn_siege()) { g_mode = "ssiege"; return; } // c8 (bot.h)
    if (at_spawn && hp_frac(me) < 0.95f && !enemy_near) { g_mode = "heal"; cast_support(false); return; }
    if (TRIB_FIRST && me.gem_count() >= 20 && tribute_now(enemies)) { g_mode = "tribnow"; W.set_away(); return; }
    // o-altar 2b: one empty capture phase resets the 6-round timer, so stepping off a pending capture is never free
    // (alpha-stampede: Prism at 57% hp walked off the 360-gem herd from a lone Willow, who was paid it 8 rounds later)
    const bool pend = PEND_HOLD && cap_group >= 0 && cap_round == W.round - 1 && W.groups[cap_group].reward
        && W.groups[cap_group].last_seen == W.round && W.groups[cap_group].spawns.is_set(p);
    pvp::pend_gi = pend ? cap_group : -1;
    debug_block({ if (pend && !enemies.empty()) std::cout << "PEND R" << W.round << " i" << W.me_idx << " g" << cap_group << " hp=" << hp_frac(me) << "\n"; });
    const bool fought = pvp::turn(enemies);
    PROF("pvp");
    if (fought) {
        if (pend && W.groups[cap_group].spawns.is_set(me.position())) cap_round = W.round;
        g_mode = "pvp";
        return;
    }
    if constexpr (MONO_TAKE || MONO_DENY) if (const char* m = mono_turn(enemies)) { g_mode = m; return; } // p-monolith
    if constexpr (UNSEEN) if (!at_spawn && unseen_escape(p, me.health(), enemies.empty(), target)) { g_mode = "unseen"; return; }
    // k-hold H1: next to a big reward nobody can hit us at (pvp declined), capture it first (nt4-stampede: v5m recalled off
    // the 360-gem herd after winning the skirmish there)
    bool big_reward = false;
    if (pvp::HOLD_ON_RETREAT)
        for (const auto& g : W.groups)
            big_reward |= g.reward && g.last_seen == W.round && (g.gems >= pvp::HOLD_GEMS || g.monolith) && min_dist(p, g.tiles) <= 2;
    // x-stamp STAMP_HOLD: on the live herd with no enemy in sight, a low-hp champion holds the ring instead of going home
    if constexpr (STAMP_HOLD) {
        g_ring = false;
        const auto& g = W.groups[std::max(target, 0)];
        if (target >= 0 && target == stamp_herd() && enemies.empty() && !at_spawn)
            g_ring = g.last_seen == W.round && !g.reward && g.respawn_at == 0 && g.hp_seen > 0 && hp_frac(me) < 0.6f;
    }
    bool fin = false; // c8 STAMP_FIN (bot.h): the herd is low, finish it before a thief does
    if constexpr (STAMP_FIN > 0) { const auto& g = W.groups[std::max(target, 0)]; fin = target >= 0 && target == stamp_herd() && !at_spawn && g.last_seen >= W.round - 3 && !g.reward && g.respawn_at == 0 && g.hp_seen > 0 && g.hp_seen <= 0.01f * STAMP_FIN * g.max_hp; if (fin) g_ring = false; }
    // after a skirmish (enemies seen in the last 10 rounds) go heal below 60% instead of farming on nearby
    if (!(STAMP_HOLD && g_ring) && !at_spawn && (hp_frac(me) < (STAMP_FIN > 0 && fin ? 0.2f : 0.33f) || (ESC_EXIT && exit_due(p, me.health())) || (!(STAMP_FIN > 0 && fin) && hp_frac(me) < 0.6f && W.round - pvp::last_seen <= 10 && !big_reward && !pend))) {
        g_mode = "retreat"; W.set_away(); retreat(enemies, camps); return;
    }
    const int gems = static_cast<int>(me.gem_count());
    const int banked = static_cast<int>(bc::global_submitted_gems(team::ally));
    bool safe = !enemy_near;
    for (const auto& c : camps) try { if (c.is_awake() && cheb(p, c.position()) <= (REACH_ALL ? reach(c.kind()) : 3)) safe = false; } catch (...) {} // p-fix F5
    const bool captured = capture(allies, safe);
    PROF("capture");
    if (captured) { g_mode = me.is_recalling() ? "caprecall" : "capture"; return; }
    if (const char* m = boss_bag(gems, at_spawn, safe, allies)) { g_mode = m; return; }
    if (const char* m = boss_escort(at_spawn, safe)) { g_mode = m; return; }
    // z-boss SEAL_NOPARK: no trip while the seal stands; once it is open every bag goes (last term)
    if (gems > 0 && !(SEAL_NOPARK && g_seal == 2 && seal_closed())) {
        const int ad = W.altars_field().get(p);
        const bool small = bc::map::width() * bc::map::height() <= SMALL_AREA;
        const int cap = bag_cap();
        // big maps: the age rule cost tempo in quiet farming races (expanse 7/12 -> 3-4/12, corners 11 -> 7-8)
        const bool old_bag = BAG_AGE && small && gems >= BAG_AGE_MIN && W.round - bag_since >= BAG_AGE;
        if ((STICKY && trip) || gems >= cap || old_bag || ad <= 3 || banked + gems >= static_cast<int>(bc::constants::winning_gems)
            || W.round > 1850 || (gems >= TRIB_NEAR && ad <= TRIB_RAD) || (SEAL_NOPARK && g_seal == 2)) {
            // recall to shop and heal on the way, when spawn->altar is shorter than the
            // walk home we would make after the tribute anyway
            const bool wants_home = me.gold_count() >= 150 || hp_frac(me) < 0.6f;
            if (!at_spawn && safe && wants_home && ad > 3 && W.spawn_altar < W.home_field().get(p)) {
                try { g_mode = "shoprecall"; W.set_away(); me.recall(); return; } catch (...) {}
            }
            g_mode = "tribute";
            trip = true; // keep going once decided (the hot cap lapses on the way)
            W.set_away();
            if (!(GATE && gate_turn())) tribute(); // c7m GATE: the gate camp first
            unstick(p, true);
            return;
        }
    }
    if (!(STAMP_HOLD && g_ring) && !(STAMP_FIN > 0 && fin) && (gems == 0 || (SEAL_NOPARK && g_seal == 2 && seal_closed())) && !at_spawn && safe && W.home_field().get(p) > 12
        && ((me.gold_count() >= 300 && me.passive() != bc::passive_type::remote_shop) || hp_frac(me) < 0.5f)) {
        try { g_mode = "shoprecall"; W.set_away(); me.recall(); return; } catch (...) {}
    }
    g_mode = "farm";
    if (!(GATE && gate_exit())) farm(camps, allies); // c7m GATE: out of the wall first
    unstick(p, false);
}

// k-hold D1: a declined fight's avoid mask can hold both teams still for good (corners: five carriers each 2-3 from the centre
// altar r200-r2000). A frozen board wins at r2000 for the team ahead: we stay if ahead and they have not banked meanwhile.
void Bot::unstick(vec2 p0, bool trib) {
    auto& me = bc::self();
    if (!UNSTICK || !pvp::avoid_on || me.position() != p0 || !me.can_attack() || !me.can_cast()) { stuck = 0; return; }
    if (stuck++ == 0) stuck_eb = static_cast<int>(bc::global_submitted_gems(team::enemy));
    if (stuck < STUCK_N) return;
    stuck = 0;
    const int ours = static_cast<int>(bc::global_submitted_gems(team::ally)), theirs = static_cast<int>(bc::global_submitted_gems(team::enemy));
    if (ours > theirs && theirs == stuck_eb) return;
    debug_block({ std::cout << "UNSTICK R" << W.round << " i" << W.me_idx << (trib ? " altar" : " target ") << (trib ? -1 : target) << " " << ours << "-" << theirs << "\n"; });
    if (!trib) {
        if (target >= 0) W.groups[target].skip_until = W.round + SKIP_R, target = -1;
        return;
    }
    if (alt_until <= W.round) alt_mask = 0;
    int bi = -1; // the altar we are held off: nearest not yet skipped
    for (int i = 0; i < static_cast<int>(W.altars.size()) && i < 32; ++i)
        if (!(alt_mask >> i & 1u) && (bi < 0 || cheb(p0, W.altars[i]) < cheb(p0, W.altars[bi]))) bi = i;
    if (!(bi >= 0 && skip_altar(bi, ALT_R)) && PUSH_R) push_until() = W.round + PUSH_R; // c7m PUSH: no other altar
}

// Skip altar bi for `rounds` rounds: tribute() walks alt_field, a distance field over the altars not skipped.
bool Bot::skip_altar(int bi, int rounds) {
    if (bi < 0 || bi >= 32) return false;
    if (alt_until <= W.round) alt_mask = 0;
    alt_mask |= 1u << bi;
    bc::bitboard roots;
    roots.clear();
    for (int i = 0; i < static_cast<int>(W.altars.size()) && i < 32; ++i)
        if (!(alt_mask >> i & 1u)) roots.draw_chebyshev(W.altars[i], static_cast<float>(bc::constants::submit_gems_range));
    roots &= ~W.static_walls;
    if (roots.is_empty()) { alt_mask = 0; return false; } // every altar held: fall back to the nearest
    alt_field.compute(W.static_walls, roots, W.visited);
    alt_until = W.round + rounds;
    return true;
}
