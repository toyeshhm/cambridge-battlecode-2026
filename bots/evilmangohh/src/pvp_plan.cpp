// pvp_plan.cpp -- see pvp_plan.h.
#include "pvp_plan.h"
#include "actions.h"
#include "zreach.h"
#include "heart.h"
#include <algorithm>
#include <iostream>

namespace pvp::plan {

// h-heart: tiles a dash may not pass (camps, visible foes), set by hor_setup (a function static: no static init when off)
static bc::bitboard& hor_blk() { static bc::bitboard b; return b; }

float foe_dmg_k(const Foe& f, int d, bool stun, int k) {
    if (d > fight::MAXD) return 0;
    if (stun || f.stunned) return k >= 2 ? f.st2[d] + (k >= 3 ? f.two[d] : 0.f) : 0.f; // a stun costs its next turn only
    return (k >= 1 ? f.one[d] : 0.f) + (k >= 2 ? f.two[d] : 0.f) + (k >= 3 ? f.two[d] : 0.f);
}

float foe_dmg(const Foe& f, int d, bool stun) { return foe_dmg_k(f, d, stun, f.turns); }

float contrib(const Ctx& c, int j, vec2 q, bool stun_now) {
    const Unit& u = c.F.u[c.foes[j].fi];
    if (!u.alive) return 0;
    return foe_dmg(c.foes[j], cheb(q, c.foes[j].pos), stun_now) * c.my_taken;
}

float soon_risk(const Ctx& c, int j, vec2 q) {
    const Unit& u = c.F.u[c.foes[j].fi];
    if (!u.alive) return 0;
    const int d = cheb(q, c.foes[j].pos) + 1;
    return d < fight::MAXD ? SOON_W * c.foes[j].soon[d] : 0.f;
}

float camp_danger(const Ctx& c, vec2 q) {
    float s = 0;
    for (const auto& [m, st] : c.camps) if (cheb(q, m) <= (REACH_ALL ? reach_at(m) : 3)) s += st * c.my_taken; // p-fix F5
    return s;
}

int hunt_carrier(const Ctx& c) {
    const Unit& mu = c.F.u[c.mi];
    int mates = 0, carrier = -1;
    for (int i = 0; i < c.F.n; ++i) mates += c.F.u[i].ally && c.F.u[i].alive && i != c.mi && fight::dist(c.F.u[i], mu) <= 6;
    if (mates < 2) return -1;
    const auto& es = bc::map::spawn_region(team::enemy);
    float bv = 0;
    for (int i = 0; i < c.F.n; ++i) {
        const Unit& u = c.F.u[i];
        const vec2 q{u.x, u.y};
        if (u.ally || !u.alive || u.hidden || fight::dist(u, mu) > 6 || u.hp >= 0.6f * u.max_hp || es.is_set(q)) continue;
        if (u.gems < 40 && (u.gems < HUNT_FARMER || cheb(q, W.home_c) >= cheb(q, W.away_c))) continue;
        bool lone = true;
        for (int k = 0; k < c.F.n && lone; ++k) lone = k == i || c.F.u[k].ally || !c.F.u[k].alive || fight::dist(c.F.u[k], u) > 4;
        const float v = static_cast<float>(u.gems) + 100.f * (1.f - u.hp / u.max_hp);
        if (lone && (carrier < 0 || v > bv)) carrier = i, bv = v;
    }
    return carrier;
}

namespace {

// Risk at one end tile, before removing the foes a plan kills or stuns.
struct Tile {
    // risk from each foe before our next turn (_s: if this plan stuns it; ch: one turn more each, T1c stun lock)
    std::array<float, MAXF> con{}, con_s{}, ch{}, ch_s{};
    unsigned lock = 0; // foes that can stun us on this tile before our next turn
    float base = 0, mul = 1, share = 1, pos = 0, next = 0;
    bool trap = false; // h-micro T2
};

// h-micro T2: after a turn in a round they open (n-1 foe turns), our next turn is in a round we open: n+1 foe turns
// follow it, the foe in our slot twice, with whatever it did not spend now. From q we can step one tile at that
// turn; if every such tile is lethal then, q is a trap now (fragment R536: our Stellar nuked their Stellar from 4,
// safe for the moment, and died to its two turns the next round with no tile out of reach).
bool trapped(const Ctx& c, vec2 q, float ehp) {
    if (ehp <= 0) return true;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 r{q.x + dx, q.y + dy};
            if (r != q && (!bc::map::is_in_bounds(r) || W.static_walls.is_set(r))) continue;
            float dn = 0;
            for (int j = 0; j < c.nf && dn < ehp; ++j) {
                if (!c.F.u[c.foes[j].fi].alive) continue;
                const int d = cheb(r, c.foes[j].pos);
                if (d <= fight::MAXD) dn += c.foes[j].nxt[d] * c.my_taken;
            }
            if (dn < ehp) return false;
        }
    return true;
}

// Damage bookkeeping of one action set.
struct Sim {
    std::array<float, MAXF> hp{}, sh{};
    float value = 0, retal = 0;
    unsigned removed = 0, killed = 0; // foes stunned or killed by this plan
};

void kill_foe(const Ctx& c, Sim& s, int j) { // apply() calls this once per foe (hp was > 0)
    s.value += c.foes[j].kill_v;
    s.killed |= 1u << j;
    if (c.foes[j].link >= 0) s.killed |= 1u << c.foes[j].link, s.hp[c.foes[j].link] = 0;
}

void apply(const Ctx& c, Sim& s, int j, float raw, float exact) {
    if (s.hp[j] <= 0) return;
    const Foe& f = c.foes[j];
    const float d = raw * f.taken;
    const float ab = std::min(s.sh[j], d);
    s.sh[j] -= ab;
    const float lost = exact >= 0 ? std::min(exact, s.hp[j]) : std::min(s.hp[j], d - ab);
    s.hp[j] -= lost;
    s.value += (lost + 0.5f * ab) * f.w;
    if (s.hp[j] <= 0.5f) { s.hp[j] = 0; kill_foe(c, s, j); }
}

void basic(const Ctx& c, Sim& s, int j) {
    apply(c, s, j, c.str, -1);
    s.retal += c.foes[j].retal * c.my_taken;
    if (c.splash)
        for (int k = 0; k < c.nf; ++k)
            if (k != j && !c.foes[k].hidden && cheb(c.foes[k].pos, c.foes[j].pos) <= 1) apply(c, s, k, c.str, -1);
}

// Best basic target from q (marginal value), -1 if none in range.
int best_basic(const Ctx& c, const Sim& s, vec2 q) {
    int b = -1;
    float bv = 0;
    [[maybe_unused]] int lo = -1; [[maybe_unused]] float lv = 0; // LOW_B: the lowest hp + shield foe in range, its value
    for (int j = 0; j < c.nf; ++j) {
        if (s.hp[j] <= 0 || c.foes[j].hidden || cheb(q, c.foes[j].pos) > c.arange) continue;
        Sim t = s;
        basic(c, t, j);
        const float v = t.value - t.retal - (s.value - s.retal);
        if (b < 0 || v > bv) b = j, bv = v;
        if constexpr (LOW_B > 0) if (lo < 0 || s.hp[j] + s.sh[j] < s.hp[lo] + s.sh[lo]) lo = j, lv = v;
    }
    if constexpr (LOW_B > 0) if (lo >= 0 && lo != b && lv + LOW_B * c.str > bv) b = lo; // one pass (c7lb's two cost fuel)
    return b;
}

void consider(const Ctx& c, Plan& best, Plan pl, const Sim& s, const Tile& t) {
    float raw = t.base, one = 0, chain = t.base;
    const bool stun_lock = (t.lock & ~(s.killed | s.removed)) != 0; // h-micro T1c: we would lose our next turn
    for (int j = 0; j < c.nf; ++j) {
        if (s.killed & (1u << j)) continue;
        const bool st = (s.removed >> j) & 1u;
        const float v = st ? t.con_s[j] : t.con[j];
        raw += v, one = std::max(one, v);
        if (stun_lock) chain += st ? t.ch_s[j] : t.ch[j];
    }
    raw *= t.mul, one *= t.mul, chain *= t.mul;
    if constexpr (HOR_PLAY) // h-heart: a turn that leaves the cast puts HoR's shield up (pvp.cpp cast_support); it absorbs first
        if (hor::shield_v > 0 && pl.dash.x < 0 && pl.spell < 0 && !pl.reset)
            raw = std::max(0.f, raw - hor::shield_v), one = std::max(0.f, one - hor::shield_v), chain = std::max(0.f, chain - hor::shield_v);
    // stunned on our tile we skip a turn: every foe gets one more before we act (top teams convert 28% of stuns to kills)
    const bool dead = raw >= c.my_ehp || (stun_lock && chain >= c.my_ehp);
    // sharing incoming damage with allies does not help against a foe that kills us alone: it will take the kill
    const float cost = raw * t.share + (one >= c.my_ehp ? c.death : dead ? c.death * t.share : 0.f) + s.retal;
    pl.value = s.value - s.retal;
    if constexpr (PV_DECLINE) if (!c.commit && s.killed == 0 && raw > 0) pl.value = std::min(pl.value, 0.f); // v22: no pokes
    if constexpr (CARRY) if (!c.commit && s.killed == 0 && carry_flag()) pl.value = std::min(pl.value, 0.f); // no kite pokes either
    if constexpr (OPEN_GATE) if (!c.commit && s.killed == 0 && open_pr() < 1e8f && (open_pr() < OPEN_PR || min_dist(pl.dest, c.allies) > 4)) pl.value = std::min(pl.value, 0.f);
    if constexpr (LOW_RUN) if (s.killed == 0 && low_run()) pl.value = std::min(pl.value, 0.f);
    pl.score = pl.value - c.lambda * cost + t.pos + (pl.value <= 0 ? t.next : 0.f);
    // h-micro R1: never end a turn where the foe turns before our next one can kill us, unless this turn kills.
    // Top teams never do (0% of contact turns); our v3 did on 2%, and those turns preceded 52% of its deaths.
    if (LETHAL_PEN > 0 && s.killed == 0 && (one >= c.my_ehp || dead)) pl.score -= LETHAL_PEN;
    else if (t.trap && s.killed == 0) pl.score -= TRAP_PEN;
    // HIT_FIRST: a hit on the way out beats the same walk without it (PV_DECLINE values both at 0: an exact tie)
    if constexpr (HIT_FIRST) if (pl.first) pl.score += 0.001f * std::max(0.f, s.value - s.retal);
    if (pl.score > best.score) best = pl;
}

} // namespace

