// c7m GATE (bot.h): altars walled in by camps. Geometry only, once per champion at a turn end (main.cpp, like seal_detect).
#include "bot.h"
#include "actions.h"
#include "world.h"
#include "pvp.h"
#include "zreach.h"
#include <iostream>

namespace {
struct St {
    int state = 0;          // 0 not run yet, 1 no gate, 2 gated (every altar's submit tiles unreachable with camps as walls)
    int ggi = -1, gk = -1;  // the gate: group / member whose death opens the cheapest path (lowest hp, then nearest our spawn)
    Field near_f, open_f;   // to the gate's outer neighbours; to the altars through the dead gate (other camps stay walls)
    Field inner;            // from the submit tiles with camps as walls: < FAR = inside the camps' wall
    bool open_ready = false;
};
// ponytail: a function static, not namespace globals (their Field destructors were emitted with GATE off: off != base)
St& st() { static St v; return v; }
#define GATE_ST [[maybe_unused]] auto& [state, ggi, gk, near_f, open_f, inner, open_ready] = st()

bc::bitboard camp_tiles() {
    bc::bitboard b;
    b.clear();
    for (const auto& g : W.groups) for (auto t : g.tiles) b.set(t);
    return b;
}
bc::bitboard submit_tiles() {
    bc::bitboard b;
    b.clear();
    for (auto a : W.altars) b.draw_chebyshev(a, static_cast<float>(bc::constants::submit_gems_range));
    return b;
}
bool gate_open() {
    GATE_ST;
    const auto& g = W.groups[ggi];
    return g.reward || g.respawn_at != 0 || g.hp_member[gk] <= 0;
}
} // namespace

// Two BFS on a gated map, one elsewhere (spawn reaches a submit tile: done). ponytail: single-camp gates only; a wall two
// camps thick has no candidate and stays as before (0-1 BFS over camp tiles if such a map shows up). Gates over GATE_HP_MAX
// are no candidates: 02guardians / 01boss-battle (monolith seals) stay SEAL_* / MONO_TAKE's, as in v31 (c7gp's screen had
// them gated: walks there ignored the avoid mask once the seal was open).
void gate_detect() {
    GATE_ST;
    if (state || W.ally_h.empty() || bc::fuel_remaining() < 60000) return;
    const auto walls = W.static_walls | camp_tiles();
    const auto sub = submit_tiles();
    auto roots = sub & ~walls;
    Field& fin = inner;
    Field fout;
    if (!roots.is_empty()) {
        fin.compute(walls, roots, W.visited);
        for (auto s : bc::map::spawn_region(team::ally).positions())
            if (fin.get(s) < FAR) { state = 1; return; }
    }
    fout.compute(walls, bc::map::spawn_region(team::ally), W.visited);
    static const vec2 dirs[8]{{0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}};
    float best = 1e30f;
    for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi)
        for (int k = 0; k < static_cast<int>(W.groups[gi].tiles.size()); ++k) {
            const vec2 t = W.groups[gi].tiles[k];
            bool in = sub.is_set(t);
            int out = FAR;
            for (auto d : dirs) {
                const vec2 n = t + d;
                out = std::min<int>(out, fout.get(n));
                in |= !roots.is_empty() && fin.get(n) < FAR;
            }
            const float c = W.groups[gi].member_hp[k] * 1000.f + static_cast<float>(out);
            if (in && out < FAR && c < best && W.groups[gi].member_hp[k] <= GATE_HP_MAX) best = c, ggi = gi, gk = k;
        }
    state = ggi >= 0 ? 2 : 1;
    if (state == 2) {
        bc::bitboard r;
        r.clear();
        for (auto d : dirs) { const vec2 n = W.groups[ggi].tiles[gk] + d; if (fout.get(n) < FAR) r.set(n); }
        auto w2 = walls;
        w2.set(W.groups[ggi].tiles[gk], false); // from inside: out through the gate tile once it is dead
        near_f.compute(w2, r, W.visited);
    }
    debug_block({ if (state == 2) std::cout << "GATE i" << W.me_idx << " g" << ggi << " k" << gk << " at " << W.groups[ggi].tiles[gk].x << "," << W.groups[ggi].tiles[gk].y << "\n"; });
}

