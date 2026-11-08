// p-fix engine camp-stat helper (zreach.h). Named to sort last: the linker keeps the first copy of shared inline code, so
// with every switch off the binary is rr-c2 byte for byte. Zero-initialised statics only: no static initialiser, so the all-off build
// links none of this and stays rr-c2's binary.
#include "zreach.h"
#include "actions.h"
#include <algorithm>
#include <variant>

namespace {
constexpr int KINDS = 6, MAXR = 8;
CampStat tab[KINDS];
bool have[KINDS];
float siege_tab[8][MAXR + 1]; // [slot][reach needed]: design damage per round of abilities reaching that far
bool siege_ready;

CampStat fallback(int k) { // the patch note's values, if the engine query fails
    switch (k) {
    case 0: return {1350, 20, 3};
    case 1: return {2400, 60, 4};
    case 4: return {20000, 200, 5};
    case 5: return {5000, 120, 4};
    default: return {120, 0, 3};
    }
}

void build_siege() {
    siege_ready = true;
    for (int i = 0; i < W.n_allies && i < 8; ++i) {
        const auto& d = bc::champion_designs[static_cast<size_t>(W.ally_h[i].champion_name())];
        const float focus = d.base_stats().base_focus;
        for (const auto& a : d.abilities()) {
            if (!a.info()) continue;
            std::visit([&](const auto& x) {
                using X = std::decay_t<decltype(x)>;
                float hits = 0, rad = 0;
                if constexpr (std::is_same_v<X, bc::ability_damage> || std::is_same_v<X, bc::ability_cc>) hits = 1;
                if constexpr (std::is_same_v<X, bc::ability_aoe_damage>) hits = 2, rad = x.radius;
                if constexpr (requires { x.range; x.modifier; })
                    if (hits > 0 && x.max_cooldown > 0)
                        for (int r = 0; r <= MAXR; ++r)
                            if (x.range + rad >= static_cast<float>(r)) siege_tab[i][r] += hits * x.modifier * focus / x.max_cooldown;
            }, *a.info());
        }
    }
}
} // namespace

const CampStat& camp_stat(bc::camp_type k) {
    static CampStat none{500, 0, 3};
    const int i = static_cast<int>(k);
    if (i < 0 || i >= KINDS) return none;
    if (!have[i]) {
        have[i] = true;
        bc::detail::camp_design d{};
        if (bc::detail::sdk_camp_design(i, &d) == 0)
            tab[i] = {d.base_stats.base_health, d.base_stats.base_strength, static_cast<int>(d.base_stats.base_attack_range)};
        else tab[i] = fallback(i);
    }
    return tab[i];
}

int reach_at(vec2 t) {
    static std::vector<std::int8_t> rt; // tile -> range of the camp spawned there (0 elsewhere), built on first use
    const int w = bc::map::width();
    if (rt.empty()) {
        rt.assign(static_cast<size_t>(w) * bc::map::height(), 0);
        for (const auto& g : W.groups)
            for (size_t k = 0; k < g.tiles.size(); ++k)
                rt[static_cast<size_t>(g.tiles[k].y) * w + g.tiles[k].x] = static_cast<std::int8_t>(reach(g.members[k].kind()));
    }
    if (!bc::map::is_in_bounds(t)) return 3;
    const int r = rt[static_cast<size_t>(t.y) * w + t.x];
    return r > 0 ? r : 3;
}

int slack(vec2 q, const std::vector<vec2>& pts) {
    int s = 1 << 20;
    for (vec2 m : pts) s = std::min(s, cheb(q, m) - reach_at(m));
    return s;
}

int esc_slack(vec2 q, const std::vector<vec2>& pts) {
    int s = 1 << 20;
    for (vec2 m : pts) s = std::min(s, cheb(q, m) - esc_reach(m));
    return s;
}

