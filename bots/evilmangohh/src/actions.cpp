#include "actions.h"
#include "zreach.h"
#include "pvp.h"
#include "prof.h"
#include "heart.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <variant>

std::string g_acts;
#define ACT(ch) debug_block({ g_acts += ch; })

static const std::array<vec2, 8> DIRS{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                      vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

bool is_ranged() { return bc::self().attack_range() > 1; }

bool exclusive(const bc::ability& a) {
    if (!a.info()) return false;
    return std::visit([](const auto& p) { return p.exclusive_with_movement; }, *a.info());
}

float ability_range(const bc::ability& a) {
    if (!a.info()) return 0;
    return std::visit([](const auto& p) -> float {
        if constexpr (requires { p.range; }) return p.range; else return 0.f;
    }, *a.info());
}

static bc::bitboard occupied() {
    return bc::vision::champion_tiles() | bc::vision::camp_tiles() | bc::vision::teleport_tiles();
}

bool best_step(const std::function<float(vec2)>& score) {
    auto& me = bc::self();
    if (!me.can_move()) return false;
    const vec2 p = me.position();
    const auto occ = occupied();
    struct C { float s; vec2 q; };
    std::vector<C> cands;
    const float stay = score(p);
    for (auto d : DIRS) {
        vec2 q = p + d;
        if (!bc::map::is_in_bounds(q) || W.static_walls.is_set(q) || occ.is_set(q)) continue;
        if (pvp::avoid_on && pvp::avoid.is_set(q) && !pvp::avoid.is_set(p)) continue; // declined fight: stay out of reach
        float s = score(q);
        if (s < stay && std::isfinite(s)) cands.push_back({s, q});
    }
    std::sort(cands.begin(), cands.end(), [](const C& a, const C& b) { return a.s < b.s; });
    for (const auto& c : cands) {
        try {
            if (me.move_absolute(c.q)) { ACT('m'); return true; }
        } catch (...) {}
    }
    return false;
}

// c8 STEP_STICK (actions.h): the round the window BFS last stepped us uphill on stick_f
static int stick_round = -100;
static const Field* stick_f = nullptr;

bool step_field(const Field& f, const bc::bitboard* avoid) {
    auto& me = bc::self();
    if (!me.can_move()) return false;
    const vec2 p = me.position();
    if (f.get(p) == 0) return false;
    bool moved = best_step([&](vec2 q) -> float {
        // c8 STEP_STICK: on a detour, no static step back toward the camps that blocked it (the window BFS below walks on)
        if constexpr (STEP_STICK > 0) if (q != p && &f == stick_f && W.round - stick_round <= STEP_STICK && W.round - pvp::last_seen > 2) return std::numeric_limits<float>::infinity();
        float d = f.get(q);
        if (d >= FAR) return std::numeric_limits<float>::infinity();
        if (avoid && q != p && avoid->is_set(q)) d += 6;
        return d;
    });
    PROF("sf.step");
    if (moved) { if constexpr (SF_STARVE) sf_starved() = false; return true; }
    // Blocked (live camps or allies on the static path, or every root occupied): BFS around visible
    // units from the best free tiles of the static field within a window around us.
    // ~25-35k (225-tile window scan, BFS capped at 28 rings) plus ~10k for the rest of a camp-fight turn: skipped
    // when the turn cannot afford it (an exhausted turn also loses the next round's actions), and for 3 rounds
    // after it failed to move us from this tile (ambush: a champion re-ran it every turn, boxed in by the avoid mask).
    static vec2 futile_at{-1, -1};
    static const Field* futile_f = nullptr;
    static int futile_round = -100;
    if (bc::fuel_remaining() < 48000 || (p == futile_at && &f == futile_f && W.round - futile_round <= 3)) {
        if constexpr (SF_STARVE) sf_starved() = bc::fuel_remaining() < 48000; // c18b (actions.h)
        return false;
    }
    auto walls = W.static_walls | occupied();
    walls.set(p, false);
    constexpr int R = 7;
    bc::bitboard win;
    win.clear();
    win.draw_chebyshev(p, static_cast<float>(R));
    win &= ~walls;
    win.set(p, false);
    std::array<vec2, (2 * R + 1) * (2 * R + 1)> tiles;
    const auto n = win.positions_into(tiles);
    int best = FAR;
    std::uint32_t nr = 0; // tiles[0, nr) = free tiles at the lowest field value (compacted in place, nr <= i)
    // c18b SF_STARVE: roots only among window tiles reachable from p (road_of_riches: the lowest tiles sat across a wall)
    if constexpr (SF_STARVE) { bc::bitboard src; src.clear(); src.set(p); sf_from().compute(walls, src, W.visited, static_cast<std::uint16_t>(4 * R)); }
    for (std::uint32_t i = 0; i < n; ++i) {
        if constexpr (SF_STARVE) if (sf_from().get(tiles[i]) >= FAR) continue;
        const int d = f.get(tiles[i]);
        if (d < best) best = d, nr = 0;
        if (d == best) tiles[nr++] = tiles[i];
    }
    if (best >= FAR || best >= f.get(p)) return false; // already as close as free tiles allow
    bc::bitboard roots;
    roots.clear();
    for (std::uint32_t i = 0; i < nr; ++i) roots.set(tiles[i]);
    PROF("sf.win");
    bc::distance_field dyn;
    dyn.compute(walls, roots, W.visited, static_cast<std::uint16_t>(4 * R));
    PROF("sf.bfs");
    const bool stepped = best_step([&](vec2 q) -> float {
        float d = dyn.get(q);
        return d >= FAR ? std::numeric_limits<float>::infinity() : d;
    });
    if (!stepped) futile_at = p, futile_f = &f, futile_round = W.round;
    if constexpr (STEP_STICK > 0) if (stepped && f.get(me.position()) >= f.get(p)) stick_round = W.round, stick_f = &f;
    return stepped;
}

bool approach(vec2 t, int range) {
    auto& me = bc::self();
    if (!me.can_move() || bc::fuel_remaining() < 20000) return false; // BFS up to ~15k on 100x100
    const vec2 p = me.position();
    auto walls = W.static_walls | occupied();
    walls.set(p, false);
    bc::bitboard roots, dest;
    roots.clear();
    roots.draw_chebyshev(t, static_cast<float>(range));
    roots &= ~walls;
    if (roots.is_empty() || roots.is_set(p)) return false;
    dest.clear();
    dest.set(p);
    bc::distance_field dyn;
    dyn.compute_to(walls, roots, dest, W.visited);
    return best_step([&](vec2 q) -> float {
        float d = dyn.get(q);
        return d >= FAR ? std::numeric_limits<float>::infinity() : d;
    });
}

bool escape_danger(const std::vector<vec2>& danger) {
    auto& me = bc::self();
    const vec2 p = me.position();
    // p-fix F4 ESC_RANGE: out of each monster's own range (elk 4, monolith 5: stop at 6); F2 MONO_NOSIEGE alone: monoliths 5
    if (!me.can_move() || danger.empty() || ((ESC_RANGE || MONO_NOSIEGE) ? esc_slack(p, danger) + 3 : min_dist(p, danger)) >= 4) return false;
    auto walls = W.static_walls | occupied();
    walls.set(p, false);
    bc::bitboard roots;
    roots.clear();
    roots.draw_chebyshev(p, 6.f);
    roots &= ~walls;
    for (vec2 d : danger) {
        bc::bitboard zone;
        zone.clear();
        zone.draw_chebyshev(d, (ESC_RANGE || MONO_NOSIEGE) ? static_cast<float>(esc_reach(d)) : 3.f);
        roots &= ~zone;
    }
    if (roots.is_empty()) return false;
    bc::distance_field dyn;
    dyn.compute(walls, roots, W.visited, 12);
    const bool was = pvp::avoid_on;
    pvp::avoid_on = false; // getting out of a monolith's reach beats staying out of an enemy's
    const bool moved = best_step([&](vec2 q) -> float {
        const float d = dyn.get(q);
        return d >= FAR ? std::numeric_limits<float>::infinity() : d;
    });
    pvp::avoid_on = was;
    return moved;
}

bool dash_field(const Field& f) {
    auto& me = bc::self();
    if (!me.can_move()) return false;
    const vec2 p = me.position();
    const int cur = f.get(p);
    if (cur >= FAR || cur < 3) return false;
    if constexpr (STEP_STICK > 0) if (&f == stick_f && W.round - stick_round <= STEP_STICK && W.round - pvp::last_seen > 2) return false; // c8: on a detour
    if constexpr (HOR_PLAY) if (hor::me() && !hor::escaping && cur < HOR_TRAVEL) return false; // h-heart: keep it for the fight
    for (const auto& a : me.abilities()) {
        if (!a.is_dash() || !a.is_ready()) continue;
        std::optional<bc::bitboard> occ; // built on the first tile that would beat a step (most turns: none)
        vec2 best = p;
        int bestd = cur - 1; // must beat a normal step
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                vec2 q{p.x + dx, p.y + dy};
                if (cheb(p, q) != 2 || !bc::map::is_in_bounds(q)) continue;
                int d = f.get(q); // guest read first: the host bitboard tests below cost ~50 each
                if (d >= bestd) continue;
                if (!occ) occ = occupied();
                if (W.static_walls.is_set(q) || occ->is_set(q)) continue;
                if (pvp::avoid_on && pvp::avoid.is_set(q)) continue;
                if constexpr (HOR_PLAY) if (hor::me() && !hor::quiet(q)) continue; // h-heart: the landing would wake a camp
                // need a non-wall intermediate tile (p-fix F15 DASH_CAMP: and no camp on it, engine error 32)
                bool ok = false;
                for (auto e : DIRS) {
                    vec2 m = p + e;
                    if (cheb(m, q) <= 1 && bc::map::is_in_bounds(m) && !bc::map::walls().is_set(m)
                        && (!DASH_CAMP || !camp_live_at(m))) { ok = true; break; }
                }
                if (ok) { best = q; bestd = d; }
            }
        if (best != p) {
            try {
                if (a.use(best)) {
                    ACT('D');
                    if (DASH_WALK && f.get(best) > 0) // cheap greedy step only (no BFS fallback after a dash)
                        best_step([&](vec2 q) -> float { const float d = f.get(q); return d >= FAR ? std::numeric_limits<float>::infinity() : d; });
                    return true;
                }
            } catch (...) {}
        }
    }
    return false;
}

