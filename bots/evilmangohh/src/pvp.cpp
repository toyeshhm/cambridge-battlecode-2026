// pvp.cpp -- champion-vs-champion turns. 1. Verdict: fight::simulate(F, 3), commit above COMMIT, stay above STAY once in
// contact; no commit below 40% hp, gem carriers need +1.0. 2. Team focus: fight::choose shared through 3 spare shared-memory
// bits. 3. Planner: every reachable end tile x every action set, scored as value dealt - risk x damage the enemy can land
// there before our next turn + position terms (pvp_plan.cpp; combat-design.md, CHANGES.md).
#include "pvp.h"
#include "actions.h"
#include "zreach.h"
#include "fight_sdk.h"
#include "pvp_plan.h"
#include "prof.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

namespace pvp {

bc::bitboard avoid;
bool avoid_on = false;
int last_seen = -100, last_contact = -100;
int hold_gi = -1, hold_round = -100;
int pend_gi = -1;

namespace {
using namespace plan;

constexpr float COMMIT = 0.5f, STAY = -0.1f; // simulate(3) score thresholds
constexpr float APPROACH = 5.f;              // committed: per tile beyond our reach from the focus
constexpr float HOME = 8.f, HOME_HURT = 20.f; // declined, under threat: per step towards spawn (hurt: < 60% hp or carrier)
constexpr float COHESION = 4.f;              // committed: per tile beyond 3 from the nearest visible ally
constexpr bool GHOST_GATE = true;            // h-micro G1b: a ghost within 4 starts a pvp turn (avoid mask, no targets)
constexpr float R5W = 0.5f;                  // h-micro R5: extra value weight on damage to a stunned foe
constexpr bool OPEN_FLOOR = true;            // v6: foe turn floor of 1 for openings in follow rounds (micro-ab.md 6.2a)
// k-hold H1 (micro-ab.md 3): winning a big reward = power ratio within HOLD_D >= HOLD_R with trade >= 0, or >= HOLD_R2 with > 0.
constexpr int HOLD_D = 8;
constexpr float HOLD_R = 1.f, HOLD_R2 = 0.7f;
constexpr float HOLD_ON = 200.f, HOLD_STEP = 25.f; // end tile on the group's tiles / per tile away from them
bool pv_run = false; int quiet = 0, standoff = 0; // pv_run: PV_RUN this turn; standoff: committed turns in a row without damage: stop chasing after a few
float last_score = 0;
int last_score_round = -100, last_score_n = 0;

unsigned hid(const bc::champion& c) { return static_cast<unsigned>(static_cast<bc::handle_champion_t>(c)); }

int enemy_slot(unsigned id) {
    static const auto es = bc::champions(team::enemy);
    for (int i = 0; i < static_cast<int>(es.size()); ++i) if (hid(es[i]) == id) return i;
    return -1;
}

// Team focus: enemy draft slot + 1 in byte 6 bit 6 (bit 0) and byte 7 bits 6-7 (bits 1-2); 0 = none.
int read_focus() {
    const unsigned b6 = W.sm[6], b7 = W.sm[7];
    return static_cast<int>(((b6 >> 6) & 1u) | ((b7 >> 5) & 6u)) - 1;
}
void write_focus(int slot) {
    const unsigned v = static_cast<unsigned>(slot + 1) & 7u;
    const unsigned b6 = W.sm[6], b7 = W.sm[7];
    W.write_sm(6, static_cast<std::uint8_t>((b6 & ~0x40u) | ((v & 1u) << 6)));
    W.write_sm(7, static_cast<std::uint8_t>((b7 & 0x3fu) | ((v & 6u) << 5)));
}

float end_hp = 0;
int end_round = -100;

} // namespace

int combat_priority() {
    static int base = -1;
    if (base < 0) {
        using CT = bc::champion_type;
        // stunners and burst casters act first (combat-design.md section 6): a stun or kill denies the next enemy turn
        switch (bc::self().champion_name()) {
        case CT::ren: base = 100; break;
        case CT::cassi: base = 90; break;
        case CT::stellar: case CT::tavi: base = 80; break;
        case CT::prism: base = 75; break;
        case CT::ryth: base = 70; break;
        case CT::issac: base = 50; break;
        case CT::varg: base = 40; break;
        default: base = 30; break;
        }
        if constexpr (HOR_PLAY) if (hor::me()) base = 85; // h-heart: HoR's landing stuns (after Ren and Cassi)
    }
    return base;
}

void note_end_hp() {
    try { end_hp = bc::self().health(); end_round = static_cast<int>(bc::round_number()); } catch (...) {}
}

void reveal() {
    auto& me = bc::self();
    if (end_round != W.round - 1 || end_hp - me.health() < 20 || me.ward_cooldown() > 0) return;
    const vec2 p = me.position();
    for (const auto& m : W.camps)
        try { if (m.is_awake() && m.strength() > 0 && cheb(p, m.position()) <= (REACH_ALL ? reach(m.kind()) : 3)) return; } catch (...) {} // p-fix F5
    for (const auto& e : bc::vision::champions(team::enemy))
        try { if (e.is_visible() && e.is_alive() && cheb(p, e.position()) <= 5) return; } catch (...) {}
    const auto vis = bc::vision::visible_tiles();
    const auto& bush = bc::map::bushes();
    vec2 best{-1, -1};
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy) {
            const vec2 q{p.x + dx, p.y + dy};
            if (!bc::map::is_in_bounds(q) || !bush.is_set(q) || vis.is_set(q)) continue;
            if (best.x < 0 || cheb(p, q) < cheb(p, best)) best = q;
        }
    if (best.x < 0) return;
    try { if (me.place_ward(best)) { debug_block({ g_acts += 'W'; }); } } catch (...) {}
}

