// Combat: retreat, walking to altars and fighting a camp group (champion fights: pvp.cpp).
#include "bot.h"
#include "actions.h"
#include "zreach.h"
#include "world.h"
#include "prof.h"
#include "pvp.h"
#include "vision.h"
#include "heart.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <iostream>

void Bot::retreat(const std::vector<bc::champion>& enemies, const std::vector<bc::camp>& camps) {
    auto& me = bc::self();
    if (GATE && gate_exit()) return; // c7m: inside a camp wall, recall (the walk out is one tile wide)
    std::vector<vec2> threats;
    for (const auto& e : enemies) threats.push_back(e.position());
    for (const auto& c : camps) {
        try { if (c.is_awake() && c.strength() > 0) threats.push_back(c.position()); } catch (...) {}
    }
    const vec2 p = me.position();
    const int md = min_dist(p, threats);
    bool enemy_close = false;
    for (const auto& e : enemies) if (cheb(p, e.position()) <= 7) enemy_close = true;
    if ((ESC_RANGE ? slack(p, threats) + 3 : md) > 3 && !enemy_close) { // p-fix F4: out of each monster's own range
        try { me.recall(); return; } catch (...) {}
    }
    if constexpr (HOR_PLAY) // h-heart: S2 as an escape (dash 2 + walk 1 toward home) before the shield takes the cast
        if (hor::me() && enemy_close) { hor::escaping = true; const bool d = dash_field(W.home_field()); hor::escaping = false; if (d) return; }
    cast_support(true);
    std::vector<vec2> mons; // awake monsters within 6
    bool boss = false;
    for (const auto& c : camps)
        try { if (c.is_awake() && c.strength() > 0 && cheb(p, c.position()) <= 6) { mons.push_back(c.position()); boss |= c.strength() >= 200; } } catch (...) {}
    // f-contest K8: monoliths hit for 200: get out by BFS around walls and units (greedy steps froze in wall pockets);
    // with an enemy near, R5 / the scored step below weigh both
    if (boss && !enemy_close && escape_danger(mons)) return;
    const auto& hf = W.home_field();
    // f-small R5: inside awake monsters' range, walk the shortest path out of all of it. Waiting for a strictly
    // better neighbour froze champions in swirl's corridor between the two central elks until the elks killed them.
    if ((ESC_RANGE ? slack(p, mons) + 3 : min_dist(p, mons)) <= 3) { // p-fix F4: inside some monster's own range
        bc::bitboard roots, scratch;
        roots.clear();
        roots.draw_chebyshev(p, 8.f);
        for (vec2 m : mons) {
            bc::bitboard d;
            d.clear();
            d.draw_chebyshev(m, ESC_RANGE ? static_cast<float>(reach_at(m)) : 3.f); // F4: zone = its range
            roots &= ~d;
        }
        roots &= ~W.static_walls;
        if (!roots.is_empty()) {
            bc::distance_field ef;
            ef.compute(W.static_walls, roots, scratch, 16);
            if (ef.get(p) < FAR && best_step([&](vec2 q) -> float {
                    const float e = ef.get(q);
                    if (e >= FAR) return std::numeric_limits<float>::infinity();
                    float h = hf.get(q);
                    if (h >= FAR) h = 200;
                    return 10.f * e - 4.f * std::min(min_dist(q, threats), 5) + 0.1f * h;
                })) return;
        }
    }
    best_step([&](vec2 q) -> float {
        float h = hf.get(q);
        if (h >= FAR) h = 200;
        return -4.f * std::min(min_dist(q, threats), 5) + 0.1f * h;
    });
}

