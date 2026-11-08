// x-expanse EXP_SOLO: job choice on solo maps (W.solo_map). On expanse independent loops work best (platform
// replays: one champion re-takes the two near wolves every ~30 rounds, one does the wolf pair and the elk, a ranged
// caster sieges an elk pair alone); rr-r1's one squad walked to the elk at 20, anchored on a far elk pair and filled the
// gaps with gold groups, so the 2nd near wolf was first taken at r316. Here each champion takes the job where it adds the
// most per round: k = 1 + other claimants (shared memory), the gain over what the claimants make without it.
// No iostream in this TU (no new static initialiser on the first turn: u-squad c0).
#include "bot.h"
#include "world.h"
#include "zreach.h"
#include <algorithm>
#include <array>

namespace {
constexpr float SOLO_CAP_T = 7.f; // rounds a capture adds to a job (plan.cpp CAP_T)
constexpr float SOLO_SIEGE_W = EXP_SIEGE_W;
constexpr int SOLO_WAIT = EXP_WAIT; // plan.cpp MAX_WAIT 3
}

bool g_heal_want = false;

void Bot::solo_target() {
    auto& me = bc::self();
    const vec2 p = me.position();
    int capturing = W.group_of_spawn(p);
    if (capturing >= 0 && !W.groups[capturing].reward) capturing = -1;
    update_needs();
    g_heal_want = false;
    // EXP_DUO: partner slot (Cassi <-> Issac), active while alive and not away
    const auto my_t = me.champion_name();
    int duo = -1;
    if (EXP_DUO && (my_t == bc::champion_type::cassi || my_t == bc::champion_type::issac))
        for (int i = 0; i < W.n_allies && i < 8; ++i) {
            const auto t = W.ally_h[i].champion_name();
            if (i != W.me_idx && t != my_t && (t == bc::champion_type::cassi || t == bc::champion_type::issac)) duo = i;
        }
    const bool duo_on = duo >= 0 && W.alive[duo] && !W.slot_away(duo);
    if (duo_on && my_t == bc::champion_type::cassi) {
        const int t = W.claim_of(duo);
        if (t >= 0 && t < static_cast<int>(W.groups.size()) && W.groups[t].skip_until <= W.round && t != capturing) {
            if (t != target) wait_turns = 0;
            target = t;
            W.set_claim(t);
            return;
        }
    }
    const float dps1 = (W.camp_dps[W.me_idx] + (duo_on ? W.camp_dps[duo] : 0.f)) * dps_scale;
    const float siege = W.siege_dps[W.me_idx] * dps_scale;
    const float hp_now = me.health(), hp_max = me.max_health();
    // damage a party with dps `dps` takes killing what is left of group gi strongest first (the tank's share)
    auto taken_of = [&](int gi, float dps) -> float {
        const auto& g = W.groups[gi];
        if (g.reward || g.harmless) return 0.f;
        float rs = 0, t = 0;
        for (size_t m = 0; m < g.hp_member.size(); ++m) if (g.hp_member[m] > 0) rs += g.member_str[m];
        std::vector<std::pair<float, float>> hs;
        for (size_t m = 0; m < g.hp_member.size(); ++m) hs.push_back({g.hp_member[m], g.member_str[m]});
        std::sort(hs.begin(), hs.end(), [](auto x, auto y) { return x.second > y.second; });
        for (auto [hp, st] : hs) if (hp > 0) { t += rs * hp / dps; rs -= st; }
        return t;
    };
    // rounds until k champions (same damage as mine) have cashed group gi in; < 0 unavailable
    auto time_of = [&](int gi, int k, float dps) -> float {
        const auto& g = W.groups[gi];
        const int d = W.dist_to_group(gi, p);
        if (d >= FAR) return -1;
        const bool est = g.reward && g.est_respawn > 0 && g.last_seen < W.round;
        const int ra = est ? g.est_respawn : g.respawn_at;
        const int avail = ra > W.round ? ra - W.round : 0;
        if (avail > d + SOLO_WAIT) return -1;
        float hp_left = (g.last_seen >= 0 && g.hp_seen >= 0) ? g.hp_seen : g.max_hp;
        if (g.reward) hp_left = est ? g.max_hp : 0;
        return static_cast<float>(std::max(d, avail)) + hp_left / (dps * static_cast<float>(k)) + SOLO_CAP_T;
    };
    // PAIR_D: other alive allies (not away) within PAIR_D of a group centre; positions read once, on first use
    std::array<vec2, 8> mate{};
    std::array<int, 8> mate_job{};
    int n_mate = -1;
    auto mates_near = [&](vec2 q, [[maybe_unused]] int gi) { // c7m PAIR_FREE: gi = the group (off: v31's lambda)
        if (n_mate < 0) {
            n_mate = 0;
            for (int i = 0; i < W.n_allies && i < 8; ++i)
                if (i != W.me_idx && W.alive[i] && !W.slot_away(i))
                    try { mate[n_mate] = W.ally_h[i].position(); if constexpr (PAIR_FREE > 0) mate_job[n_mate] = W.claim_of(i); ++n_mate; } catch (...) {} // ponytail: an unreadable ally counts as far
        }
        if constexpr (PAIR_FREE == 0) return static_cast<int>(std::count_if(mate.begin(), mate.begin() + n_mate, [&](vec2 m) { return cheb(m, q) <= PAIR_D; }));
        else {
            int n = 0;
            for (int m = 0; m < n_mate; ++m) {
                const int j = mate_job[m];
                const bool busy = j >= 0 && j != gi && j < static_cast<int>(W.groups.size()) && (PAIR_FREE == 1 || (PAIR_FREE == 2 && W.groups[j].need >= 2));
                n += cheb(mate[m], q) <= PAIR_D && !busy;
            }
            return n;
        }
    };
    float best = 0;
    int bt = -1;
    for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
        const auto& g = W.groups[gi];
        if (g.skip_until > W.round || gi == capturing) continue;
        const float value = g.gems + (g.gems > 0 ? EXP_GOLD_W : EXP_GOLD0_W) * g.gold;
        if (value <= 0) continue;
        // own half only: our own groups on expanse are safe to farm (top teams: 0.1 deaths a game); a walk to the enemy's wolves
        // (Varg at r2-90 to (62,45) in the first build) meets their loops
        if (g.enemy_side) continue;
        const int c = W.claims(gi) - (duo_on && W.claim_of(duo) == gi ? 1 : 0);
        const bool duo_ok = duo_on && taken_of(gi, dps1) <= EXP_DUO_HP * hp_now;
        // Y_LIVE: a started group (no regeneration) is judged by what is left of it, so allies join a siege once the
        // rest fits them (sall debug, ambush: Stellar sieged the 120-gem elk pair alone for 82 rounds while 5 allies
        // walked to gold; the need stays the design need)
        bool can_live = false, can_c = false; // constant false with Y_LIVE off (the build is rr-c1's)
        if constexpr (Y_LIVE) if (g.need > 1 + c && !g.reward && !g.harmless && g.hp_seen >= 0 && g.hp_seen < g.max_hp) {
            std::vector<std::pair<float, float>> live;
            for (size_t m = 0; m < g.hp_member.size(); ++m) live.push_back({g.hp_member[m], g.member_str[m]});
            std::sort(live.begin(), live.end(), [](auto x, auto y) { return x.second > y.second; });
            can_live = tankable(live, 1 + c);
            can_c = c > 0 && tankable(live, c); // the claimants finish what is left without me
        }
        float sc = -1;
        if (g.need <= 1 + c || duo_ok || can_live) {
            const float t = time_of(gi, 1 + c, dps1);
            if (t <= 0) continue;
            if (EXP_HPCHK && !g.harmless && hp_now - taken_of(gi, dps1 * static_cast<float>(1 + c)) / static_cast<float>(duo_on ? 1 : 1 + c)
                < EXP_FLOOR * hp_max) { g_heal_want = true; continue; }
            sc = value / t;
            // a joiner adds the gain over the claimants alone; the holder keeps the whole value (else a holder and a
            // joiner of a pair swapped jobs every turn: Issac and Stellar on g2 / g22, r323-420)
            if (gi != target && c > 0 && (g.need <= c || can_c)) { const float t0 = time_of(gi, c, dps1); if (t0 > 0) sc -= value / t0; }
        } else if (PAIR_N > 0 && 1 + c < g.need && g.need <= PAIR_N && !g.monolith && (PAIR_D <= 0 || gi == target || mates_near(g.center, gi) >= g.need - 1 - c)) {
            const float t = time_of(gi, g.need, dps1);
            if (t <= 0) continue;
            sc = value / (t * static_cast<float>(g.need));
        } else if (EXP_SIEGE && (SIEGE_REACH ? siege_vs(W.me_idx, gi) : siege) > 0 && (c == 0 || gi == target) && !g.monolith) {
            const float sv = SIEGE_REACH ? siege_vs(W.me_idx, gi) * dps_scale : siege; // p-fix F3: per target
            const float t = time_of(gi, 1, dps1);
            if (t <= 0) continue;
            // range-4 casts on the members that hit back; basic attacks take the soft ones (gem clusters) meanwhile
            float hp = g.hp_seen >= 0 ? g.hp_seen : g.max_hp, hard = 0;
            if (g.reward) hp = 0;
            for (size_t m = 0; m < g.hp_member.size(); ++m) if (g.member_str[m] > 0) hard += g.reward ? 0.f : g.hp_member[m];
            sc = SOLO_SIEGE_W * value / (t - hp / dps1 + std::max(hard / sv, (hp - hard) / dps1));
        }
        if (sc <= 0) continue;
        if (gi == target) sc *= (g.awake && g.hp_seen > 0 && g.hp_seen < g.max_hp) ? 4.f : EXP_KEEP;
        if (sc > best) { best = sc; bt = gi; }
    }
    if (bt >= 0) g_heal_want = false;
    if (bt != target) wait_turns = 0;
    target = bt;
    W.set_claim(bt);
}

// EXP_HOLD: a lone champion fights what is left of a group it could not tank only if the rest fits in its current hp
bool Bot::hold_ok(const std::vector<bc::camp>& members, int k) const {
    if (k > 1) return true;
    std::vector<std::pair<float, float>> m;
    for (const auto& c : members) try { m.push_back({c.health(), c.strength()}); } catch (...) {}
    std::sort(m.begin(), m.end(), [](auto x, auto y) { return x.second > y.second; });
    float rs = 0;
    for (auto [hp, st] : m) if (hp > 0 && st > 0) rs += st;
    if (rs <= 0) return true;
    const float dps = EXP_HOLD_DPS * W.camp_dps[W.me_idx] * dps_scale;
    float taken = 0;
    for (auto [hp, st] : m) if (hp > 0) { taken += rs * hp / dps; rs -= st; }
    return taken <= EXP_HOLD_HP * bc::self().health();
}