// Visible unit state from one raw engine record: is_visible(), health(), position() and strength() each re-query
// the whole record (~4x the fuel when a camp fight scans 10+ members per ability).
static bool look(const bc::camp& c, vec2& pos, float& hp, float& str) {
    bc::detail::camp_state s{};
    if (bc::detail::sdk_camp(static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(c)), &s) != 0) return false;
    pos = {s.x, s.y}, hp = s.health, str = s.strength;
    return true;
}
static bool look(const bc::champion& c, vec2& pos, float& hp, float& str) {
    bc::detail::champion_info s{};
    if (bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)), &s) != 0 || s.x < 0 || s.y < 0) return false;
    pos = {s.x, s.y}, hp = s.health, str = s.strength;
    return true;
}

bool attack_camp(const std::vector<bc::camp>& camps) {
    auto& me = bc::self();
    if (!me.can_attack()) return false;
    const vec2 p = me.position();
    const int r = me.attack_range();
    const bc::camp* best = nullptr;
    float bests = 1e30f;
    vec2 q;
    float hp, str;
    for (const auto& c : camps) {
        if (!look(c, q, hp, str) || hp <= 0 || cheb(p, q) > r) continue;
        float s = hp - (str > 0 ? 100000.f : 0.f);
        if (s < bests) { bests = s; best = &c; }
    }
    if (!best) return false;
    try { me.attack(*best); ACT('a'); } catch (...) { return false; }
    return true;
}