bool Bot::tribute() {
    auto& me = bc::self();
    const vec2 p = me.position();
    int lh = 0; // fewest hits left on an enemy-locked altar in submit range
    vec2 la{};
    for (auto a : W.altars) {
        if (cheb(p, a) > bc::constants::submit_gems_range) continue;
        const int h = vis::enemy_lock_hits(a);
        if (h == 0) {
            if (JACKPOT && !jp_start()) { debug_block({ std::cout << "JPHOLD R" << W.round << " i" << W.me_idx << " gems=" << me.gem_count() << "\n"; }); continue; } // c19j
            try { me.submit_gems(a); return true; } catch (...) {}
        }
        else if (lh == 0 || h < lh) { lh = h; la = a; }
    }
    bool leaving = false; // 4b: walking off to another altar this turn
    if (lh > 0) {
        // Enemy lock: breaking it takes one hit per turn (submit_gems hits it), then the tribute starts.
        // Walk instead when another unlocked altar is no more turns away.
        int bd = 99;
        vec2 ba{};
        for (auto a : W.altars)
            if (a != la && vis::enemy_lock_hits(a) == 0 && cheb(p, a) - 1 <= lh && cheb(p, a) < bd) { bd = cheb(p, a); ba = a; }
        if (bd < 99 && approach(ba, 1)) return true;
        // o-altar 4b: three turns of hits next to an enemy within 7 (who likely placed it): go to another altar
        int li = -1;
        for (int i = 0; i < static_cast<int>(W.altars.size()); ++i) if (W.altars[i] == la) li = i;
        // judge (o-altar, ported by the q-act review): already skipped (it re-fires every turn while still in range):
        // keep alt_field, no recompute
        leaving = LOCK_WALK && pvp::last_seen == W.round && li >= 0 && li < 32
            && ((alt_until > W.round && (alt_mask >> li & 1u)) || skip_altar(li, LOCK_R));
        if (!leaving) {
            try { me.submit_gems(la); } catch (...) {}
            return true;
        }
        debug_block({ std::cout << "LOCKWALK R" << W.round << " i" << W.me_idx << " altar " << li << " hits " << lh << "\n"; });
    }
    // hit an enemy lock in reach on the way in (free for melee only: a ranged attack uses up the walk, which left 4b's
    // ranged walkers hitting the lock in place, judge: corners Stellar r126-128)
    if (!leaving) for (auto a : W.altars)
        if (cheb(p, a) <= me.attack_range() && vis::enemy_lock_hits(a) > 0)
            try { me.attack(bc::vision::altar_lock_at(a)); break; } catch (...) {}
    // k-hold D1: an altar the standoff breaker gave up is skipped for ALT_R rounds
    const Field* gf = GATE ? gate_field() : nullptr; // c7m GATE: through the dead gate
    // c7m PUSH: walk in through their reach; GATE: always through an open gate (its one outer tile: a bag held there by the
    // mask locked Marcella in on the gate tile r400-r2000)
    if ((PUSH_R && push_until() > W.round) || gf) pvp::avoid_on = false;
    const auto& af = gf ? *gf : alt_mask && alt_until > W.round ? alt_field : W.altars_field();
    if ((dash_field(af) || step_field(af)) && TRIB_STEP && turn_enemies) tribute_now(*turn_enemies); // o-altar 4d
    return true;
}

// Monsters hit the nearest champion within 3. With a melee tank at the group, a ranged champion steps to a
// tile from which it can still shoot but where an ally is strictly closer to every dangerous member within 3;
// while a melee ally is still on its way it holds at distance 4 (abilities only).
// Returns 0: fight normally, 1: moved (no basic attack this turn), 2: holding still (cast from range 4).
int Bot::ranged_reposition(const std::vector<bc::camp>& members, const std::vector<bc::champion>& allies, vec2 npos) {
    const vec2 p = bc::self().position();
    const int r = bc::self().attack_range();
    std::vector<vec2> dz = W.danger_tiles(target), mp, mates;
    std::vector<char> melee; // melee allies act before us (tank priority) and so win monster target ties
    for (const auto& c : members) {
        try {
            mp.push_back(c.position());
            if (c.strength() > 0 && min_dist(mp.back(), dz) > 0) dz.push_back(mp.back());
        } catch (...) {}
    }
    if (dz.empty()) return 0;
    bool tank = false, coming = false;
    for (const auto& a : allies) {
        try {
            mates.push_back(a.position());
            melee.push_back(a.attack_range() <= 1);
            if (!melee.back()) continue;
            const int d = min_dist(mates.back(), dz);
            if (d <= 1) tank = true;
            // i-opening 4.5: only a melee that claims this group is coming (a passer-by on its own job left Cassi
            // poking a wolf from range 4 every 8 rounds for 70 rounds: interlock, collision)
            else if (d <= 7 && (!OP_TANK || W.claim_of(W.slot_of(a)) == target)) coming = true;
        } catch (...) {}
    }
    auto exposed = [&](vec2 q) {
        int n = 0;
        for (vec2 m : dz) {
            const int d = cheb(q, m);
            if (d > (REACH_ALL ? reach_at(m) : 3)) continue; // p-fix F5: each member's own range
            bool cover = false;
            for (size_t i = 0; i < mates.size(); ++i)
                if (cheb(mates[i], m) < d || (melee[i] && cheb(mates[i], m) == d)) { cover = true; break; }
            if (!cover) ++n;
        }
        return n;
    };
    if (tank) {
        if (exposed(p) == 0 && min_dist(p, mp) <= r) return 0; // already a good firing spot
        return best_step([&](vec2 q) -> float {
            return 100.f * exposed(q) + (min_dist(q, mp) <= r ? 0.f : 10.f) + 0.1f * cheb(q, npos);
        }) ? 1 : 0;
    }
    if (!coming || bc::self().health() > 0.9f * bc::self().max_health() && W.groups[target].awake) return 0;
    // wait for the tank just outside monster range, as close as possible for range-4 casts
    // p-fix F5: d = slack + 3, so "4" is 1 outside every member's own range
    if (best_step([&](vec2 q) -> float {
            const int d = REACH_ALL ? slack(q, dz) + 3 : min_dist(q, dz);
            return d < 4 ? 100.f - d : static_cast<float>(d - 4) + 0.1f * cheb(q, npos);
        })) return 1;
    return (REACH_ALL ? slack(p, dz) + 3 : min_dist(p, dz)) >= 4 ? 2 : 0;
}