// Every action set from pl.dest. mode 0: stayed, 1: walked, 2: dashed (cast spent).
void eval_tile(const Ctx& c, Plan& best, Plan pl, int mode, float pos_term) {
    auto broke = [&] { return static_cast<int>(bc::fuel_remaining()) < c.floor; };
    if (broke()) return; // keep fuel for the actions
    const vec2 q = pl.dest;
    Tile t;
    t.pos = pos_term;
    t.base = camp_danger(c, q);
    float now = t.base;
    for (int j = 0; j < c.nf; ++j) {
        const Foe& f = c.foes[j];
        if (!c.F.u[f.fi].alive) continue;
        const int d = cheb(q, f.pos);
        const float sr = soon_risk(c, j, q);
        t.con[j] = foe_dmg_k(f, d, false, f.turns) * c.my_taken + sr;
        t.con_s[j] = foe_dmg_k(f, d, true, f.turns) * c.my_taken + sr;
        now += t.con[j];
        if (!STUN_LOCK) continue;
        t.ch[j] = foe_dmg_k(f, d, false, f.turns + 1) * c.my_taken + sr;
        t.ch_s[j] = foe_dmg_k(f, d, true, f.turns + 1) * c.my_taken + sr;
        if (f.stun_r >= 0 && d <= f.stun_r && f.turns >= 1) t.lock |= 1u << j;
    }
    if (bc::map::spawn_region(team::ally).is_set(q)) t.mul = 0.35f; // full heal at the end of our turn
    if (TRAP_PEN > 0 && c.trap_on && t.mul == 1.f) t.trap = trapped(c, q, c.my_ehp - now);
    if (c.commit) {
        int near = 0;
        for (vec2 a : c.allies) near += cheb(a, q) <= 2;
        t.share = 1.f / (1.f + static_cast<float>(near));
        if (c.focus >= 0) {
            const Unit& f = c.F.u[c.focus];
            const int d = cheb(q, {f.x, f.y});
            if (d < fight::MAXD) t.next = NEXT * c.S.tab[c.mi].dmg[d].dmg * f.taken;
        }
    }
    Sim s0;
    for (int j = 0; j < c.nf; ++j) s0.hp[j] = c.foes[j].hp, s0.sh[j] = c.foes[j].shield;
    if constexpr (HOR_PLAY) // h-heart: HoR's landing hits and stuns every foe within 1; waking a sleeping camp costs
        if (mode == 2 && hor::strike > 0 && pl.dash.x >= 0) {
            // the engine refuses a dash through an enemy champion or a camp (error 32; pvp.cpp's ring checks walls only)
            if (!(pl.walk && pl.dest == pl.dash) && !hor::dash_path(c.p, pl.dash, hor_blk())) return;
            if constexpr (HOR_PRESHIELD == 2) { // c12h: no engage landing into 2+ foes (within 2) with no ally within 3 of it
                int n = 0; bool eng = false;
                for (int j = 0; j < c.nf; ++j) if (!c.foes[j].hidden && c.F.u[c.foes[j].fi].alive) { const int d = cheb(pl.dash, c.foes[j].pos); n += d <= 2, eng |= d <= 1; }
                if (eng && n >= 2 && min_dist(pl.dash, c.allies) > 3) return;
            }
            for (int j = 0; j < c.nf; ++j) {
                const Foe& f = c.foes[j];
                if (f.hidden || !c.F.u[f.fi].alive || cheb(pl.dash, f.pos) > 1) continue;
                if (!f.stunned) s0.value += f.stun_v, s0.removed |= 1u << j;
                apply(c, s0, j, hor::strike, f.sd[c.dash_slot] > 0 ? f.sd[c.dash_slot] : -1.f);
            }
            for (vec2 z : hor::sleepers()) if (cheb(pl.dash, z) <= 1) s0.value -= 150.f;
        }
    const bool atk = !(c.ranged && mode != 0), cast = mode != 2;
    { // basics only
        Sim s = s0;
        Plan a = pl;
        if (atk && (a.hit0 = best_basic(c, s, q)) >= 0) basic(c, s, a.hit0);
        consider(c, best, a, s, t);
    }
    if (cast && c.reset_slot >= 0) { // attack, reset, attack (ranged after a walk: reset, attack)
        Sim s = s0;
        Plan a = pl;
        a.reset = true;
        if (atk && (a.hit0 = best_basic(c, s, q)) >= 0) basic(c, s, a.hit0);
        if ((atk && a.hit0 >= 0) || (c.ranged && mode == 1))
            if ((a.hit1 = best_basic(c, s, q)) >= 0) {
                basic(c, s, a.hit1);
                consider(c, best, a, s, t);
            }
    }
    if constexpr (HIT_FIRST) if (atk && mode != 0 && !c.ranged) { // hit from the start tile, then dash / walk to q
        Sim s = s0;
        Plan a = pl;
        a.first = true;
        if ((a.hit0 = best_basic(c, s, c.p)) >= 0) {
            basic(c, s, a.hit0);
            consider(c, best, a, s, t);
            if (cast && c.reset_slot >= 0 && (a.hit1 = best_basic(c, s, c.p)) >= 0) { // attack, reset, attack, walk
                a.reset = true;
                basic(c, s, a.hit1);
                consider(c, best, a, s, t);
            }
        }
    }
    if (!cast) return;
    for (const auto& m : c.sp) {
        if (m.slot < 0 || !(m.dmg || m.cc || m.aoe) || (m.excl && mode != 0)) continue;
        auto after = [&](Sim& s, Plan& a) {
            if (atk && (a.hit0 = best_basic(c, s, q)) >= 0) basic(c, s, a.hit0);
            consider(c, best, a, s, t);
        };
        if (!m.aoe) {
            for (int j = 0; j < c.nf; ++j) {
                if (c.foes[j].hidden || cheb(q, c.foes[j].pos) > m.range || broke()) continue;
                Sim s = s0;
                Plan a = pl;
                a.spell = m.slot, a.sp_foe = j;
                if (m.stuns && !c.foes[j].stunned) s.value += c.foes[j].stun_v, s.removed |= 1u << j;
                if (m.raw > 0) apply(c, s, j, m.raw, c.foes[j].sd[m.slot]);
                after(s, a);
            }
            continue;
        }
        // AoE centres: each foe, the tile next to it towards us (reach range + radius), midpoints of pairs 2 apart
        std::array<vec2, 2 * MAXF + MAXF * MAXF / 2> cs;
        int n = 0;
        for (int j = 0; j < c.nf; ++j) {
            if (c.foes[j].hidden) continue;
            const vec2 f = c.foes[j].pos;
            cs[n++] = f;
            cs[n++] = {f.x + (q.x > f.x) - (q.x < f.x), f.y + (q.y > f.y) - (q.y < f.y)};
            for (int k = j + 1; k < c.nf; ++k)
                if (cheb(f, c.foes[k].pos) == 2 && n < static_cast<int>(cs.size())) cs[n++] = {(f.x + c.foes[k].pos.x) / 2, (f.y + c.foes[k].pos.y) / 2};
        }
        for (int i = 0; i < n; ++i) {
            const vec2 g = cs[i];
            if (!bc::map::is_in_bounds(g) || cheb(q, g) > m.range || broke()) continue;
            Sim s = s0;
            Plan a = pl;
            a.spell = m.slot, a.ground = g;
            bool any = false;
            for (int j = 0; j < c.nf; ++j) {
                if (c.foes[j].hidden || cheb(g, c.foes[j].pos) > m.radius) continue;
                any = true;
                if (m.stuns && !c.foes[j].stunned) s.value += c.foes[j].stun_v, s.removed |= 1u << j;
                if (m.raw > 0) apply(c, s, j, m.raw, c.foes[j].sd[m.slot]);
            }
            if (any) after(s, a);
        }
    }
}