bool turn(const std::vector<bc::champion>& enemies) {
    auto& me = bc::self();
    // ponytail: one Ctx reused across turns (zeroing its tables cost ~7k a turn); every field read is reset or assigned below
    static Ctx c;
    c.mi = -1, c.nf = 0, c.focus = -1, c.reset_slot = -1, c.dash_slot = -1, c.trap_on = false;
    c.sp = {};
    c.camps.clear(), c.allies.clear();
    c.p = me.position();
    int nearest = 1 << 20;
    unsigned prey = 0; // hunt: an enemy channelling a tribute/recall that we can reach before it finishes
    bool hunting = false;
    [[maybe_unused]] unsigned fprey = 0; [[maybe_unused]] float fhp = 2.f; // FINISH: the lowest-hp low foe
    for (const auto& e : enemies) {
        const int d = cheb(c.p, e.position());
        nearest = std::min(nearest, d);
        try {
            if (d <= 10 && is_channeling(e) && e.gem_count() >= 15 && d - 4 <= static_cast<int>(e.remaining_channel_time()))
                prey = hid(e), hunting = true;
            if constexpr (FINISH) {
                const float h = hp_frac(e);
                const bool rec = h < 0.35f && d <= 10 && e.is_recalling() && !e.is_submitting_gems() && d - 4 <= static_cast<int>(e.remaining_channel_time());
                if ((rec || (h < 0.3f && d <= 4)) && h < fhp && !bc::map::spawn_region(team::enemy).is_set(e.position())) fprey = hid(e), fhp = h;
            }
        } catch (...) {}
    }
    hunting = hunting && me.gem_count() < 30 && hp_frac(me) >= 0.5f;
    [[maybe_unused]] bool fin = false;
    if constexpr (FINISH) if (!hunting && fprey && me.gem_count() < 30 && hp_frac(me) >= 0.4f) prey = fprey, hunting = fin = true;
    // G1: an enemy that just vanished next to us (a bush) still counts
    if (nearest > 7 && !hunting && !(GHOST_GATE && fight::GHOST_AGE > 0 && fight::ghost_near(c.p.x, c.p.y, 4))) { standoff = 0; return false; }
    last_seen = W.round; if constexpr (HOR_PRESHIELD > 0) if (hor::me() && (HOR_PRESHIELD == 3 ? nearest >= 5 : nearest <= 4) && !hunting) hor::preshield(); // c12h
    PROF("p.gate");
    [[maybe_unused]] const auto fuel0 = bc::fuel_remaining();

    fight::from_vision(c.F);
    const unsigned my_id = hid(me);
    for (int i = 0; i < c.F.n; ++i) if (c.F.u[i].ally && c.F.u[i].id == my_id) c.mi = i;
    if (c.mi < 0) return false;
    c.S = fight::prepare(c.F);
    const Unit& mu = c.F.u[c.mi];
    PROF("p.vision");
    [[maybe_unused]] const auto fuel1 = bc::fuel_remaining();

    // my kit (from the model: from_vision read cooldowns and spell metadata)
    const auto abs = me.abilities();
    c.ranged = mu.ranged;
    c.arange = static_cast<int>(me.attack_range());
    c.str = mu.str;
    c.splash = mu.splash;
    c.my_taken = mu.taken;
    c.my_ehp = mu.hp + mu.shield;
    const bool cast_ok = me.can_cast();
    for (int s = 0; s < 2 && cast_ok; ++s) {
        const auto& sp = mu.sp[s];
        if (!fight::ready(sp)) continue;
        MySpell m;
        m.slot = s;
        m.excl = sp.excl;
        m.range = sp.range, m.radius = sp.radius;
        m.dmg = sp.k == fight::K::nuke;
        m.cc = sp.k == fight::K::stun_nuke || sp.k == fight::K::stun;
        m.aoe = sp.k == fight::K::aoe || sp.k == fight::K::aoe_stun;
        m.stuns = m.cc || sp.k == fight::K::aoe_stun;
        m.raw = sp.mod * mu.foc;
        if (sp.k == fight::K::reset) c.reset_slot = s;
        if (sp.k == fight::K::dash || (HOR_PLAY && sp.k == fight::K::strike)) c.dash_slot = s;
        c.sp[s] = m;
    }
    if constexpr (HOR_PLAY) hor_setup(c); // h-heart
    if (!me.can_move() && !me.can_attack() && !cast_ok) return true;
    // v7: hunt a hurt carrier (plan::hunt_carrier); allies converge on the shared focus
    const int carrier = V8_HUNT && !hunting && me.gem_count() < 30 && hp_frac(me) >= 0.5f ? hunt_carrier(c) : -1;
    hunting = hunting || carrier >= 0;

    // focus: the shared one if we see it near, else ours (and publish it)
    const int reach_me = fight::reach(c.S.tab[c.mi]);
    if (const int sf = read_focus(); sf >= 0)
        for (int i = 0; i < c.F.n; ++i)
            if (!c.F.u[i].ally && c.F.u[i].alive && !c.F.u[i].hidden && enemy_slot(c.F.u[i].id) == sf && fight::dist(c.F.u[i], mu) <= reach_me + 2) c.focus = i;
    if (hunting)
        for (int i = 0; i < c.F.n; ++i) if (!c.F.u[i].ally && c.F.u[i].id == prey) c.focus = i;
    if constexpr (FINISH) if (fin && c.focus >= 0) write_focus(enemy_slot(c.F.u[c.focus].id)); // allies converge
    if (carrier >= 0) write_focus(enemy_slot(c.F.u[c.focus = carrier].id));
    if (c.focus < 0 && (c.focus = fight::choose(c.F, c.S, true)) >= 0) write_focus(enemy_slot(c.F.u[c.focus].id));

    // foes
    for (int i = 0; i < c.F.n && c.nf < MAXF; ++i) {
        const Unit& u = c.F.u[i];
        if (u.ally || !u.alive) continue;
        Foe& f = c.foes[c.nf++];
        f.fi = i, f.id = u.id, f.pos = {u.x, u.y}, f.link = -1, f.sd = {}, f.soon = {}; // static Ctx: reset every field
        f.hp = u.hp, f.shield = u.shield, f.taken = u.taken, f.retal = u.retaliate;
        f.stunned = u.stun > 0, f.hidden = u.hidden;
        const float thr = c.S.thr[i];
        const float link_v = u.link >= 0 && c.F.u[u.link].alive ? 250.f + 2.f * c.S.thr[u.link] : u.ghost ? 250.f + 2.f * thr : 0.f;
        f.kill_v = 250.f + 2.f * thr + 3.f * u.gems + link_v;
        f.stun_v = f.stunned ? 0.f : 0.5f * thr + (u.channel ? 150.f + 3.f * u.gems : 0.f);
        // h-micro R5: a stunned foe can neither answer nor leave (top teams hit the stunned one 81% of the time, we 52%)
        f.w = 1.f + (i == c.focus ? 0.5f : 0.f) + (u.channel ? 0.3f : 0.f) + (f.stunned ? R5W : 0.f);
        if constexpr (FINISH) { f.w += 0.6f * (1.f - u.hp / std::max(1.f, u.max_hp)); if (u.hp < 0.3f * u.max_hp) f.kill_v += 200.f; }
        if (bc::map::spawn_region(team::enemy).is_set(f.pos)) f.w = 0.1f; // heals fully: only kills count
        { // f-small R1: spells back by its turn after next (cd 1): stepping in now traps us inside their range
            Unit soon = u;
            bool any = false;
            for (auto& sp : soon.sp) if (sp.k != fight::K::none && sp.cd == 1) sp.cd = 0, any = true;
            if (any && !f.stunned) {
                const auto t2 = fight::tabulate(soon, true);
                for (int d = 0; d < fight::MAXD; ++d) f.soon[d] = std::max(0.f, t2.dmg[d].dmg - c.S.tab[i].dmg[d].dmg) * c.my_taken;
            }
        }
        const int d = cheb(c.p, f.pos);
        for (int s = 0; s < 2; ++s) {
            const auto& m = c.sp[s];
            if (m.slot < 0 || m.raw <= 0 || d > m.range + m.radius || f.hidden) continue;
            f.sd[s] = m.raw * f.taken;
            // exact (shields, items) when it can decide a kill
            if (f.sd[s] + 2 * c.str >= f.hp + f.shield) try { f.sd[s] = abs[s].damage_amount(champ(u.id)); } catch (...) {}
        }
    }
    for (int j = 0; j < c.nf; ++j)
        for (int k = 0; k < c.nf; ++k)
            if (c.F.u[c.foes[j].fi].link == c.foes[k].fi) c.foes[j].link = k;
    { // h-micro T1: turns alternate one champion at a time and the opening team swaps every round. Foe j acts again
      // this round if it has not acted yet, and next round before me if its slot there is <= mine (they open) or < mine (we open).
        const int mr = fight::next_rank(my_id, combat_priority());
        const bool lead = c.F.ally_starts;
        const bool opening_contact = W.round - last_contact <= 2;
        for (int j = 0; j < c.nf; ++j) {
            Foe& f = c.foes[j];
            const Unit& u = c.F.u[f.fi];
            const int re = fight::next_rank(u.id, u.prio_next);
            f.turns = static_cast<signed char>(fight::turns_before_next(u.acted, lead, re, mr));
            // v6 (micro-ab.md 6.2a): out of contact in a round they open, every visible foe counts at least once (v3's floor)
            if (OPEN_FLOOR && !lead && !opening_contact && !f.hidden && f.turns < 1) f.turns = 1;
            const bool two = f.turns == 2;
            const auto& t1 = c.S.tab[f.fi];
            f.stun_r = -1;
            if (!f.stunned) for (int d = 0; d < fight::MAXD; ++d) if (t1.stun[d].stun) f.stun_r = d;
            f.one = {}, f.two = {}, f.st2 = {}, f.nxt = {};
            for (int d = 0; d < fight::MAXD; ++d) f.one[d] = t1.dmg[d].dmg;
            // h-micro T2: the window after our next turn: the foe in our slot acts twice when we open next round, else once.
            const int nk = !lead && re == mr ? 2 : 1;
            const bool idle = f.turns == 0;
            if (TRAP_PEN > 0 && !lead) {
                if (idle) {
                    if (nk == 2) c.trap_on = true;
                    for (int d = 0; d < fight::MAXD; ++d) f.nxt[d] = f.one[d];
                } else {
                    const auto tb = fight::tabulate(u, true, true);
                    for (int d = 0; d < fight::MAXD; ++d) f.nxt[d] = nk * tb.dmg[d].dmg;
                }
            }
            if (!two && !(TRAP_PEN > 0 && !lead && idle && nk == 2)) continue;
            // second turn: cooldowns one tick lower, minus the slot its first turn spent; from where the first left it
            std::array<fight::Tab, 3> t2;
            std::array<bool, 3> have{};
            auto second = [&](int slot) -> const fight::Tab& {
                if (!have[slot + 1]) {
                    Unit v = u;
                    fight::tick(v);
                    if (slot >= 0) v.sp[slot].cd = 7;
                    t2[slot + 1] = fight::tabulate(v, true);
                    have[slot + 1] = true;
                }
                return t2[slot + 1];
            };
            for (int d = 1; d < fight::MAXD; ++d) {
                const fight::Opt& o = t1.dmg[d];
                const int d2 = o.touch ? 1 : (o.step || o.dmg <= 0) ? std::max(1, d - 1) : d;
                f.two[d] = second(o.dmg > 0 ? o.slot : -1).dmg[d2].dmg;
                f.st2[d] = second(-1).dmg[d].dmg;
            }
            // from 6 it walks to 5 on its first turn (fragment R1017-18: Stellar 6 -> 5, then its AoE killed two of ours)
            f.two[fight::MAXD] = second(-1).dmg[fight::MAXD - 1].dmg;
            if (TRAP_PEN > 0 && !lead && idle && nk == 2)
                for (int d = 0; d <= fight::MAXD; ++d) f.nxt[d] = f.one[d] + f.two[d];
        }
    }
    int hold = -1;
    if (HOLD) {
        int bd = 3;
        for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
            const auto& g = W.groups[gi];
            if (!g.reward || g.last_seen != W.round || (g.gems < HOLD_GEMS && !g.monolith)) continue;
            if (const int d = min_dist(c.p, g.tiles); d < bd) bd = d, hold = gi;
        }
        if (const int cand = hold; cand >= 0) {
            const vec2 ctr = W.groups[hold].center;
            float a = 0, b = 0, ta = 0, tb = 0;
            for (int i = 0; i < c.F.n; ++i) {
                const Unit& u = c.F.u[i];
                if (!u.alive || cheb({u.x, u.y}, ctr) > HOLD_D) continue;
                (u.ally ? a : b) += fight::ehp(u);
                (u.ally ? ta : tb) += c.S.thr[i];
            }
            const float r = (a * ta + 1.f) / (b * tb + 1.f);
            const int tr = W.trade(TRADE_R);
            if (!((r >= HOLD_R && tr >= 0) || (r >= HOLD_R2 && tr > 0))) hold = -1;
            debug_block({ std::cout << "HOLD R" << W.round << " i" << W.me_idx << " g" << cand << " on=" << (hold >= 0) << " ratio=" << r << " trade=" << tr << "\n"; });
        }
        if (hold >= 0) hold_gi = hold, hold_round = W.round;
    }
    for (const auto& m : W.camps)
        try { if (m.is_awake() && m.strength() > 0) c.camps.push_back({m.position(), m.strength()}); } catch (...) {}
    for (int i = 0; i < c.F.n; ++i)
        if (c.F.u[i].ally && i != c.mi && c.F.u[i].alive) c.allies.push_back({c.F.u[i].x, c.F.u[i].y});
    c.occ = bc::vision::champion_tiles() | bc::vision::camp_tiles() | bc::vision::teleport_tiles();
    PROF("p.setup");
    [[maybe_unused]] const auto fuel2 = bc::fuel_remaining();

    // verdict: simulate(3) when the turn can afford it; a verdict from last round on the same bodies is reused
    const bool contact = W.round - last_contact <= 2;
    const bool dash_plans = c.dash_slot >= 0 && !c.ranged && me.can_move();
    const int plan_fuel = TILE_FUEL * (9 + (dash_plans ? 12 : 0)) + EXEC_FUEL;
    float score = last_score;
    const bool fresh = W.round - last_score_round <= 1 && last_score_n == c.F.n;
    // The sim also leaves FARM_FUEL; 6+ bodies use the Lanchester verdict (simulating them lost 10 of 170, f-fuel CHANGES F2)
    if (!fresh && c.F.n <= SIM_MAX_UNITS && static_cast<int>(bc::fuel_remaining()) > plan_fuel + SIM_UNIT_FUEL * c.F.n + FARM_FUEL) {
        score = fight::simulate(c.F, 3, &c.S).score;
        last_score = score, last_score_round = W.round, last_score_n = c.F.n;
    } else if (W.round - last_score_round > 2) {
        float a = 0, b = 0, ta = 0, tb = 0; // Lanchester fallback
        for (int i = 0; i < c.F.n; ++i) {
            const Unit& u = c.F.u[i];
            if (!u.alive || fight::dist(u, mu) > 7) continue;
            (u.ally ? a : b) += fight::ehp(u);
            (u.ally ? ta : tb) += c.S.thr[i];
        }
        // graded so a clearly better side can still commit (+0.5 threshold) when simulate() is unaffordable
        const float r = (a * ta + 1.f) / (b * tb + 1.f);
        score = r > 1.6f ? 0.8f : r > 1.25f ? 0.4f : r < 0.625f ? -0.8f : r < 0.8f ? -0.4f : 0.f;
    }
    PROF("p.sim");
    [[maybe_unused]] const auto fuel3 = bc::fuel_remaining();
    const float hpf = hp_frac(me);
    const int gems = static_cast<int>(me.gem_count());
    constexpr int CG = CARRY ? 15 : 30; // a gem carrier
    c.commit = score > (contact || hunting ? STAY : COMMIT) && hpf >= 0.4f && !(gems >= CG && score < 1.f) && standoff <= 3;
    [[maybe_unused]] bool press = false;
    if constexpr (FINISH || PRESS || OPEN_GATE || LOW_RUN) { // c6f (zreach.h): a, b as PV_POWER's
        float a = 0, b = 0;
        for (int i = 0; i < c.F.n; ++i) if (c.F.u[i].alive && fight::dist(c.F.u[i], mu) <= 7) (c.F.u[i].ally ? a : b) += fight::ehp(c.F.u[i]) * c.S.thr[i];
        if constexpr (OPEN_GATE) plan::open_pr() = contact ? 1e9f : a / std::max(1.f, b); if constexpr (LOW_RUN) plan::low_run() = hpf < 0.3f && nearest <= 4 && a < 0.67f * b; // c7f
        if (FINISH && fin && standoff <= 3 && b <= 1.5f * a) c.commit = true;
        if (PRESS && !fin && hpf >= 0.6f && gems < 30 && standoff <= 3 && nearest <= 3 && a >= 1.5f * b) c.commit = press = true;
        debug_block({ if (fin) std::cout << "FIN R" << W.round << " " << bc::to_string(me.champion_name()) << " hp=" << hpf << " sc=" << score << " commit=" << c.commit << " a/b=" << a / std::max(1.f, b) << " so=" << standoff << " prey=" << fhp << " focus=" << c.focus << "\n"; });
    }
    if constexpr (CARRY) plan::carry_flag() = gems >= CG;
    if constexpr (PV_POWER) if (c.commit) { float a = 0, b = 0; for (int i = 0; i < c.F.n; ++i) if (c.F.u[i].alive && fight::dist(c.F.u[i], mu) <= 7) (c.F.u[i].ally ? a : b) += fight::ehp(c.F.u[i]) * c.S.thr[i]; c.commit = b < 2.f * a; } // v22: never commit into 2x

    float here = camp_danger(c, c.p);
    for (int j = 0; j < c.nf; ++j) here += contrib(c, j, c.p);
    const bool link_alive = (mu.link >= 0 && c.F.u[mu.link].alive) || mu.ghost;
    c.death = 300.f + 4.f * gems + (link_alive ? 300.f : 0.f);
    if constexpr (PV_RUN) pv_run = score < STAY || hpf < 0.6f;
    c.lambda = (c.commit ? 0.5f : (gems >= CG || hpf < 0.4f ? 1.6f : 1.f)) * std::clamp(800.f / std::max(1.f, c.my_ehp), 0.6f, 2.5f);
    if constexpr (PRESS) if (press) c.lambda *= PRESS_L; if constexpr (LOW_RUN) if (plan::low_run()) c.lambda *= 2.f;
    const auto& hf = W.home_field();
    const float hd = hf.get(c.p);

    const vec2 fpos = c.focus >= 0 ? vec2{c.F.u[c.focus].x, c.F.u[c.focus].y} : c.p;
    [[maybe_unused]] const int ally_d = REGROUP && !c.allies.empty() ? min_dist(c.p, c.allies) : 0;
    auto pos_term = [&](vec2 q) {
        float t = 0;
        if (pend_gi >= 0) t += W.groups[pend_gi].spawns.is_set(q) ? PEND_ON : 0.f; // o-altar 2b
        else if (hold >= 0) { // k-hold H1: on the reward tiles; declined, as close to them as the fight allows (no home pull)
            const auto& hg = W.groups[hold];
            t += hg.spawns.is_set(q) ? HOLD_ON : c.commit ? 0.f : -HOLD_STEP * static_cast<float>(min_dist(q, hg.tiles));
        }
        if (c.commit && c.focus >= 0) { t -= APPROACH * static_cast<float>(std::max(0, cheb(q, fpos) - reach_me)); if constexpr (FINISH) if (fin) t -= FIN_PULL * static_cast<float>(cheb(q, fpos));
            if constexpr (PRESS) if (press) t -= PRESS_PULL * static_cast<float>(cheb(q, fpos)); }
        else if (here > 0 && hold < 0 && pend_gi < 0) {
            const float a = hf.get(c.p), b = hf.get(q);
            if (a < FAR && b < FAR) { t += (hpf < 0.6f || gems >= CG ? HOME_HURT : HOME) * (a - b); if constexpr (LOW_RUN) if (plan::low_run()) t += HOME_HURT * (a - b); if constexpr (PV_RUN) if (pv_run) t += 2.f * (hpf < 0.6f || gems >= 30 ? HOME_HURT : HOME) * (a - b); }
            if constexpr (REGROUP) if (!c.commit && ally_d > 4) t -= 8.f * static_cast<float>(min_dist(q, c.allies) - ally_d);
        }
        // grouping helps a fight we take; while backing off it pulled hurt champions along the enemy (contest)
        if (c.commit && !c.allies.empty()) t -= COHESION * static_cast<float>(std::max(0, min_dist(q, c.allies) - 3));
        return t;
    };

    c.floor = EXEC_FUEL + 3000 + ((!c.commit && here <= 0) || (PV_QUIET && quiet >= 8) ? FARM_FUEL : 0); // PV_QUIET: a quiet standoff farms on
    Plan best;
    Plan base;
    if constexpr (HIT_FIRST) best.first = base.first = false;
    base.dest = c.p;
    eval_tile(c, best, base, 0, pos_term(c.p));
    static const std::array<vec2, 8> DIRS{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1}, vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};
    if (me.can_move()) {
        for (auto d : DIRS) {
            const vec2 q = c.p + d;
            if (!free_tile(c, q)) continue;
            Plan pl = base;
            pl.dest = q, pl.walk = true;
            eval_tile(c, best, pl, 1, pos_term(q));
        }
        if (dash_plans && (here > 0 || nearest <= 4)) {
            // dash next to a foe 3 away, dash + walk next to one 4 away, or (declined, under threat) dash out; stop at the floor
            bool out = false;
            for (int dx = -2; dx <= 2 && !out; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    if ((out = static_cast<int>(bc::fuel_remaining()) < c.floor)) break;
                    const vec2 q{c.p.x + dx, c.p.y + dy};
                    if (cheb(c.p, q) != 2 || !free_tile(c, q)) continue;
                    bool via = false; // a dash needs a non-wall tile next to both ends (as an earlier bot's dash_field)
                    for (auto e : DIRS) {
                        const vec2 m = c.p + e;
                        if (cheb(m, q) <= 1 && bc::map::is_in_bounds(m) && !bc::map::walls().is_set(m)
                            && (!DASH_CAMP || !camp_live_at(m))) { via = true; break; } // p-fix F15: not over a camp (error 32)
                    }
                    if (!via) continue;
                    Plan pl = base;
                    pl.dest = q, pl.dash = q;
                    bool engage = false;
                    for (int j = 0; j < c.nf; ++j) engage |= cheb(q, c.foes[j].pos) == 1;
                    const bool escape = !c.commit && here > 0 && hf.get(q) < hd;
                    if (engage || escape) eval_tile(c, best, pl, 2, pos_term(q));
                    if (escape) { // dash + walk: 3 tiles towards spawn
                        vec2 r = q;
                        for (auto e : DIRS) {
                            const vec2 t = q + e;
                            if (cheb(c.p, t) == 3 && free_tile(c, t) && hf.get(t) < hf.get(r)) r = t;
                        }
                        if (r != q) {
                            Plan pw = pl;
                            pw.dest = r, pw.walk = true;
                            eval_tile(c, best, pw, 2, pos_term(r));
                        }
                    }
                    for (int j = 0; j < c.nf; ++j) {
                        const vec2 f = c.foes[j].pos;
                        if (cheb(c.p, f) != 4 || cheb(q, f) != 2) continue;
                        for (auto e : DIRS) {
                            const vec2 r = q + e;
                            if (cheb(r, f) != 1 || cheb(c.p, r) != 3 || !free_tile(c, r)) continue;
                            Plan pw = pl;
                            pw.dest = r, pw.walk = true;
                            eval_tile(c, best, pw, 2, pos_term(r));
                        }
                    }
                }
        }
    }
    if constexpr (HOR_PLAY) if (dash_plans && (here > 0 || nearest <= 4)) hor_plans(c, best, base, pos_term); // h-heart
    PROF("p.plan");
    [[maybe_unused]] const auto fuel4 = bc::fuel_remaining();

    const bool dealt = best.value > 0;
    if (dealt || here > 0) last_contact = W.round;
    standoff = dealt || here > 0 ? 0 : c.commit ? standoff + 1 : standoff; if constexpr (PV_QUIET) quiet = !dealt && end_round == W.round - 1 && end_hp - me.health() < 1.f ? quiet + 1 : 0;
    debug_block({
        std::cout << "PVP R" << W.round << " " << bc::to_string(me.champion_name()) << " sc=" << score << " commit=" << c.commit
                  << " here=" << here << " focus=" << c.focus << " best v=" << best.value << " s=" << best.score << " dest=" << bc::to_string(best.dest)
                  << " sp=" << best.spell << " h=" << best.hit0 << "/" << best.hit1 << " so=" << standoff << " n=" << c.F.n << " lead=" << c.F.ally_starts << " pr=" << me.prev_priority() << "/" << me.priority()
                  << " me@" << bc::to_string(c.p) << " foes:";
        for (int j = 0; j < c.nf; ++j) std::cout << " " << bc::to_string(champ(c.foes[j].id).champion_name()) << "@" << bc::to_string(c.foes[j].pos) << ":" << (int)c.foes[j].hp << (c.foes[j].stunned ? "S" : "") << "/" << (int)contrib(c, j, c.p) << "t" << (int)c.foes[j].turns;
        std::cout << " fuel " << fuel0 - fuel1 << "/" << fuel1 - fuel2 << "/" << fuel2 - fuel3 << "/" << fuel3 - fuel4 << " left " << fuel0 << "\n";
    });
    if (!dealt && (here <= 0 || (PV_QUIET && quiet >= 8)) && (!c.commit || standoff > 3 || (PV_QUIET && quiet >= 8))) { // PV_QUIET: a quiet standoff farms on
        // decline: farm on, never stepping into enemy reach (a foe's damage covers 0..r with no gap: one Chebyshev square, ~1k)
        avoid.clear();
        for (int j = 0; j < c.nf; ++j) {
            const Unit& u = c.F.u[c.foes[j].fi];
            if (!u.alive) continue;
            int r = -1;
            for (int d = 0; d <= fight::MAXD; ++d) if (foe_dmg(c.foes[j], d, false) > 0) r = d;
            // f-small R1 soon_risk: a spell back next turn reaches one tile further
            for (int d = 0; d + 1 < fight::MAXD; ++d) if (c.foes[j].soon[d + 1] > 0) r = std::max(r, d);
            if (r >= 0) avoid.draw_chebyshev(c.foes[j].pos, static_cast<float>(r));
        }
        {
            // q-act review: 3, not 2: DASH_WALK and CAP_STEP after a dash end a farm turn up to 3 tiles out (radius 2 left it unmasked)
            bc::bitboard win;
            win.clear();
            win.draw_chebyshev(c.p, 3.f);
            avoid &= win;
        }
        avoid_on = true;
        return false;
    }
    PROF("p.dbg");
    execute(c, best);
    PROF("p.exec");
    cast_support(here > 0 || (HOR_PLAY && hor::me() && hor_danger(c))); // h-heart: HoR shields on the end tile's risk
    return true;
}

} // namespace pvp