bool camps_dead(const std::vector<bc::camp>& camps) {
    vec2 q;
    float hp, str;
    for (const auto& c : camps) if (look(c, q, hp, str) && hp > 0) return false;
    return true;
}

bool attack_champion(const std::vector<bc::champion>& enemies) {
    auto& me = bc::self();
    if (!me.can_attack()) return false;
    const vec2 p = me.position();
    const int r = me.attack_range();
    const bc::champion* best = nullptr;
    float bests = 1e30f;
    for (const auto& e : enemies) {
        try {
            if (!e.is_visible() || !e.is_alive() || cheb(p, e.position()) > r) continue;
            float s = e.health() - (is_channeling(e) ? 400.f : 0.f);
            if (s < bests) { bests = s; best = &e; }
        } catch (...) {}
    }
    if (!best) return false;
    try { me.attack(*best); ACT('a'); } catch (...) { return false; }
    return true;
}

bool try_reset_and_attack(const std::function<bool()>& attack_again) {
    auto& me = bc::self();
    if (me.can_attack()) return false;
    for (const auto& a : me.abilities()) {
        if (!a.is_attack_reset() || !a.is_ready()) continue;
        try {
            if (a.use()) { ACT('r'); return attack_again(); }
        } catch (...) {}
    }
    return false;
}