bool camp_live_at(vec2 t) {
    const int gi = W.group_of_spawn(t);
    if (gi < 0) return false;
    const auto& g = W.groups[gi];
    if (g.reward || g.respawn_at > W.round) return false;
    for (size_t k = 0; k < g.tiles.size(); ++k)
        if (g.tiles[k] == t) return g.respawn_at != 0 || g.hp_member[k] > 0; // respawned since: alive again
    return false;
}

int group_reach(int gi) {
    // REACH_CACHE (p-fix judge): design kinds and member_str never change, so cache per group (the per-call member loop
    // x 6 allies in siege_of_vs put plan's target pick at ~22k fuel on 00stampede's herd turns)
    if constexpr (REACH_CACHE) {
        static std::vector<std::int8_t> gr;
        if (gr.empty())
            for (const auto& g : W.groups) {
                int r = 3;
                for (size_t k = 0; k < g.members.size(); ++k)
                    if (g.member_str[k] > 0) r = std::max(r, reach(g.members[k].kind()));
                gr.push_back(static_cast<std::int8_t>(r));
            }
        return gr[static_cast<size_t>(gi)];
    }
    const auto& g = W.groups[gi];
    int r = 3;
    for (size_t k = 0; k < g.members.size(); ++k)
        if (g.member_str[k] > 0) r = std::max(r, reach(g.members[k].kind()));
    return r;
}

bool exit_due(vec2 p, float hp) {
    static float prev = -1;
    static int prev_r = -10;
    const float drop = prev_r == W.round - 1 ? prev - hp : 0.f;
    prev = hp, prev_r = W.round;
    if (drop <= 0) return false;
    int steps = 0;
    for (const auto& c : W.camps) { // live visible camps (they never leave their spawn tile)
        const vec2 t = c.spawn_position();
        const int gi = W.group_of_spawn(t);
        if (gi < 0 || !W.groups[gi].awake || camp_stat(c.kind()).str <= 0) continue;
        const int r = reach(c.kind()), d = cheb(p, t);
        if (d <= r) steps = std::max(steps, r - d + 1);
    }
    return steps > 0 && drop * static_cast<float>(steps) >= hp;
}

bool unseen_escape(vec2 p, float hp, bool no_enemy, int target) {
    static float prev = -1;
    static int prev_r = -10;
    bool hit = prev_r == W.round - 1 && hp < prev - 1.f && no_enemy;
    prev = hp, prev_r = W.round;
    for (const auto& c : W.camps) // a visible awake camp in reach explains the hit
        if (hit && camp_stat(c.kind()).str > 0 && cheb(p, c.spawn_position()) <= reach(c.kind())) {
            std::uint32_t st = 0;
            if (bc::detail::sdk_camp_state(static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(c)), &st) == 0 && st == 1) hit = false;
        }
    std::vector<vec2> hd;
    for (const auto& g : W.groups) {
        if (!g.monolith || g.reward || g.respawn_at > W.round) continue;
        const bool seen = g.last_seen == W.round;
        if (!(g.awake && W.round - g.last_seen <= 10) && !(hit && !seen)) continue;
        for (size_t k = 0; k < g.tiles.size(); ++k)
            if (g.member_str[k] > 0 && g.hp_member[k] > 0 && cheb(p, g.tiles[k]) <= reach_at(g.tiles[k])) hd.push_back(g.tiles[k]);
    }
    if (hd.empty()) return false;
    if (target >= 0 && target < static_cast<int>(W.groups.size())) {
        auto& tg = W.groups[target];
        for (vec2 t : tg.tiles)
            if (slack(t, hd) < 1) { tg.skip_until = W.round + 30; break; }
    }
    return escape_danger(hd);
}

float siege_vs(int slot, int gi) {
    if (slot < 0 || slot >= 8 || W.groups[gi].monolith) return 0;
    if (!siege_ready) build_siege();
    return siege_tab[slot][std::min(MAXR, group_reach(gi) + 1)];
}

float siege_of_vs(int squad, int gi) {
    float s = 0;
    for (int i = 0; i < W.n_allies && i < 6; ++i)
        if (W.alive[i] && (squad < 0 || W.squad_of[i] == squad)) s += siege_vs(i, gi);
    return s;
}