bool free_tile(const Ctx& c, vec2 q) {
    return bc::map::is_in_bounds(q) && !W.static_walls.is_set(q) && !c.occ.is_set(q);
}

void execute(const Ctx& c, const Plan& pl) {
    auto& me = bc::self();
    const auto abs = me.abilities();
    auto foe = [&](int j) { return champ(c.foes[j].id); };
    try {
        bool late = true; // basics after the move (the default); HIT_FIRST plays them before it
        if constexpr (HIT_FIRST) if (pl.first) {
            late = false;
            if (pl.hit0 >= 0) { me.attack(foe(pl.hit0)); debug_block({ g_acts += 'a'; }); }
            if (pl.reset && pl.hit1 >= 0 && c.reset_slot >= 0) {
                abs[c.reset_slot].use();
                if (const auto t = foe(pl.hit1); t.is_alive()) { me.attack(t); debug_block({ g_acts += 'a'; }); }
            }
        }
        if constexpr (HOR_PLAY) // h-heart: walk first, then dash (pl.dest == pl.dash with a walk)
            if (pl.dash.x >= 0 && pl.walk && pl.dest == pl.dash) {
                const auto& camps = hor_blk();
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy) {
                        const vec2 w{c.p.x + dx, c.p.y + dy};
                        if (w == c.p || !free_tile(c, w) || cheb(w, pl.dash) > 2 || w == pl.dash || !hor::dash_path(w, pl.dash, camps)) continue;
                        if (me.position() == c.p) me.move_absolute(w);
                    }
                debug_block({ g_acts += 'm'; });
            }
        bool walked = false;
        if constexpr (DASH_CAMP) if (pl.dash.x >= 0 && c.dash_slot >= 0) {
            // p-fix F15: a failed dash (engine error 32) walks one step toward the plan's end tile and plays the rest
            // (after h-heart's walk-first above, the step is a no-op: HoR has moved this turn)
            bool ok = false;
            try { ok = abs[c.dash_slot].use(pl.dash); } catch (...) {}
            if (!ok) {
                walked = true;
                const vec2 to = pl.dest;
                best_step([&](vec2 q) -> float { return static_cast<float>(cheb(q, to)); });
                debug_block({ std::cout << "DASHFAIL R" << W.round << " i" << W.me_idx << " " << bc::to_string(pl.dash) << "\n"; });
            } else debug_block({ g_acts += 'D'; });
        }
        if (!DASH_CAMP && pl.dash.x >= 0 && c.dash_slot >= 0) { abs[c.dash_slot].use(pl.dash); debug_block({ g_acts += 'D'; }); }
        if (!walked && pl.walk && pl.dest != me.position()) { me.move_absolute(pl.dest); debug_block({ g_acts += 'm'; }); }
        if (pl.spell >= 0) {
            if (pl.ground.x >= 0) abs[pl.spell].use(pl.ground); else abs[pl.spell].use(foe(pl.sp_foe));
            debug_block({ g_acts += pl.ground.x >= 0 ? 'O' : 'C'; });
        }
        if (late && pl.hit0 >= 0) { me.attack(foe(pl.hit0)); debug_block({ g_acts += 'A'; }); }
        if (late && pl.reset && pl.hit1 >= 0 && c.reset_slot >= 0) {
            abs[c.reset_slot].use();
            debug_block({ g_acts += 'R'; });
            if (const auto t = foe(pl.hit1); t.is_alive()) { me.attack(t); debug_block({ g_acts += 'A'; }); }
        }
    } catch (const std::exception& e) {
        debug_block({ std::cout << "pvp exec error: " << e.what() << " dash=" << bc::to_string(pl.dash) << " dest=" << bc::to_string(pl.dest) << " walk=" << pl.walk;
            if (pl.dash.x >= 0) { const vec2 m{(c.p.x + pl.dash.x) / 2, (c.p.y + pl.dash.y) / 2}; std::cout << " mid=" << bc::to_string(m) << " blk=" << hor_blk().is_set(m) << " champ=" << bc::vision::champion_tiles().is_set(m)
                << " espawn=" << bc::map::spawn_region(team::enemy).is_set(pl.dash) << " hole=" << bc::map::holes().is_set(pl.dash) << " midhole=" << bc::map::holes().is_set(m) << " wall=" << bc::map::walls().is_set(m); }
            std::cout << "\n"; });
    }
}