// Whether k champions can finish these live members within their health budget (strongest killed first).
bool Bot::feasible_now(const std::vector<bc::camp>& members, int k) const {
    std::vector<std::pair<float, float>> m; // (hp, strength)
    for (const auto& c : members) {
        try { m.push_back({c.health(), c.strength()}); } catch (...) {}
    }
    std::sort(m.begin(), m.end(), [](auto x, auto y) { return x.second > y.second; });
    return tankable(m, k);
}

void Bot::fight(const std::vector<bc::camp>& members, const std::vector<bc::champion>& allies) {
    auto& me = bc::self();
    const vec2 p = me.position();
    std::vector<vec2> danger;
    for (const auto& c : members) {
        try { if (c.is_awake() && c.strength() > 0) danger.push_back(c.position()); } catch (...) {}
    }
    const bool tanking = !danger.empty() && (REACH_ALL ? slack(p, danger) + 3 : min_dist(p, danger)) <= 3; // p-fix F5
    int helpers = 0;
    for (const auto& a : allies) if (cheb(a.position(), W.groups[target].center) <= 5) ++helpers;
    if (tanking && hp_frac(me) < 0.4f && helpers >= 1) {
        best_step([&](vec2 q) -> float {
            int d = REACH_ALL ? slack(q, danger) + 3 : min_dist(q, danger);
            return d >= 4 ? static_cast<float>(d) : 100.f - d;
        });
        cast_support(true);
        return;
    }
    int nearest = 1 << 20;
    vec2 npos = p;
    for (const auto& c : members) {
        const vec2 q = c.position();
        if (cheb(p, q) < nearest) { nearest = cheb(p, q); npos = q; }
    }
    const auto& gf = W.group_field(target);
    // static field while far; near the group route around units to a free tile in range of a live member
    // (the static field reads 0 on a dead member's tile, which used to freeze melee champions there)
    auto close_in = [&] {
        if (gf.get(p) > 2) { if (!dash_field(gf)) step_field(gf); }
        else approach(npos, me.attack_range());
    };
    PROF("f.pre");
    // the reposition scan is ~5-12k (tiles x dangerous members x mates): skipped when a fight left little fuel
    const int rp = is_ranged() && bc::fuel_remaining() > 25000 ? ranged_reposition(members, allies, npos) : 0;
    PROF("f.rp");
    if (rp) {
        cast_on_camps(members, rp == 2);
    } else if (is_ranged()) {
        if (nearest <= me.attack_range()) {
            cast_on_camps(members, true);
            attack_camp(members);
            try_reset_and_attack([&] { return attack_camp(members); });
        } else if (nearest <= 4 && cast_on_camps(members, true)) {
        } else {
            cast_on_camps(members, false);
            close_in();
        }
    } else if (HOR_PLAY && hor::me() && hor::engage(members)) { // h-heart: land the dash on the group, then hit
        attack_camp(members);
    } else {
        if (nearest <= 1) {
            cast_on_camps(members, true);
            attack_camp(members);
            try_reset_and_attack([&] { return attack_camp(members); });
        } else {
            cast_on_camps(members, false);
            close_in();
            attack_camp(members);
            try_reset_and_attack([&] { return attack_camp(members); });
        }
    }
    PROF("f.act");
    // q-act F2: the kill left the move (melee attacks do): step onto the freed reward tile now, not next turn
    if (CAP_STEP && me.can_move() && camps_dead(members)) {
        auto& g = W.groups[target];
        std::uint32_t st = 0;
        if (bc::detail::sdk_camp_group_state(g.handle, &st) == 0 && st == 2) {
            g.reward = true, g.last_seen = W.round, g.respawn_at = 0, g.hp_seen = -1;
            const bool on = capture(allies, false) && g.spawns.is_set(me.position());
            debug_block({ std::cout << "CAPSTEP R" << W.round << " i" << W.me_idx << " g" << target << (on ? " on" : " off") << "\n"; });
        }
    }
    cast_support(tanking);
    PROF("f.cs");
}
