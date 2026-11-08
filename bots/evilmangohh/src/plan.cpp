// Camp planning: every champion claims the group where it adds the most
// gems per round (shared-memory claims fill each group up to the champions it needs), re-farming groups
// as they respawn; exactly one champion captures a kill while the others move straight on.
#include "bot.h"
#include "actions.h"
#include "zreach.h"
#include "world.h"
#include "pvp.h"
#include "prof.h"
#include <algorithm>
#include <array>
#include <cstdio>

namespace {
constexpr int SIDE_RANGE = 10; // side jobs only this close (a far solo trip strands the champion)
constexpr float RET_W = 0.7f;  // weight of the distance from a target back to the squad anchor
constexpr float SIEGE_W = 0.5f; // groups too strong to tank are sieged from range 4, discounted (exposed, slow)
constexpr float GOLD_W_EARLY = 0.3f, GOLD_W_LATE = 0.15f; // gems per gold in target values
constexpr float CAP_T = 7.f;  // rounds a capture adds to a job (was 13: far big groups looked 2x better than near wolves)
constexpr int SHOP_HOME = 20; // OP_SHOP: small purchases / heals only this close to spawn
constexpr float PRE_X = 1.5f; // OP_PRE: switch margin
constexpr int FAR_D = 45, FAR_R = 250; // OP_FAR (openings 4.1: nothing beyond 45 while own groups exist)
constexpr int HERD_R = EC_HERD ? 80 : 150; // l-econ W2 2c (EC_HERD, bot.h); OP_SIEGE: elk herds sieged from this round (top teams on pilgrimage: r195 with emblems)
constexpr int MAX_WAIT = 3;    // rounds we may wait at a respawning group after arriving (waiting longer lost to walking)
bool split_seen; // ONE_SQUAD: the split was handled (static, zero-initialised: the switch-off build runs no extra code)
}

int stamp_herd() {
    static int h = -2;
    if (h == -2) {
        h = -1;
        for (int gi = 0; W.herd_map && !W.boss_map && h < 0 && gi < static_cast<int>(W.groups.size()); ++gi)
            if (!W.groups[gi].monolith && W.groups[gi].str >= 240) h = gi;
    }
    return h;
}

// Damage taken by a party of k killing (hp, str) members strongest first, against its tank's health. A solo uses its own
// damage and health; a party the team's mean camp damage per champion and its tankiest one or two champions.
bool Bot::tankable(const std::vector<std::pair<float, float>>& hs, int k) const {
    float rs = 0;
    int n = 0;
    for (auto [hp, st] : hs) if (hp > 0 && st > 0) rs += st, ++n;
    if (n == 0) return true;
    if (!OP_NEED) {
        float taken = 0;
        for (auto [hp, st] : hs) if (hp > 0) { taken += rs * hp / (60.f * dps_scale * k); rs -= st; }
        return taken < 850.f * hp_scale * k;
    }
    const float dps = (k == 1 ? W.camp_dps[W.me_idx] : static_cast<float>(k) * W.team_dps) * dps_scale;
    float taken = 0;
    for (auto [hp, st] : hs) if (hp > 0) { taken += rs * hp / dps; rs -= st; }
    const float tank = k == 1 ? W.base_hp[W.me_idx] : W.top_hp[0] + (n >= 2 ? W.top_hp[1] : 0.f);
    return taken <= GATE_HP * tank * hp_scale;
}

// Champions needed per group with our current power (recomputed only when our stats change).
void Bot::update_needs() {
    auto& me = bc::self();
    // damage output counts spells (~2x focus per 8 rounds) and Ren's retaliation, so focus builds scale it too
    const auto bs = me.base_stats();
    const float sscale = std::max(1.f, (me.strength() + 0.3f * me.focus()) / std::max(1.f, bs.base_strength + 0.3f * bs.base_focus));
    const float hscale = std::max(1.f, me.max_health() / std::max(1.f, bs.base_health));
    dps_scale = sscale;
    hp_scale = hscale;
    if (sscale * hscale == need_scale) return;
    need_scale = sscale * hscale;
    for (auto& g : W.groups) {
        int need = 1;
        if (!g.harmless) {
            need = W.n_allies + 1;
            for (int k = 1; k <= W.n_allies; ++k)
                if (tankable(g.by_str, k)) { need = k; break; }
        }
        g.need = need;
    }
}

