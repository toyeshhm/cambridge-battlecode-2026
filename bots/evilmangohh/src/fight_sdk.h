#pragma once
// fight_sdk.h -- fills a fight::Fight from what this champion sees (self included).
// Cost: one raw sdk_champion record per roster handle; spell metadata cached per handle.
#include <cambc.h>
#include <type_traits>
#include <variant>
#include "fight.h"

namespace fight {

inline Spell spell_of(const cambc::ability& ab) {
    Spell s;
    const auto& inf = ab.info();
    if (!inf) return s;
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        s.excl = v.exclusive_with_movement;
        if constexpr (std::is_same_v<T, cambc::ability_attack_reset>) s.k = K::reset;
        else if constexpr (std::is_same_v<T, cambc::ability_dash>) {
            s.k = K::dash;
            if constexpr (HOR_MODEL) if (v.modifier > 0) s.k = K::strike, s.mod = v.modifier, s.range = (int)v.range, s.radius = (int)v.radius;
        }
        else if constexpr (std::is_same_v<T, cambc::ability_damage>) s.k = K::nuke, s.mod = v.modifier, s.range = (int)v.range;
        else if constexpr (std::is_same_v<T, cambc::ability_cc>) s.k = v.modifier > 0 ? K::stun_nuke : K::stun, s.mod = v.modifier, s.range = (int)v.range;
        else if constexpr (std::is_same_v<T, cambc::ability_aoe_damage>)
            s.k = v.duration > 0 ? K::aoe_stun : K::aoe, s.mod = v.modifier, s.range = (int)v.range, s.radius = (int)v.radius;
        else if constexpr (std::is_same_v<T, cambc::ability_shield>) s.k = K::shield, s.mod = v.modifier;
        else if constexpr (std::is_same_v<T, cambc::ability_heal>) s.k = K::heal, s.mod = v.modifier, s.range = (int)v.range;
    }, *inf);
    return s;
}

// h-micro T1: next-round turn order. Every champion (dead ones too) keeps a slot in its team's order: higher
// priority first, ties by handle. priority() is readable only in vision, so hidden champions keep the last value
// seen. (Counting them as behind everyone instead, which fixed one debug case, lost 25 of 376 games: va, CHANGES.md.)
struct Order { std::array<int, 16> pn{}; std::array<unsigned, 16> h{}; std::array<bool, 16> ally{}; int n = 0; };
inline Order& order() { static Order o; return o; }
// Slot of handle h in its team's order next round if its priority there is pv.
inline int next_rank(unsigned h, int pv) {
    const Order& o = order();
    bool mine = false;
    for (int k = 0; k < o.n; ++k) if (o.h[k] == h) mine = o.ally[k];
    int r = 0;
    for (int k = 0; k < o.n; ++k) {
        const unsigned g = o.h[k];
        if (o.ally[k] == mine && g != h && (o.pn[g] > pv || (o.pn[g] == pv && g < h))) ++r;
    }
    return r;
}

inline constexpr int GHOST_AGE = 2; // h-micro G1: rounds an unseen enemy stays in the fight (0: off)
// h-micro G1: last sight of each enemy (filled by from_vision).
struct Memory { std::array<Unit, 16> u{}; std::array<int, 16> round{}; Memory() { round.fill(-100); } };
inline Memory& memory() { static Memory m; return m; }
// An enemy out of sight now, seen in the last GHOST_AGE rounds within r of (x, y) and still alive.
inline bool ghost_near(int x, int y, int r) {
    const Memory& m = memory();
    const int now = static_cast<int>(cambc::round_number());
    for (unsigned h = 0; h < m.u.size(); ++h) {
        const int age = now - m.round[h];
        if (age < 1 || age > GHOST_AGE || std::max(std::abs(m.u[h].x - x), std::abs(m.u[h].y - y)) > r) continue;
        if (cambc::detail::handle_access::make_champion(static_cast<cambc::handle_champion_t>(h)).is_alive()) return true;
    }
    return false;
}

