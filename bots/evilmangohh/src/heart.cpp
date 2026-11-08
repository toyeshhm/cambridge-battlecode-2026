// heart.cpp -- our Heart of Ruin (heart.h). Reached only behind HOR_PLAY.
#include "heart.h"
#include "actions.h"
#include "pvp.h"
#include "world.h"
#include <algorithm>
#include <array>
#include <iostream>

namespace hor {

bool escaping = false;
float strike = 0, shield_v = 0;

static const std::array<vec2, 8> DIRS{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                      vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

bool dash_path(vec2 a, vec2 b, const bc::bitboard& camps) {
    const int d = cheb(a, b);
    if (d == 1) return true;
    if (d != 2) return false;
    for (auto e : DIRS) { // F15: the engine refuses a dash whose only intermediate tiles hold camps (error 32)
        const vec2 m = a + e;
        if (cheb(m, b) == 1 && bc::map::is_in_bounds(m) && !bc::map::walls().is_set(m) && !camps.is_set(m)) return true;
    }
    return false;
}

static bool near_camp(vec2 q, const bc::bitboard& camps, const std::vector<vec2>& except) {
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 t{q.x + dx, q.y + dy};
            if (bc::map::is_in_bounds(t) && camps.is_set(t) && std::find(except.begin(), except.end(), t) == except.end()) return true;
        }
    return false;
}

bool quiet(vec2 q) { return !near_camp(q, bc::vision::camp_tiles(), {}); }

bool want_shield(bool in_danger) { return in_danger && !bc::self().buffs().has_shield(); }

void preshield() {
    auto& me = bc::self();
    if (!me.can_cast() || me.buffs().has_shield() || hp_frac(me) < 0.4f) return;
    for (const auto& a : me.abilities())
        try { if (a.info() && a.is_shield() && a.is_ready() && a.use()) { debug_block({ g_acts += 'P'; }); return; } } catch (...) {}
}

void squad(const std::vector<bc::champion>& allies, std::array<int, 8>& sq) {
    int n1 = 0, h = -1;
    for (int i = 0; i < static_cast<int>(allies.size()) && i < 8; ++i) {
        if (allies[i].champion_name() == bc::champion_type::heart_of_ruin) h = i;
        else n1 += sq[i] == 1;
    }
    if (h >= 0) sq[h] = n1 < 3 ? 1 : 0;
}

bool engage(const std::vector<bc::camp>& members) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    const auto abs = me.abilities();
    int slot = -1;
    for (int s = 0; s < 2; ++s) if (abs[s].is_dash() && abs[s].is_ready()) slot = s;
    if (slot < 0) return false;
    const vec2 p = me.position();
    std::vector<vec2> mp;   // live members
    std::vector<float> mw;  // landing worth: damage, + 3 x strength for an awake attacker not yet stunned (its hit is skipped)
    for (const auto& c : members) try {
        const float hp = c.health();
        if (hp <= 0) continue;
        float w = std::min(175.f, hp); // ponytail: base landing damage (a cluster dies to it: 120 hp)
        if (c.strength() > 0) w += 50.f + (c.is_awake() && !c.buffs().has_stun() ? 3 * c.strength() : 0.f);
        mp.push_back(c.position()), mw.push_back(w);
    } catch (...) {}
    if (mp.empty()) return false;
    const auto camps = bc::vision::camp_tiles();
    const auto occ = bc::vision::champion_tiles() | camps | bc::vision::teleport_tiles();
    const bool walk = me.can_move();
    vec2 bq = p, bw = p;
    float bs = 0;
    std::vector<vec2> tried;
    for (vec2 m : mp)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const vec2 q{m.x + dx, m.y + dy};
                const int d = cheb(p, q);
                if (d == 0 || d > (walk ? 3 : 2) || std::find(tried.begin(), tried.end(), q) != tried.end()) continue;
                tried.push_back(q);
                if (!bc::map::is_in_bounds(q) || W.static_walls.is_set(q) || occ.is_set(q)) continue;
                if (pvp::avoid_on && pvp::avoid.is_set(q)) continue;
                if (near_camp(q, camps, mp)) continue; // never wake another group
                float s = 0;
                for (size_t i = 0; i < mp.size(); ++i) if (cheb(q, mp[i]) <= 1) s += mw[i];
                vec2 w = p;
                if (d == 3 || !dash_path(p, q, camps)) {
                    w = {-1, -1};
                    if (walk)
                        for (auto e : DIRS) {
                            const vec2 t = p + e;
                            if (bc::map::is_in_bounds(t) && !W.static_walls.is_set(t) && !occ.is_set(t) && cheb(t, q) <= 2
                                && t != q && dash_path(t, q, camps) && !(pvp::avoid_on && pvp::avoid.is_set(t))) { w = t; break; }
                        }
                    if (w.x < 0) continue;
                    s -= 1; // ties: keep the walk
                }
                if (s > bs) bs = s, bq = q, bw = w;
            }
    if (bs <= 0) return false;
    try {
        if (bw != p && !me.move_absolute(bw)) return false;
        if (!abs[slot].use(bq)) return false;
        debug_block({ g_acts += 'H'; std::cout << "HENGAGE R" << W.round << " p=" << bc::to_string(p) << " w=" << bc::to_string(bw) << " q=" << bc::to_string(bq) << " s=" << bs << " atk=" << me.can_attack() << " mv=" << me.can_move() << "\n"; });
        return true;
    } catch (...) { return false; }
}

} // namespace hor