void Bot::choose_target() {
    if constexpr (MONO_TAKE) if (mono_lead()) return; // p-monolith: a take running is the target (started in mono_end)
    if constexpr (EXP_SOLO) if (W.solo_map) { solo_target(); return; }
    auto& me = bc::self();
    const vec2 p = me.position();
    const int my_sq = W.squad_of[W.me_idx];
    const int team_n = W.squad_active(my_sq);
    const int other = W.n_squads > 1 && W.squad_alive(1 - my_sq) > 0 ? W.squad_target(1 - my_sq) : -1;
    int capturing = W.group_of_spawn(p);
    if (capturing >= 0 && !W.groups[capturing].reward) capturing = -1;

    update_needs();
    PROF("ct.needs");
    // ONE_SQUAD: at the split a squad-1 member finishes the fight it is in (kept as a side job) instead of walking off it
    if (ONE_SQUAD && !split_seen && W.split_round > 0) {
        split_seen = true;
        if (W.squad_of[W.me_idx] == 1 && target >= 0) {
            const auto& g = W.groups[target];
            if (g.awake && g.hp_seen > 0 && g.hp_seen < g.max_hp && cheb(p, g.center) <= 6) {
                commit_until = W.round + 25;
                W.split_keep = target; // debug: world.cpp prints it
            }
        }
    }
    const float dps1 = 50.f * dps_scale;
    // rounds until a party of k (k>=1) has cashed group gi in; <0 if unavailable for us
    auto time_of = [&](int gi, int k) -> float {
        auto& g = W.groups[gi];
        if (g.skip_until > W.round || gi == capturing) return -1;
        // contest: the enemy half is where we died fleeing (22 of 26 pvp deaths were declined fights there)
        // y-contest MONO_R: also monoliths before MONO_R (contest.cpp boss_skip)
        if (W.boss_map && (MONO_R > 0 ? boss_skip(g) : g.enemy_side && !g.monolith)) return -1;
        const int d = W.dist_to_group(gi, p);
        if (d >= FAR) return -1;
        // verify (openings 4.1/4.8): an elk pair (herds are OP_SIEGE's) more than 45 steps from spawn waits for r250 (top teams
        // take the 120s from r284). Expanse: all four walked 55 tiles to one, the tank recalled at r70, first capture
        // r97. Per-champion needs differ (a leader with items said 3 of 4), so the gate is on the group, not on need.
        if (OP_FAR && W.round < FAR_R && !W.boss_map && !g.monolith && g.str >= 120 && g.str < 180
            && (W.spawn_ready ? W.spawn_field.get(g.tiles[0]) : d) > FAR_D) return -1;
        const bool est = OP_EST && g.reward && g.est_respawn > 0 && g.last_seen < W.round; // left while an ally captured it
        // x-stamp STAMP_BACK: an unseen herd reward was captured by whoever stood there: back at full hp after the respawn
        const bool gone = STAMP_BACK && g.reward && !est && g.last_seen < W.round && gi == stamp_herd();
        const int ra = gone ? g.last_seen + 6 + g.respawn : est ? g.est_respawn : g.respawn_at;
        const int avail = ra > W.round ? ra - W.round : 0;
        if (avail > d + MAX_WAIT) return -1;
        float hp_left = (g.last_seen >= 0 && g.hp_seen >= 0) ? g.hp_seen : g.max_hp;
        if (g.reward) hp_left = est || gone ? g.max_hp : 0;
        // a far job also costs the walk back to the squad's camps
        const float ret = RET_W * static_cast<float>(cheb(g.center, W.anchor[my_sq & 1]));
        return static_cast<float>(std::max(d, avail)) + hp_left / (dps1 * k) + (OP_CAPT ? CAP_T : 13.f) + ret;
    };
    // gems per gold: squad targets value gold (shards); side jobs are judged on gems. Contest has no other gems
    // than the bosses: gold (shards, and the lifetime-gold tiebreak) is the rest of the game.
    // i-opening 4.7: top teams take gold-only groups (9% of their r300 gold, ours 2%); 150 gold early is an emblem
    float gold_w = W.boss_map ? BOSS_GOLD_W : !OP_GOLD ? 0.08f : W.round < 300 ? GOLD_W_EARLY : GOLD_W_LATE;
    if (SEAL_GOLD && g_seal == 2) gold_w = 1.f / SEAL_GEM_W; // z-boss: values ~ gold + SEAL_GEM_W x gems (scale-free)
    if constexpr (SCARCE) if (gem_scarce()) gold_w = 0.005f; // c18b SCARCE (bot.h)
    auto value_of = [&](int gi) { return W.groups[gi].gems + gold_w * W.groups[gi].gold; };
    auto score_of = [&](int gi, int squad_n) -> float {
        if (W.groups[gi].need > squad_n) {
            // too strong to tank: siege with range-4 abilities (free, permanent damage: no regeneration).
            // On stampede the whole team can cash the 360-gem elk herd by round ~130.
            // i-opening 4.5: before r250 only monoliths on contest and elk herds on herd maps (stampede). Sieging a wolf or
            // an elk pair was 3,069 of our 4,900 idle champion-rounds against top teams (theirs: 40); collision's
            // 3-elk herd (252 with gold at 0.3) held three champions on its ring from r24 to r125.
            const auto& sg = W.groups[gi];
            // p-fix F2 MONO_NOSIEGE: monolith range 5 and no spell reaches 6: a monolith is only ever a reward job
            if constexpr (MONO_NOSIEGE) if (sg.monolith) {
                const float t = sg.reward ? time_of(gi, 1) : -1.f;
                return t <= 0 ? -1.f : SIEGE_W * value_of(gi) / t;
            }
            if constexpr (!SIEGE_HERD) if (!sg.monolith && !sg.reward) return -1; // A/B: no herd siege at all
            if (OP_SIEGE && !(sg.monolith ? W.boss_map || W.round >= 250 : sg.str >= 180 && (W.herd_map || W.round >= HERD_R))) return -1;
            const float siege = SIEGE_REACH ? siege_of_vs(squad_n >= W.alive_count() ? -1 : my_sq, gi) // p-fix F3
                                            : W.siege_of(squad_n >= W.alive_count() ? -1 : my_sq);
            const float t = time_of(gi, 1);
            if (siege <= 0 || t < 0) return -1;
            const auto& g = W.groups[gi];
            float hp_left = g.hp_seen >= 0 ? g.hp_seen : g.max_hp;
            if (STAMP_BACK && g.reward && gi == stamp_herd()) hp_left = g.last_seen < W.round ? g.max_hp : 0.f; // = time_of's
            return SIEGE_W * value_of(gi) / (t - hp_left / dps1 + hp_left / siege);
        }
        const int kp = SQ_PICK_K > 0 && W.n_squads < W.planned_squads ? std::min(squad_n, SQ_PICK_K) : squad_n; // ONE_SQUAD phase
        const float t = time_of(gi, std::max(1, kp));
        return t < 0 ? -1 : value_of(gi) / t;
    };
    // team gain per round from adding a k-th champion to group gi (claimed by k-1 others)
    auto marginal = [&](int gi, int k) -> float {
        const auto& g = W.groups[gi];
        const bool started = g.hp_seen >= 0 && g.hp_seen < g.max_hp; // no regeneration: partial work persists
        // i-opening: a group chipped by a range-4 cast is "started", and Cassi alone kept poking an expanse wolf pair for
        // 54 rounds as a side job; it only counts when the claimants can finish what is left of it
        std::vector<std::pair<float, float>> live;
        if (OP_MARG && started) {
            for (size_t m = 0; m < g.hp_member.size(); ++m) live.push_back({g.hp_member[m], g.member_str[m]});
            std::sort(live.begin(), live.end(), [](auto x, auto y) { return x.second > y.second; });
        }
        auto can = [&](int n) { return g.need <= n || (started && (!OP_MARG || tankable(live, n))); };
        if (!can(k)) return 0;
        const float tk = time_of(gi, k);
        if (tk < 0) return 0;
        float gain = value_of(gi) / tk;
        if (k > 1 && can(k - 1)) gain -= value_of(gi) / time_of(gi, k - 1);
        return gain;
    };

    const int cur_st = W.squad_target(my_sq);
    const bool fresh_invalid = cur_st >= 0 && W.groups[cur_st].last_seen >= W.round - 2 && score_of(cur_st, team_n) <= 0
        && !(W.squad_help() && score_of(cur_st, W.alive_count()) > 0);
    if (W.is_leader() || fresh_invalid) {
        const int cur = W.squad_target(my_sq);
        auto pick = [&](int squad_n, int excl) {
            float best = -1;
            int bi = -1;
            for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
                if (gi == excl) continue;
                float sc = score_of(gi, squad_n);
                if (gi == cur) {
                    const auto& g = W.groups[gi];
                    // never walk away from a fight in progress (allies would idle next to awake monsters)
                    sc *= (g.awake && g.hp_seen > 0 && g.hp_seen < g.max_hp) ? 4.f : 1.3f;
                }
                if (sc > best) { best = sc; bi = gi; }
            }
            return bi;
        };
        // Squad 0 leads: it may call the whole team (help flag) onto a group too big for one squad, and
        // squad 1 then joins. Squad 1 never calls, so the two leaders cannot chase each other's targets.
        // (Needs are estimated from each champion's own stats, so only the explicit flag is trusted.)
        // A squad commits to its target until it is captured or becomes unreachable in time (re-deciding
        // every turn made squads criss-cross the map); only then is a new one picked.
        auto valid = [&](int gi, int n) { return gi >= 0 && score_of(gi, n) > 0; };
        int bi = -1;
        bool help = false;
        const int other_n = W.n_squads > 1 ? W.squad_alive(1 - my_sq) : 0;
        const int all = W.alive_count();
        if (my_sq == 1 && other_n > 0) {
            if (W.squad_help()) bi = other;
            else if (cur >= 0 && cur != other && valid(cur, team_n)) bi = cur;
            else {
                bi = pick(team_n, other);
                if (bi < 0) bi = other; // nothing this squad can do alone: join squad 0
            }
        } else if (other_n > 0 && W.squad_help() && valid(cur, all)) {
            bi = cur; // keep the whole-team job
            help = true;
        } else if (!W.squad_help() && cur >= 0 && (cur != other || other_n == 0) && valid(cur, team_n)) {
            bi = cur;
            // i-opening 4.4: top teams re-take their own groups on the respawn tick (median lag 3 rounds, ours 22): a group
            // that respawns as we arrive pre-empts a target not yet woken (camp hp persists, so it can be finished later)
            if (OP_PRE && !W.groups[cur].awake) {
                const int alt = pick(team_n, other >= 0 && other != cur ? other : -1);
                if (alt >= 0 && alt != cur && score_of(alt, team_n) > PRE_X * score_of(cur, team_n)) bi = alt;
            }
        } else {
            bi = pick(team_n, other >= 0 && other != cur ? other : -1);
            const float sb = bi >= 0 ? score_of(bi, team_n) : 0.f;
            if (other_n > 0) {
                const int ti = pick(all, -1);
                if (ti >= 0 && W.groups[ti].need > team_n && (bi < 0 || score_of(ti, all) > 2.f * sb)) { bi = ti; help = true; }
            }
        }
        debug_block({ for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) if (!W.groups[gi].monolith && W.groups[gi].str >= 240 && W.herd_map && !W.boss_map)
            std::printf("LEAD R%d i%d pick g%d sc=%.3f herd g%d sc=%.3f n=%d siege=%.1f t1=%.1f\n", W.round, W.me_idx, bi, bi >= 0 ? score_of(bi, team_n) : -9.f, gi,
                        score_of(gi, team_n), team_n, W.siege_of(team_n >= W.alive_count() ? -1 : my_sq), time_of(gi, 1)); });
        W.set_squad_target(bi, help);
    }
    int st = W.squad_target(my_sq);
    int t = -1;
    if (st >= 0 && st < static_cast<int>(W.groups.size()) && W.groups[st].skip_until <= W.round && st != capturing) t = st;
    if (t < 0) {
        float best = -1;
        for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
            float sc = score_of(gi, team_n);
            if (sc > best) { best = sc; t = gi; }
        }
    }
    // Leave the squad for a better job when the squad can do its target without us (claims in shared memory).
    // A side job, once taken, is kept until done or impossible (re-deciding every turn made parties oscillate).
    const bool keep_side = target >= 0 && target != st && W.round < commit_until && marginal(target, 1 + W.claims(target)) > 0;
    if (keep_side) t = target;
    else if (t >= 0 && t == st) {
        const int ks = 1 + W.claims(st);
        const auto& sg = W.groups[st];
        const bool idle = sg.respawn_at > W.round + W.dist_to_group(st, p) + 8; // would only stand and wait
        const bool sieging = sg.need > team_n && !(W.squad_help() && sg.need <= W.alive_count());
        const bool stack = SQ_STACK && W.n_squads < W.planned_squads; // ONE_SQUAD phase (world.h)
        if (ks - 1 >= (stack ? SQ_STACK_X * sg.need : sg.need) || idle || (sieging && (SIEGE_REACH ? siege_vs(W.me_idx, st) : W.siege_dps[W.me_idx]) <= 0)) {
            if (!OP_GOLD) gold_w = W.boss_map ? BOSS_GOLD_W : 0.05f;
            const float stay = sieging ? 0.f : marginal(st, ks); // only range-4 casters help a siege
            float best = 1.25f * stay;
            for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
                if (gi == st || W.dist_to_group(gi, p) > (stack && SQ_SIDE_R > 0 ? SQ_SIDE_R : SIDE_RANGE)) continue;
                float m = marginal(gi, 1 + W.claims(gi));
                if (m > best) { best = m; t = gi; }
            }
            if (t != st) commit_until = W.round + 25;
        }
    }
    if (t != target) wait_turns = 0;
    target = t;
    W.set_claim(t);
}