// Every visible, living champion of both teams, written into f (reused: constructing a Fight costs ~2k).
// Team A (handles 0..) opens odd rounds.
inline void from_vision(Fight& f) {
    namespace d = cambc::detail;
    struct Meta { bool init = false, cassi = false, issac = false, splash = false, ren = false; std::array<Spell, 2> sp{}; };
    static std::array<Meta, 16> meta{};
    static std::vector<cambc::champion> roster;
    static std::size_t n_ally = 0;
    static bool items_ok = true;  // [UNTESTED] item_count() on enemies; falls back to taken = 1
    auto& mem = memory().u;
    auto& mem_round = memory().round;
    const int now = static_cast<int>(cambc::round_number());
    if (roster.empty()) {
        roster = cambc::champions(cambc::team::ally);
        n_ally = roster.size();
        for (auto& c : cambc::champions(cambc::team::enemy)) roster.push_back(c);
        Order& o = order();
        for (std::size_t k = 0; k < roster.size() && o.n < 16; ++k, ++o.n)
            o.h[o.n] = static_cast<unsigned>(static_cast<cambc::handle_champion_t>(roster[k])), o.ally[o.n] = k < n_ally;
    }
    f.n = 0, f.hold[0] = f.hold[1] = false;
    const auto id0 = [](const cambc::champion& c) { return static_cast<unsigned>(static_cast<cambc::handle_champion_t>(c)); };
    f.ally_starts = ((cambc::round_number() & 1) == 1) == (id0(roster[0]) == 0);
    std::array<int, MAXU> who{};
    for (std::size_t k = 0; k < roster.size(); ++k) {
        const auto& c = roster[k];
        const unsigned h = id0(c);
        d::champion_info ci{};
        if (f.n == MAXU || h >= meta.size() || d::sdk_champion(h, &ci) != 0 || ci.x < 0 || (ci.state & 1) == 0) continue;  // hidden or dead
        Meta& m = meta[h];
        if (!m.init) {  // abilities() of a champion object built while hidden carry no metadata: rebuild it
            const auto live = d::handle_access::make_champion(static_cast<cambc::handle_champion_t>(c));
            const auto abs = live.abilities();
            m.sp = {spell_of(abs[0]), spell_of(abs[1])};
            const auto t = live.champion_name();
            m.cassi = t == cambc::champion_type::cassi;
            m.issac = t == cambc::champion_type::issac;
            m.splash = live.passive() == cambc::passive_type::splash_attack;
            m.ren = live.passive() == cambc::passive_type::retaliation;
            m.init = true;
        }
        Unit u;
        u.ally = k < n_ally;
        u.id = h;
        u.hp = ci.health, u.max_hp = ci.max_health, u.str = ci.strength, u.foc = ci.focus;
        u.ranged = ci.attack_range > 1;
        u.x = ci.x, u.y = ci.y;
        u.acted = (ci.state & (1 << 5)) != 0;
        u.channel = (ci.state & 0x6) != 0;  // recalling | submitting gems
        if constexpr (PV_RECALL) if ((ci.state & 0x6) == 0x2) { std::int32_t ct = 99; d::sdk_champion_channel_time(h, &ct); u.channel = ct <= 3; } // v22
        u.splash = m.splash;
        u.retaliate = m.ren ? 0.2f * ci.focus : 0;
        u.sp = m.sp;
        // h-heart HOR_MODEL: HoR's passive scales all its damage with missing health (ponytail: folded into str and focus, so the
        // model's shield reads it too; it is at most +50%)
        if constexpr (HOR_MODEL) if (m.sp[0].k == K::strike || m.sp[1].k == K::strike) { const float k = hor::passive_mul(ci.health, ci.max_health); u.str *= k, u.foc *= k; }
        for (int i = 0; i < 2; ++i) d::sdk_champion_cooldown(h, i, &u.sp[i].cd);  // enemy cooldowns are readable
        const auto b = c.buffs();
        if (b.has_shield()) u.shield = b.get<cambc::buff_shield>().value;
        if (b.has_stun() && b.get<cambc::buff_stun>().duration > 0) u.stun = 1;  // STUN(0) = already served
        std::uint32_t gems = 0;
        d::sdk_champion_gems(h, &gems);
        u.gems = (int)gems;
        u.prio = c.prev_priority();
        if (std::int32_t pn = 0; d::sdk_bc_champion_priority(h, 1, &pn) == 0) order().pn[h] = pn;
        u.prio_next = order().pn[h];
        if (items_ok) try {
            using IT = cambc::item_type;
            const auto it = c.item_count();
            if (it[IT::durability_emblem_of_strength] + it[IT::durability_emblem_of_focus] + it[IT::durability_emblem_of_health] > 0) u.taken = 0.9f;
        } catch (const cambc::error&) { items_ok = false; }
        who[f.n] = (m.cassi ? 1 : m.issac ? 2 : 0) | (u.ally ? 4 : 0);
        if (!u.ally) mem[h] = u, mem_round[h] = now;
        f.u[f.n++] = u;
    }
    // h-micro G1: enemies seen in the last GHOST_AGE rounds and not now (a bush, the edge of vision) stay in the
    // fight where they were last seen, spells ticked by the rounds since (swirl R476: their Cassi stepped into a
    // bush next to our Cassi, unseen, and nuked her and Issac). They can hit us; we cannot target them.
    for (std::size_t k = n_ally; k < roster.size() && GHOST_AGE > 0; ++k) {
        const unsigned h = id0(roster[k]);
        if (f.n == MAXU || h >= mem.size() || mem_round[h] == now || now - mem_round[h] > GHOST_AGE) continue;
        if (!roster[k].is_alive()) { mem_round[h] = -100; continue; }
        Unit u = mem[h];
        u.hidden = true, u.acted = false, u.stun = 0, u.channel = false, u.link = -1;
        for (auto& sp : u.sp) sp.cd = std::max(0, sp.cd - (now - mem_round[h]));
        who[f.n] = 0;
        f.u[f.n++] = u;
    }
    for (int i = 0; i < f.n; ++i)
        for (int j = 0; j < f.n; ++j)
            if ((who[i] & 3) == 1 && (who[j] & 3) == 2 && (who[i] & 4) == (who[j] & 4)) f.u[i].link = j, f.u[j].link = i;
    // a living Cassi means a living Issac (they die and respawn together): an unseen partner still dies with her
    for (int i = 0; i < f.n; ++i) f.u[i].ghost = (who[i] & 3) != 0 && f.u[i].link < 0;
}

}  // namespace fight