// The tribute walk's field while the gate is dead: the altars with every other camp tile a wall (nullptr: none / not open).
const Field* gate_field() {
    GATE_ST;
    if (state != 2 || !gate_open()) return nullptr;
    if (!open_ready) {
        if (bc::fuel_remaining() < 20000) return nullptr;
        auto walls = W.static_walls | camp_tiles();
        walls.set(W.groups[ggi].tiles[gk], false);
        open_f.compute(walls, submit_tiles() & ~walls, W.visited);
        open_ready = true;
    }
    return &open_f;
}

// A tribute trip on a gated map while the gate stands: walk to it and hit it (the camps' damage does not stop a tribute,
// and they sleep two rounds after the last hit).
bool gate_turn() {
    GATE_ST;
    if (!GATE || state != 2 || gate_open()) return false;
    auto& me = bc::self();
    const vec2 p = me.position(), t = W.groups[ggi].tiles[gk];
    const std::vector<bc::camp> one{W.groups[ggi].members[gk]};
    const int r = me.attack_range();
    if (cheb(p, t) > r) {
        cast_on_camps(one, false);
        if (near_f.get(p) > 2) { if (!dash_field(near_f)) step_field(near_f); }
        else approach(t, r);
    }
    if (cheb(me.position(), t) <= r) {
        cast_on_camps(one, true);
        attack_camp(one);
        try_reset_and_attack([&] { return attack_camp(one); });
    }
    debug_block({ std::cout << "GATEHIT R" << W.round << " i" << W.me_idx << " d" << cheb(me.position(), t) << " hp=" << hp_frac(me) << " gate=" << W.groups[ggi].hp_member[gk] << "\n"; });
    return true;
}

// Farm walk from inside the wall (02the_pack: bags banked, Marcella and HoR stood at (22,24) / (21,23) with 0 gems r400-r2000:
// every static field crosses the ring, so step_field's local BFS found no way out). Recall out; walk out if the recall fails.
// Also retreat()'s first step (combat.cpp). Returns true whenever I am inside (the turn is handled).
bool gate_exit() {
    GATE_ST;
    if (state != 2 || !gate_open()) return false;
    auto& me = bc::self();
    const vec2 p = me.position();
    if (inner.get(p) >= FAR && p != W.groups[ggi].tiles[gk]) return false;
    // recall out: the way is one tile wide (the_pack: (18,20)-(19,21)-(20,22)) and the next bag waits on it, so walking
    // out locked HoR / Marcella inside and Ren with 79 gems on the gate (no swaps); sleeping camps never cut the recall
    if (me.is_recalling()) return true;
    if constexpr (GATE_WALK) { // c18b (bot.h): awake camps in reach cut every recall; walk out instead
        static int rc = -100; // my last recall here: still inside within 2 rounds = it was cut
        bool hit = W.round - rc <= 2;
        for (const auto& c : W.camps) try { hit |= c.is_awake() && c.strength() > 0 && cheb(p, c.position()) <= reach(c.kind()); } catch (...) {}
        if (!hit) try { me.recall(); rc = W.round; return true; } catch (...) {}
    } else try { me.recall(); return true; } catch (...) {}
    const bool was = pvp::avoid_on;
    pvp::avoid_on = false; // a declined fight's mask held Marcella (0 gems) one step from the gate, then farm walked her back in
    bool moved = dash_field(near_f) || step_field(near_f);
    // blocked (the way is one tile wide: a bag waiting on the gate's outer tile and me on the gate swap nothing): make room
    // deeper inside, toward the altar, so the bag passes
    if (!moved) moved = step_field(inner);
    pvp::avoid_on = was;
    debug_block({ std::cout << "GATEEXIT R" << W.round << " i" << W.me_idx << (moved ? " moved" : " held") << "\n"; });
    return true; // inside, blocked: hold (farm's fields cross the wall)
}