void hor_setup(const Ctx& c) {
    hor::strike = 0, hor::shield_v = 0;
    hor_blk() = bc::vision::camp_tiles();
    for (int i = 0; i < c.F.n; ++i) if (!c.F.u[i].ally && c.F.u[i].alive && !c.F.u[i].hidden) hor_blk().set({c.F.u[i].x, c.F.u[i].y}); // (c.foes is filled later)
    hor::sleepers().clear();
    if (!hor::me()) return;
    auto& me = bc::self();
    const Unit& mu = c.F.u[c.mi];
    if (me.can_cast())
        for (const auto& sp : mu.sp) {
            if (!fight::ready(sp)) continue;
            if (sp.k == fight::K::strike) hor::strike = sp.mod * mu.foc; // foc carries the passive (fight_sdk.h)
            if (sp.k == fight::K::shield && !me.buffs().has_shield()) hor::shield_v = sp.mod * me.focus();
        }
    if (hor::strike > 0)
        for (const auto& m : W.camps) try { if (!m.is_awake() && m.strength() > 0) hor::sleepers().push_back(m.position()); } catch (...) {}
}

void hor_plans(const Ctx& c, Plan& best, const Plan& base, const std::function<float(vec2)>& pos) {
    if (hor::strike <= 0 || c.dash_slot < 0) return;
    const auto& camps = hor_blk();
    std::vector<vec2> tried;
    for (int j = 0; j < c.nf; ++j) {
        const Foe& f = c.foes[j];
        if (f.hidden || !c.F.u[f.fi].alive || cheb(c.p, f.pos) > 4) continue;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const vec2 q{f.pos.x + dx, f.pos.y + dy};
                const int d = cheb(c.p, q);
                if (d == 0 || d == 2 || d > 3 || std::find(tried.begin(), tried.end(), q) != tried.end()) continue;
                tried.push_back(q);
                // margin: the walk-first search (9 tiles x dash paths, ~50 a host bitboard test) runs before eval_tile's own check
                if (!free_tile(c, q) || static_cast<int>(bc::fuel_remaining()) < c.floor + 8000) continue;
                Plan pl = base;
                pl.dest = q, pl.dash = q;
                if (d == 1 && !hor::dash_path(c.p, q, camps)) continue;
                if (d == 3) {
                    bool ok = false;
                    for (int ex = -1; ex <= 1 && !ok; ++ex)
                        for (int ey = -1; ey <= 1 && !ok; ++ey) {
                            const vec2 w{c.p.x + ex, c.p.y + ey};
                            ok = w != c.p && free_tile(c, w) && cheb(w, q) <= 2 && hor::dash_path(w, q, camps);
                        }
                    if (!ok) continue;
                    pl.walk = true;
                }
                eval_tile(c, best, pl, 2, pos(q));
            }
    }
}

bool hor_danger(const Ctx& c) {
    const vec2 q = bc::self().position();
    float r = camp_danger(c, q);
    for (int j = 0; j < c.nf; ++j) r += contrib(c, j, q);
    return r > 0;
}

} // namespace pvp::plan
