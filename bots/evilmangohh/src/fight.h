#pragma once
// fight.h -- PvP fight evaluation for cambc2026a (design + numbers: notes/combat-design.md).
// Pure model: no SDK calls, fixed arrays, no heap. fight_sdk.h fills it from vision.
// Engine facts it encodes (verified in notes/mechanics.md and runs/combat):
//   basic hit = strength; spell = mod * focus; durability emblem = x0.9 taken; shields absorb first;
//   stun = victim skips its next own turn, does NOT stack; cooldown = 8 of the caster's own turn ends;
//   ranged: a walk spends the attack (a reset gives it back); movement-exclusive spells allow a basic, not a walk;
//   ground AoE (range 4, radius 1) reaches distance 5 and never hits allies; Varg splash = str to every enemy
//   within 1 of the target; Ren hits back 0.2 * focus per damage instance, at any range; Cassi+Issac die together.
// ponytail: open-ground Chebyshev geometry (no walls, bodies never block), greedy turns, shields never expire
// inside the horizon (<= 5 rounds). Upgrade path: BFS distances in dist(), shield timers.
#include <algorithm>
#include <array>
#include <cstdlib>
#include "heart.h"

namespace fight {

enum class K : unsigned char { none, reset, dash, nuke, stun_nuke, stun, aoe, aoe_stun, shield, heal, strike }; // strike: HoR dash (h-heart)
struct Spell { K k = K::none; float mod = 0; int range = 0, radius = 0; bool excl = false; int cd = 0; };

struct Unit {
    bool ally = false, alive = true, ranged = false, splash = false, acted = false, channel = false;
    float hp = 0, max_hp = 1, shield = 0, str = 0, foc = 0;
    float taken = 1.f;    // incoming damage multiplier: 0.9 with a durability emblem
    float retaliate = 0;  // Ren passive: 0.2 * focus back at each attacker
    std::array<Spell, 2> sp{};
    int x = 0, y = 0;
    int stun = 0;   // 1 = skips its next own turn (buff_stun.duration > 0)
    int link = -1;  // index of the linked body (Cassi <-> Issac)
    bool ghost = false; // f-small E1: linked body alive but not in this fight (unseen): dies with this one
    bool hidden = false; // h-micro G1: an enemy seen lately but not now (bush, edge of vision): it can hit, not be hit
    int prio = 0;   // in-team order key, higher acts first (prev_priority())
    int prio_next = 0; // h-micro T1: next round's key (priority())
    int gems = 0;
    unsigned id = 0; // engine handle (p-combat: maps units back to SDK champions)
};

constexpr int MAXU = 12, MAXD = 6;  // nothing in the roster reaches distance 6
constexpr float STUN_W = 80;        // worth of one denied turn when the victim is unknown

// One way to spend a turn. touch: melee walks max(0, d-1) first (dash if d > 2); step: ranged walks 1 first.
struct Opt {
    float dmg = 0;
    signed char slot = -1, hits = 0;
    bool stun : 1 = false, aoe : 1 = false, touch : 1 = false, step : 1 = false;
};
inline Opt opt(float dmg, int slot, int hits, bool stun = false, bool aoe = false, bool touch = false, bool step = false) {
    Opt o;
    o.dmg = dmg, o.slot = static_cast<signed char>(slot), o.hits = static_cast<signed char>(hits);
    o.stun = stun, o.aoe = aoe, o.touch = touch, o.step = step;
    return o;
}
// Best option at each distance 0..MAXD-1, by damage and the best one that also stuns.
struct Tab { std::array<Opt, MAXD> dmg{}, stun{}; };

struct Fight {
    std::array<Unit, MAXU> u{};
    int n = 0;
    bool ally_starts = true;        // our team opens the CURRENT round (odd rounds: team A)
    bool hold[2] = {false, false};  // [enemy, ally]: never walk, only hit what is already in reach
};
struct Outcome { float score = 0, dmg_to_enemy = 0, dmg_to_ally = 0; int dead_enemy = 0, dead_ally = 0, first_kill_round = -1; };

// h-micro T1: turns a foe takes between our turn now and our next one. Turns alternate one champion at a time, the
// opening team swaps every round; lead: our team opens this round; ranks: next round's slots in each team's order.
inline int turns_before_next(bool acted, bool lead, int rank_foe, int rank_me) {
    return (acted ? 0 : 1) + ((lead ? rank_foe <= rank_me : rank_foe < rank_me) ? 1 : 0);
}

inline int dist(const Unit& a, const Unit& b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }
inline bool ready(const Spell& s) { return s.k != K::none && s.cd <= 0; }
inline int find(const Unit& u, K k) {
    for (int i = 0; i < 2; ++i) if (u.sp[i].k == k && ready(u.sp[i])) return i;
    return -1;
}
inline float ehp(const Unit& u) { return (u.hp + u.shield) / u.taken; }
inline void tick(Unit& a) { for (auto& s : a.sp) if (s.cd > 0) --s.cd; }

inline void offer(Tab& t, int lo, int hi, const Opt& o) {
    for (int d = std::max(lo, 0); d <= std::min(hi, MAXD - 1); ++d) {
        if (o.dmg > t.dmg[d].dmg) t.dmg[d] = o;
        if (o.stun && (!t.stun[d].stun || o.dmg > t.stun[d].dmg)) t.stun[d] = o;
    }
}

// Every way `a` can spend this turn, by distance. can_walk=false: options that stay put; no_cast: cast already used.
inline Tab tabulate(const Unit& a, bool can_walk = true, bool no_cast = false) {
    Tab t;
    const float s = a.str;
    const int R = a.ranged ? 3 : 1, touch = can_walk ? 2 : 1;
    if (a.ranged) offer(t, 0, 3, opt(s, -1, 1));
    else offer(t, 0, touch, opt(s, -1, 1, false, false, true));
    if (no_cast) return t;
    if (const int r = find(a, K::reset); r >= 0) {
        if (!a.ranged) offer(t, 0, touch, opt(2 * s, r, 2, false, false, true));
        else {
            offer(t, 0, 3, opt(2 * s, r, 2));
            if (can_walk) offer(t, 4, 4, opt(s, r, 1, false, false, false, true));  // walk, reset, attack
        }
    }
    if (const int ds = find(a, K::dash); ds >= 0 && !a.ranged && can_walk) offer(t, 3, 4, opt(s, ds, 1, false, false, true));
    for (int i = 0; i < 2; ++i) {
        const Spell& sp = a.sp[i];
        if (!ready(sp)) continue;
        const float v = sp.mod * a.foc;
        const bool st = sp.k == K::stun_nuke || sp.k == K::aoe_stun;
        switch (sp.k) {
        case K::nuke: case K::stun_nuke:  // movement-exclusive: a basic is allowed, a walk is not
            offer(t, 0, std::min(R, sp.range), opt(v + s, i, 1, st));
            offer(t, R + 1, sp.range, opt(v, i, 0, st));
            break;
        case K::aoe: case K::aoe_stun:  // ground target next to the victim: reach range + radius
            offer(t, 0, R, opt(v + s, i, 1, st, true));
            offer(t, R + 1, sp.range + sp.radius, opt(v, i, 0, st, true));
            break;
        case K::stun:  // Cassi s1: no damage, walk-compatible
            if (a.ranged) offer(t, 0, 3, opt(s, i, 1, true));
            else offer(t, 0, touch, opt(s, i, 1, true, false, true));
            offer(t, a.ranged ? 4 : touch + 1, sp.range, opt(0, i, 0, true));
            if (can_walk && !sp.excl) offer(t, sp.range + 1, sp.range + 1, opt(0, i, 0, true, false, false, true));
            break;
        case K::strike:  // h-heart HOR_MODEL: walk 1 + dash, landing next to the victim: damage and stun within the radius
            if constexpr (HOR_MODEL) if (!a.ranged && can_walk) offer(t, 0, sp.range + sp.radius + 1, opt(v + s, i, 1, true, true, true));
            break;
        default: break;
        }
    }
    return t;
}

inline float value(const Opt& o, const Unit& t, float stun_val) { return o.dmg * t.taken + (o.stun && t.stun == 0 ? stun_val : 0); }
inline Opt pick(const Tab& tb, const Unit& t, int d, float stun_val) {
    if (d >= MAXD) return {};
    const Opt& a = tb.dmg[d];
    const Opt& b = tb.stun[d];
    return b.stun && value(b, t, stun_val) > value(a, t, stun_val) ? b : a;
}
// Largest distance at which u can damage or stun someone this turn.
inline int reach(const Tab& tb) {
    for (int d = MAXD - 1; d > 0; --d) if (tb.dmg[d].dmg > 0 || tb.stun[d].stun) return d;
    return 0;
}
// Expected damage per turn over the next ~3 turns (+ a third of STUN_W if a stun is up).
inline float threat(const Unit& u, const Tab& tb) {
    const int d = u.ranged ? 3 : 1;
    return (tb.dmg[d].dmg + 2 * u.str * (u.splash ? 1.5f : 1.f)) / 3 + (tb.stun[d].stun || tb.stun[4].stun ? STUN_W / 3 : 0);
}

namespace detail {
inline void kill(Fight& f, int i) {
    Unit& t = f.u[i];
    if (!t.alive) return;
    t.alive = false;
    t.hp = 0;
    if (t.link >= 0) kill(f, t.link);
}
inline void hit(Fight& f, int ai, int ti, float raw, Outcome& o) {
    Unit& t = f.u[ti];
    Unit& a = f.u[ai];
    if (!t.alive || raw <= 0) return;
    float d = raw * t.taken;
    const float s = std::min(t.shield, d);
    t.shield -= s;
    (t.ally ? o.dmg_to_ally : o.dmg_to_enemy) += std::min(d, t.hp + s);
    t.hp -= d - s;
    if (t.retaliate > 0 && a.alive) {
        a.hp -= t.retaliate * a.taken;
        if (a.hp <= 0) kill(f, ai);
    }
    if (t.hp <= 0) kill(f, ti);
}
}  // namespace detail

// End of a's turn: tick cooldowns; rebuild its option table only if a spell was spent or came back.
inline void retick(Unit& a, Tab& tab, bool walk, bool spent) {
    bool back = false;
    for (auto& s : a.sp) if (s.cd > 0 && --s.cd == 0) back = true;
    if (spent || back) tab = tabulate(a, walk);
}

struct State {  // per-simulation caches
    std::array<Tab, MAXU> tab{};
    std::array<float, MAXU> thr{};
};

// Option tables and threats for every unit (what simulate() starts from).
inline State prepare(const Fight& f) {
    State st;
    for (int i = 0; i < f.n; ++i) {
        st.tab[i] = tabulate(f.u[i], !f.hold[f.u[i].ally]);
        st.thr[i] = f.u[i].alive ? threat(f.u[i], st.tab[i]) : 0;
    }
    return st;
}

// Damage the other side can put on one of ours standing at (x, y) before our next turn
// (each enemy once, its best option at that distance, walking included). Positioning: keep it < hp + shield.
inline float danger(const Fight& f, const State& st, bool ally_side, int x, int y) {
    float s = 0;
    for (int j = 0; j < f.n; ++j) {
        const Unit& e = f.u[j];
        if (!e.alive || e.ally == ally_side || e.stun) continue;
        const int d = std::max(std::abs(e.x - x), std::abs(e.y - y));
        if (d < MAXD) s += st.tab[j].dmg[d].dmg;
    }
    return s;
}

// Focus target for one side, Smith's rule: threat removed per round of killing time,
// score = weight * (burst on it this round) / ehp, x2 if that burst kills it now.
// Killing either linked body removes both, so a linked target also carries its partner's threat.
inline int choose(const Fight& f, const State& st, bool ally_side) {
    int best = -1;
    float bs = -1;
    for (int i = 0; i < f.n; ++i) {
        const Unit& t = f.u[i];
        if (!t.alive || t.ally == ally_side || t.hidden) continue;
        const float w = st.thr[i] + (t.link >= 0 && f.u[t.link].alive ? st.thr[t.link] : t.ghost ? st.thr[i] : 0) + 2.f * t.gems + (t.channel ? 60.f : 0);
        float burst = 0;
        for (int j = 0; j < f.n; ++j) {
            const Unit& a = f.u[j];
            const int d = dist(a, t);
            if (a.alive && a.ally == ally_side && !a.stun && d < MAXD) burst += st.tab[j].dmg[d].dmg * t.taken;
        }
        float s = w * (burst + 10.f) / std::max(50.f, ehp(t));
        if (burst >= ehp(t)) s *= 2;
        if (s > bs) bs = s, best = i;
    }
    return best;
}

inline void step_towards(Unit& a, const Unit& t, int steps) {
    for (int k = 0; k < steps && dist(a, t) > 1; ++k) {
        a.x += (t.x > a.x) - (t.x < a.x);
        a.y += (t.y > a.y) - (t.y < a.y);
    }
}

// One turn of unit i. focus = its side's focus, enemy_focus = the other side's.
inline void act(Fight& f, State& st, int i, int focus, int enemy_focus, Outcome& o) {
    Unit& a = f.u[i];
    const bool walk = !f.hold[a.ally];
    if (!a.alive) return;
    if (a.stun) { a.stun = 0; retick(a, st.tab[i], walk, false); return; }
    bool cast = false;
    if (const int s = find(a, K::shield); s >= 0 && (i == enemy_focus || a.hp < 0.6f * a.max_hp)) {
        bool near = false;
        for (int j = 0; j < f.n; ++j) near |= f.u[j].alive && f.u[j].ally != a.ally && dist(a, f.u[j]) <= reach(st.tab[j]);
        if (near) a.shield += a.sp[s].mod * a.foc, a.sp[s].cd = 8, cast = true;
    }
    if (const int s = find(a, K::heal); s >= 0 && !cast) {
        int bj = -1;
        float miss = 0.7f * a.sp[s].mod * a.foc;
        for (int j = 0; j < f.n; ++j) {
            const Unit& b = f.u[j];
            if (j != i && b.alive && b.ally == a.ally && dist(a, b) <= a.sp[s].range && b.max_hp - b.hp >= miss) miss = b.max_hp - b.hp, bj = j;
        }
        if (bj >= 0) f.u[bj].hp = std::min(f.u[bj].max_hp, f.u[bj].hp + a.sp[s].mod * a.foc), a.sp[s].cd = 8, cast = true;
    }
    Tab spent;
    if (cast) spent = tabulate(a, walk, true);
    const Tab& tb = cast ? spent : st.tab[i];
    int ti = -1;
    Opt p;
    bool dashed = false;
    if (focus >= 0 && f.u[focus].alive) ti = focus, p = pick(tb, f.u[ti], dist(a, f.u[ti]), st.thr[ti]);
    // Focus out of reach: hit something else only if that kills it, or if the focus is more than
    // one step beyond our reach (walking would not let us hit it next turn either).
    if (ti < 0 || value(p, f.u[ti], st.thr[ti]) <= 0) {
        const bool far = ti < 0 || dist(a, f.u[ti]) > reach(tb) + 1;
        float bs = 0;
        ti = -1;
        for (int j = 0; j < f.n; ++j) {
            const Unit& t = f.u[j];
            if (!t.alive || t.ally == a.ally) continue;
            const Opt q = pick(tb, t, dist(a, t), st.thr[j]);
            const float v = value(q, t, st.thr[j]) / std::max(50.f, ehp(t));
            if ((far || q.dmg * t.taken >= t.hp + t.shield) && v > bs) bs = v, ti = j, p = q;
        }
    }
    if (ti < 0) {  // nothing worth hitting: close in on the focus, melee dashes when far
        if (focus >= 0 && f.u[focus].alive && walk) {
            int steps = 1;
            if (const int s = find(a, K::dash); s >= 0 && !cast && !a.ranged && dist(a, f.u[focus]) > 4) steps = 3, a.sp[s].cd = 8, dashed = true;
            step_towards(a, f.u[focus], steps);
        }
    } else {
        Unit& t = f.u[ti];
        step_towards(a, t, p.touch ? dist(a, t) - 1 : p.step ? 1 : 0);
        if (p.slot >= 0) a.sp[p.slot].cd = 8;
        const float spell = p.dmg - p.hits * a.str;
        if (p.aoe) {
            const int cx = t.x, cy = t.y;
            for (int j = 0; j < f.n; ++j) {
                Unit& e = f.u[j];
                if (!e.alive || e.ally == a.ally || std::max(std::abs(e.x - cx), std::abs(e.y - cy)) > 1) continue;
                if (p.stun) e.stun = 1;
                detail::hit(f, i, j, spell, o);
            }
        } else {
            if (p.stun) t.stun = 1;
            detail::hit(f, i, ti, spell, o);
        }
        for (int h = 0; h < p.hits && a.alive; ++h) {
            const int tx = t.x, ty = t.y;
            detail::hit(f, i, ti, a.str, o);
            if (a.splash)
                for (int j = 0; j < f.n; ++j)
                    if (j != ti && f.u[j].alive && f.u[j].ally != a.ally && std::max(std::abs(f.u[j].x - tx), std::abs(f.u[j].y - ty)) <= 1)
                        detail::hit(f, i, j, a.str, o);
        }
    }
    retick(a, st.tab[i], walk, cast || p.slot >= 0 || dashed);
}

// Both sides focus-fire for `rounds` rounds from now; units with acted=true skip round 0.
// score = enemy bodies killed - ours (each 1 + gems/50) + 0.3 * (enemy hp fraction lost - ours); > 0 favours us.
inline Outcome simulate(Fight f, int rounds, const State* pre = nullptr) {  // pre: prepare(f) already done
    Outcome o;
    State st = pre ? *pre : prepare(f);
    std::array<float, MAXU> hp0{};
    std::array<int, MAXU> A{}, B{};
    int na = 0, nb = 0;
    for (int i = 0; i < f.n; ++i) {
        hp0[i] = f.u[i].hp;
        (f.u[i].ally ? A[na++] : B[nb++]) = i;
    }
    auto by_prio = [&](int x, int y) { return f.u[x].prio != f.u[y].prio ? f.u[x].prio > f.u[y].prio : x < y; };
    std::sort(A.begin(), A.begin() + na, by_prio);
    std::sort(B.begin(), B.begin() + nb, by_prio);
    bool ally_first = f.ally_starts;
    for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < f.n; ++i) st.thr[i] = f.u[i].alive ? threat(f.u[i], st.tab[i]) : 0;
        int fa = choose(f, st, true), fe = choose(f, st, false);
        int ia = 0, ib = 0;
        for (bool ally_turn = ally_first; ia < na || ib < nb; ally_turn = !ally_turn) {
            const bool mine = (ally_turn && ia < na) || ib >= nb;
            const int i = mine ? A[ia++] : B[ib++];
            if (r == 0 && f.u[i].acted) continue;
            int& foc = mine ? fa : fe;
            if (foc < 0 || !f.u[foc].alive) foc = choose(f, st, mine);
            act(f, st, i, foc, mine ? fe : fa, o);
        }
        if (o.first_kill_round < 0)
            for (int i = 0; i < f.n; ++i) if (!f.u[i].alive) { o.first_kill_round = r + 1; break; }
        ally_first = !ally_first;
    }
    for (int i = 0; i < f.n; ++i) {
        const Unit& u = f.u[i];
        const float v = u.alive ? 0.3f * (hp0[i] - u.hp) / u.max_hp : (u.ghost ? 2.f : 1.f) + u.gems / 50.f;
        if (!u.alive) ++(u.ally ? o.dead_ally : o.dead_enemy);
        o.score += u.ally ? -v : v;
    }
    return o;
}

// Rounds for side `side` (all its units committed, target passive) to kill unit ti; -1 if not within cap.
inline int rounds_to_kill(Fight f, bool side, int ti, int cap = 8) {
    for (int i = 0; i < f.n; ++i) if (f.u[i].ally != side && i != ti && f.u[i].link != ti) f.u[i].alive = false;
    State st = prepare(f);
    Outcome o;
    for (int r = 1; r <= cap; ++r) {
        for (int i = 0; i < f.n; ++i) if (f.u[i].ally == side) act(f, st, i, ti, -1, o);
        if (!f.u[ti].alive) return r;
        for (int i = 0; i < f.n; ++i) if (f.u[i].ally != side && f.u[i].alive) tick(f.u[i]);
    }
    return -1;
}

}  // namespace fight
