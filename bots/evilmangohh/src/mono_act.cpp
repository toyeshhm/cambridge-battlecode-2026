// p-monolith (mono.h): the take's actions: walking out, other camps' threat, spells on the monolith.
#include "bot.h"
#include "actions.h"
#include "pvp.h"
#include "world.h"
#include <algorithm>
#include <limits>
#include <variant>

// out of range of the monolith tiles: down a static field to the tiles 6+ away (cached per group). Allies are no walls: a
// field around them (escape_danger's) changed every round and Ryth stepped back and forth at 4-5 under fire (cb3 debug, died).
// Only strictly better steps; true = handled the turn (stay put rather than walk back in)
bool mono::get_out(const std::vector<vec2>& mt) {
    static Field ef; // function-local: no static initialiser in the all-off build
    static std::vector<vec2> ef_mt;
    if (ef_mt != mt) {
        bc::bitboard roots, in;
        roots.clear(), in.clear();
        for (auto t : mt) roots.draw_chebyshev(t, static_cast<float>(TAKE_STAGE));
        for (auto t : mt) in.draw_chebyshev(t, static_cast<float>(TAKE_STAGE - 1));
        roots &= ~in;
        roots &= ~W.static_walls;
        ef.compute(W.static_walls, roots, W.visited);
        ef_mt = mt;
    }
    auto& me = bc::self();
    const vec2 p = me.position();
    const int d = min_dist(p, mt);
    if (d >= TAKE_STAGE) return false;
    // the healthiest walks out last: hold while a weaker ally in the take is not yet farther out than me (k2 debug: everyone
    // left at once, the two from the adjacent tiles stayed nearest and took both monoliths' hits, three died). The hits go to
    // the healthiest of the nearest (priority by hp), so the one holding covers the others.
    // Only weaker allies strictly nearer than me, and at most HOLD_MAX rounds of an escape (k5 debug: Issac held at 5 for a
    // Cassi who was farming at 5, not leaving, and died there).
    static int hold_round = -10, held = 0;
    if (hold_round < W.round - 1) held = 0;
    if (held < 4)
        for (int i = 0; i < W.n_allies && i < 6; ++i) {
            if (i == W.me_idx || !W.alive[i]) continue;
            bc::detail::champion_info ci{};
            if (bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(W.ally_h[i])), &ci) == 0 && ci.x >= 0
                && ci.health < me.health() && min_dist({ci.x, ci.y}, mt) < d) { hold_round = W.round, ++held; return true; }
        }
    const bool was = pvp::avoid_on;
    pvp::avoid_on = false;
    // never back toward it: equal steps sideways round an ally that blocks the way (cb4: escape_danger's detour went back in)
    if (ef.get(p) >= FAR) escape_danger(mt);
    else {
        // spread out on the way: a step next to fewer allies first (cb5: three in single file east of (32,18), the last one
        // stuck behind the others took 5 hits while the open north was as near)
        const auto occ = bc::vision::champion_tiles();
        best_step([&](vec2 q) -> float {
            const float d = ef.get(q);
            if (d >= FAR) return std::numeric_limits<float>::infinity();
            if (q == p) return d + 0.5f;
            int n = 0;
            for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) n += (dx || dy) && bc::map::is_in_bounds(vec2{q.x + dx, q.y + dy}) && occ.is_set(vec2{q.x + dx, q.y + dy});
            return d + 0.15f * static_cast<float>(n);
        });
    }
    pvp::avoid_on = was;
    return true;
}

// an awake camp of another group can hit me here (its own range): a recall would be cut, the normal flow handles it
bool mono::camp_threat(vec2 p, int skip_gi) {
    for (const auto& c : W.camps) {
        const vec2 t = c.spawn_position();
        if (skip_gi >= 0 && W.groups[skip_gi].spawns.is_set(t)) continue;
        try { if (c.is_awake() && c.strength() > 0 && cheb(p, c.position()) <= reach_at(t)) return true; } catch (...) {}
    }
    return false;
}

// stun duration on a live member tile: 2 = cast this round, 1 = spent, 0 none
static int stun_of(const bc::camp& c) {
    try { const auto b = c.buffs(); return b.has_stun() ? b.get<bc::buff_stun>().duration : 0; } catch (...) { return 2; }
}

// Spells on the monolith(s): a stun only on a monolith nobody stunned this round; damage spells freely. Shield when tanking.
void mono::spells(const std::vector<bc::camp>& ms, bool moved, bool tank) {
    auto& me = bc::self();
    if (!me.can_cast()) return;
    const vec2 p = me.position();
    for (const auto& a : me.abilities()) {
        if (!a.info() || !a.is_ready() || (moved && exclusive(a))) continue;
        const float range = ability_range(a);
        try {
            if (a.is_shield()) { if (tank && me.health() < 0.95f * me.max_health() && a.use()) return; continue; }
            if (a.is_heal()) { if (cast_support(false)) return; continue; }
            bool stun = false;
            std::visit([&](const auto& v) { if constexpr (requires { v.duration; }) stun = v.duration > 0; }, *a.info());
            for (const auto& c : ms) {
                if (stun && stun_of(c) >= 2) continue;
                const vec2 m = c.position();
                if (a.is_dash()) {
                    if (!stun) continue;
                    for (int dx = -2; dx <= 2; ++dx)
                        for (int dy = -2; dy <= 2; ++dy) {
                            const vec2 q{p.x + dx, p.y + dy};
                            if (cheb(p, q) < 1 || cheb(p, q) > static_cast<int>(range) || cheb(q, m) != 1 || !bc::map::is_in_bounds(q)
                                || W.static_walls.is_set(q) || (bc::vision::champion_tiles() | bc::vision::camp_tiles()).is_set(q)) continue;
                            try { if (a.use(q)) return; } catch (...) {}
                        }
                    continue;
                }
                if (cheb(p, m) > static_cast<int>(range)) continue;
                if (a.is_aoe_damage() ? a.use(m) : (a.is_damage() || a.is_cc()) ? a.use(c) : false) return;
            }
        } catch (...) {}
    }
}