// One champion captures each kill (the reward is not split): of the squad near the dead group, the one with
// the least gold (published gold buckets; ally gold is self-only), then lowest slot. Spreading captures
// lets every champion complete its build; the rest move straight on to the next camp.
static bool recalling(const bc::champion& a) {
    try { return a.is_recalling(); } catch (...) { return false; }
}

bool Bot::capture(const std::vector<bc::champion>& allies, bool safe) {
    auto& me = bc::self();
    const vec2 p = me.position();
    int mine = -1; // reward group I should capture
    int hold = -1; // y-contest MONO_SAFE: monolith reward we wait next to
    // v2 fix: "near" is Chebyshev 4 AND <= 5 steps on the group's field. With walls or holes in between the path to a
    // reward 4 tiles away first leads away from it, and the champion flipped between capture and farm/tribute every
    // turn (contest: a Ren carrying 200 gems for 900 rounds).
    auto near = [&](int gi, vec2 q) { return min_dist(q, W.groups[gi].tiles) <= 4 && W.group_field(gi).get(q) <= 5; };
    for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
        auto& g = W.groups[gi];
        // f-contest K10: only rewards seen this turn: a stale reward (monolith hidden behind bushes since its last capture) had
        // Ren bounce between it and a gold triple for 700 rounds on contest
        if (!g.reward || g.last_seen != W.round || (gi == cap_skip_group && W.round < cap_skip_until)) continue;
        if (!near(gi, p)) {
            for (const auto& a : allies)
                if (g.spawns.is_set(a.position())) {
                    g.skip_until = W.round + 8; // being captured
                    // i-opening 4.4: it pays within 6 rounds and respawns g.respawn after that; champions that moved
                    // on keep the timer (they used to learn it only by seeing the group again: 22-round re-take lag)
                    if (g.est_respawn == 0) g.est_respawn = W.round + 6 + g.respawn;
                }
            continue;
        }
        bool me_first = true;
        const bool mar = FUNNEL && me.champion_name() == bc::champion_type::marcella; // bot.h
        const bool on = g.spawns.is_set(p);
        const auto& gfi = W.group_field(gi);
        const int md = gfi.get(p);
        for (const auto& a : allies) {
            const int s = W.slot_of(a);
            // T2: an ally already on a reward tile is capturing, even "away" (recalling on the tile): a second
            // capturer split ambush's 106-gem payouts 53/53 and then recalled too
            // u-squad CAP_ONE: also when I am on a tile myself, if that ally is recalling there (it took the capture first; I walked
            // onto a freed tile on my way past: gen-03 Stellar onto Ryth's 66-gem payout, Ryth onto Issac's gold)
            if (OP_T2 && s >= 0 && g.spawns.is_set(a.position()) && (!on || (CAP_ONE && recalling(a)))) { me_first = false; break; }
            if (s < 0 || W.slot_away(s) || !near(gi, a.position()) || mar) continue;
            if (FUNNEL && a.champion_name() == bc::champion_type::marcella) { me_first = false; break; }
            const int b = W.gold_bucket(s);
            // q-act F3: same bucket -> nearest to the reward tiles first, then draft slot
            const int ad = CAP_NEAR ? gfi.get(a.position()) : md;
            if (b < W.my_bucket || (b == W.my_bucket && (ad < md || (ad == md && s < W.me_idx)))) { me_first = false; break; }
        }
        // y-contest MONO_SAFE: until someone is on its tile, everyone near it waits; the first capturer starts only when safe
        if (MONO_SAFE && g.monolith && !on) {
            const int gate = mono_gate(gi, allies);
            if (gate != 2 && (gate == 1 || !me_first)) {
                if (mono_hold(gi)) hold = gi;
                else g.skip_until = W.round + 8; // held MONO_HOLD rounds: leave it
                continue;
            }
        }
        if (!me_first) g.skip_until = W.round + 8;
        else if (mine < 0) mine = gi;
    }
    if (mine < 0) {
        if (MONO_SAFE && hold >= 0) { target = hold; W.set_claim(hold); cast_support(false); return true; }
        return false;
    }
    target = mine;
    W.set_claim(mine);
    if (!W.groups[mine].spawns.is_set(p)) {
        const auto& gf = W.group_field(mine);
        if (pvp::HOLD_WALK && pvp::hold_round == W.round && pvp::hold_gi == mine) pvp::avoid_on = false; // k-hold H1: winning there
        if (!dash_field(gf)) step_field(gf);
        if (W.groups[mine].spawns.is_set(me.position())) { cap_group = mine; cap_round = W.round; }
        // f-contest K14: an enemy parked on a single-tile reward (contest monolith) kept Ren "capturing" next to it for 1100 rounds
        cap_blocked = me.position() == p ? cap_blocked + 1 : 0;
        if (cap_blocked > 10) { W.groups[mine].skip_until = cap_skip_until = W.round + 40; cap_skip_group = mine; cap_blocked = 0; target = -1; }
        return true;
    }
    cap_blocked = 0;
    // f-tempo: recall ON the capture tile (top teams: capture at r75 at the far camp, shop at spawn at r76). A tile
    // occupied in round N pays at the round start of N+6; a recall started in N+1 lands at our turn start in N+6,
    // after that payout. The 5 capture rounds are spent waiting anyway, so the trip home costs nothing.
    const bool held = cap_group == mine && cap_round == W.round - 1;
    cap_group = mine;
    cap_round = W.round;
    if (held && safe && home_after_capture(mine)) {
        try { me.recall(); W.set_away(); g_acts += "R"; } catch (...) {}
    }
    return true;
}