// A stun on a camp cancels its next round-start hit: spend it on the strongest awake attacker not yet stunned.
// Pure stuns (no damage) are kept when no such camp is in range.
static bool stun_camp(const bc::ability& a, const std::vector<bc::camp>& camps, float range) {
    const vec2 p = bc::self().position();
    const bc::camp* best = nullptr;
    float bests = 1e30f;
    vec2 q;
    float hp, str;
    for (const auto& c : camps) {
        try {
            if (!look(c, q, hp, str) || cheb(p, q) > range || str <= 0) continue;
            const float s = hp - 1000.f * str;
            if (s >= bests || !c.is_awake() || c.buffs().has_stun()) continue;
            bests = s, best = &c;
        } catch (...) {}
    }
    if (!best) return false;
    try { if (a.use(*best)) { ACT('z'); return true; } } catch (...) {}
    return false;
}

template <class Unit>
static bool cast_single(const bc::ability& a, const std::vector<Unit>& units, float range, bool prefer_dangerous) {
    const vec2 p = bc::self().position();
    if constexpr (std::is_same_v<Unit, bc::camp>) {
        if (a.is_cc()) {
            if (stun_camp(a, units, range)) return true;
            if (a.template get<bc::ability_cc>().modifier <= 0) return false;
        }
    }
    const Unit* best = nullptr;
    float bests = 1e30f;
    vec2 q;
    float hp, str;
    for (const auto& u : units) {
        if (!look(u, q, hp, str) || hp <= 0 || cheb(p, q) > range) continue;
        float s = hp;
        if constexpr (std::is_same_v<Unit, bc::camp>) { if (prefer_dangerous && str > 0) s -= 100000.f; }
        else { if (is_channeling(u)) s -= 1000.f; }
        if (s < bests) { bests = s; best = &u; }
    }
    if (!best) return false;
    try { if (a.use(*best)) { ACT('c'); return true; } return false; } catch (...) { return false; }
}