// Whether, with the payout of group gi in the bag, this champion would go home right away anyway (the shop-recall
// rules of play_turn, evaluated on the predicted bag).
bool Bot::home_after_capture(int gi) const {
    auto& me = bc::self();
    const vec2 p = me.position();
    const auto& g = W.groups[gi];
    const int gems = static_cast<int>(me.gem_count()) + g.gems;
    const int gold = static_cast<int>(me.gold_count()) + g.gold;
    const int home = W.home_field().get(p);
    const int banked = static_cast<int>(bc::global_submitted_gems(team::ally));
    const bool shut = SEAL_NOPARK && g_seal == 2 && seal_closed(); // z-boss: no trip, the bag never holds back a shop recall
    const bool tribute = !shut && (gems >= bag_cap() || (gems >= TRIB_NEAR && W.altars_field().get(p) <= TRIB_RAD)
        || banked + gems >= static_cast<int>(bc::constants::winning_gems));
    if (tribute) return (gold >= 150 || hp_frac(me) < 0.6f) && W.spawn_altar < home;
    // i-opening 4.3 (F1 plus): top teams recall on the capture tile after 45-84% of their payouts (ours 25-29%),
    // shop 3-shard bundles early and are back on the next near group from spawn; the recall overlaps the capture wait
    if (OP_SHOP && home > 2 && me.passive() != bc::passive_type::remote_shop
        && (gold >= 150 || (home <= SHOP_HOME && (gold >= 90 || hp_frac(me) < 0.8f)))) return true;
    return (gems == 0 || shut) && home > 12 && ((gold >= 300 && me.passive() != bc::passive_type::remote_shop) || hp_frac(me) < 0.5f);
}