template <class Unit>
static bool cast_aoe(const bc::ability& a, const std::vector<Unit>& units, float range, float radius) {
    const vec2 p = bc::self().position();
    std::vector<vec2> pts;
    vec2 q0;
    float hp, str;
    for (const auto& u : units) if (look(u, q0, hp, str) && hp > 0) pts.push_back(q0);
    if (pts.empty()) return false;
    vec2 best = p;
    int bestn = 0;
    const int rad = static_cast<int>(radius);
    // Points within rad of each centre, counted on a local grid (the pairwise count was points^2 x 9 tests:
    // ~16k for a 10-camp group). Same scan order as before, so ties resolve the same way.
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    for (auto c : pts) x0 = std::min(x0, c.x), y0 = std::min(y0, c.y), x1 = std::max(x1, c.x), y1 = std::max(y1, c.y);
    x0 -= 2 * rad, y0 -= 2 * rad, x1 += 2 * rad, y1 += 2 * rad;
    const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    std::array<std::uint8_t, 576> cnt{};
    const bool grid = bw * bh <= static_cast<int>(cnt.size());
    if (grid)
        for (auto o : pts)
            for (int dx = -rad; dx <= rad; ++dx)
                for (int dy = -rad; dy <= rad; ++dy) ++cnt[(o.y + dy - y0) * bw + (o.x + dx - x0)];
    for (auto c : pts)
        for (int dx = -rad; dx <= rad; ++dx)
            for (int dy = -rad; dy <= rad; ++dy) {
                vec2 q{c.x + dx, c.y + dy};
                if (!bc::map::is_in_bounds(q) || cheb(p, q) > range) continue;
                int n = 0;
                if (grid) n = cnt[(q.y - y0) * bw + (q.x - x0)];
                else for (auto o : pts) if (cheb(o, q) <= rad) ++n;
                if (n > bestn) { bestn = n; best = q; }
            }
    if (bestn == 0) return false;
    try { if (a.use(best)) { ACT('o'); return true; } return false; } catch (...) { return false; }
}

template <class Unit>
static bool cast_offense(const std::vector<Unit>& units, bool allow_exclusive, bool camps) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    for (const auto& a : me.abilities()) {
        if (!a.info() || !a.is_ready()) continue;
        if (!allow_exclusive && exclusive(a)) continue;
        if (camps && !a.can_target_camps() && !a.can_target_ground()) continue;
        if (!camps && !a.can_target_enemies() && !a.can_target_ground()) continue;
        const float range = ability_range(a);
        if (a.is_damage() || a.is_cc()) {
            if (cast_single(a, units, range, true)) return true;
        } else if (a.is_aoe_damage()) {
            float radius = a.template get<bc::ability_aoe_damage>().radius;
            if (cast_aoe(a, units, range, radius)) return true;
        }
    }
    return false;
}

bool cast_on_camps(const std::vector<bc::camp>& camps, bool allow_exclusive) {
    return cast_offense(camps, allow_exclusive, true);
}
bool cast_on_enemies(const std::vector<bc::champion>& enemies, bool allow_exclusive) {
    return cast_offense(enemies, allow_exclusive, false);
}

bool cast_support(bool in_danger) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    for (const auto& a : me.abilities()) {
        if (!a.info() || !a.is_ready()) continue;
        try {
            if (a.is_heal()) {
                const float range = ability_range(a);
                const bc::champion* best = nullptr;
                float most = 0;
                for (const auto& c : bc::vision::champions(team::ally)) {
                    if (c == me || !c.is_visible() || !c.is_alive()) continue;
                    if (cheb(me.position(), c.position()) > range) continue;
                    float missing = c.max_health() - c.health();
                    if (missing > most) { most = missing; best = &c; }
                }
                if (best && most >= 0.6f * a.heal_amount()) { if (a.use(*best)) { ACT('h'); return true; } }
            } else if (a.is_shield()) {
                if (HOR_PLAY && hor::me() ? hor::want_shield(in_danger) : in_danger && me.health() < 0.9f * me.max_health()) { if (a.use()) { ACT('s'); return true; } }
            }
        } catch (...) {}
    }
    return false;
}

float champ_dps(const bc::champion& c) {
    try { return c.strength() + 0.25f * c.focus(); } catch (...) { return 60; }
}

bool is_channeling(const bc::champion& c) {
    try {
        if constexpr (PV_RECALL) if (c.is_recalling() && !c.is_submitting_gems()) return c.remaining_channel_time() <= 3; // v22
        return c.is_submitting_gems() || c.is_recalling(); } catch (...) { return false; }
}

float hp_frac(const bc::champion& c) { return c.health() / std::max(1.f, c.max_health()); }

int min_dist(vec2 q, const std::vector<vec2>& pts) {
    int m = 1 << 20;
    for (auto p : pts) m = std::min(m, cheb(q, p));
    return m;
}
