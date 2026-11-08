// p-monolith MONO_TAKE (mono.h): a deliberate full-team monolith take. Team state = squad 0's target byte with the help
// flag on a monolith group (MONO_NOSIEGE never makes a monolith a normal target, so that byte means "take").
// Leader (squad 0's lowest active slot): starts a take in choose_target (mono_lead), keeps or aborts it in mono_turn.
// Everyone: stage at 6 while it sleeps, walk in together (proximity never wakes it), all adjacent, stagger the stuns; the
// healthiest of the nearest takes each hit (turn priority by hp), the hurt leave to 6 and recall. MONO_DENY: deny().
#include "bot.h"
#include "actions.h"
#include "pvp.h"
#include "world.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <variant>

namespace {
constexpr float HIT = 200.f;      // monolith strength
constexpr int GO_WAIT = 25;       // rounds at the stage before going in one short
constexpr int GO_IN = 10;         // rounds after the go before the first hit even if someone is not adjacent yet
constexpr int NO_RESTART = 40;    // rounds every instance waits after a take ends
constexpr int NO_RESTART_FARM = 150; // off only_way groups (a failed attempt there costs the farm race ~50 rounds of the team)
constexpr float ESC = 5.f;        // hits a walk from 1 to 6 takes
constexpr float FINISH_T = 4.f;   // TAKE_FINISH: rounds to the kill under which an enemy near does not end the take

int g_prio = -1, g_prio_round = -1; // turn priority to set at the end of this turn (engaged)
int last_gi = -1, last_round = -100; // last take I was in (abort: get out of range)
int stage_since = -1, go_since = -1;
bool healing = false;
int no_start_until = 0;
int started = 0;             // round this instance first kept the take (leader)
constexpr int STALL = 90;    // staging longer than this: give the take up

// Per champion type: basic attacks a round (1 + attack resets / 8), spell damage x focus a round, stuns per 8 rounds,
// shield and heal x focus per cast. Design data (bc::champion_designs), cached per type.
struct Kit { float basic = 1, spell = 0, stuns = 0, shield = 0, heal = 0; bool init = false; };
const Kit& kit_of(bc::champion_type t) {
    static std::array<Kit, 16> cache{};
    const auto i = static_cast<size_t>(t);
    auto& k = cache[i & 15];
    if (k.init || i >= bc::champion_designs.size()) return k;
    k.init = true;
    for (const auto& a : bc::champion_designs[i].abilities()) {
        if (!a.info()) continue;
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bc::ability_attack_reset>) k.basic += 1.f / 8;
            else if constexpr (std::is_same_v<T, bc::ability_damage>) k.spell += v.modifier / 8;
            else if constexpr (std::is_same_v<T, bc::ability_aoe_damage> || std::is_same_v<T, bc::ability_cc>
                               || std::is_same_v<T, bc::ability_dash>) { k.spell += v.modifier / 8; k.stuns += v.duration > 0; }
            else if constexpr (std::is_same_v<T, bc::ability_shield>) k.shield += v.modifier;
            else if constexpr (std::is_same_v<T, bc::ability_heal>) k.heal += v.modifier;
        }, *a.info());
    }
    return k;
}

// hp a champion keeps: it leaves below this; at least two rounds of every live monolith's hit (01boss-battle: a pair, 400 a round)
float reserve(float mhp, int nm) { return std::max({TAKE_RES * mhp, TAKE_RES_ABS, 2.f * HIT * static_cast<float>(nm) + 50.f}); }
int live_monos(const Group& g) {
    int n = 0;
    for (size_t k = 0; k < g.members.size(); ++k) n += g.member_str[k] >= HIT && g.hp_member[k] > 0;
    return std::max(n, 1);
}

struct Mate { float hp, mhp, str, foc; bc::champion_type t; vec2 at; bool away; };
// Me and every living ally: live stats in sight; otherwise the stats last seen of that ally at full hp (they heal before going
// in), or its design stats if never seen. (Design x my own scale overstated the pool by ~65%: my health build is not the
// casters' focus build, so the leader started takes on estimates the live check at the stage refused: k1 debug, 21 a game.)
// Away allies (recalling, healing) are flagged: model() counts them only for a take that outlasts their trip back.
std::vector<Mate> mates() {
    static std::array<Mate, 8> last{};
    static std::array<bool, 8> known{};
    std::vector<Mate> v;
    for (int i = 0; i < W.n_allies && i < 6; ++i) {
        if (!W.alive[i]) continue;
        const bool away = i != W.me_idx && W.slot_away(i);
        const auto t = W.ally_h[i].champion_name();
        bc::detail::champion_info ci{};
        if (bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(W.ally_h[i])), &ci) == 0 && ci.x >= 0) {
            last[i] = {ci.max_health, ci.max_health, ci.strength, ci.focus, t, {-1, -1}, away}, known[i] = true;
            v.push_back({away ? ci.max_health : ci.health, ci.max_health, ci.strength, ci.focus, t, {ci.x, ci.y}, away});
        } else if (known[i]) {
            v.push_back(last[i]);
            v.back().away = away;
        } else {
            const auto b = bc::champion_designs[static_cast<size_t>(t)].base_stats();
            v.push_back({b.base_health, b.base_health, b.base_strength, b.base_focus, t, {-1, -1}, away});
        }
    }
    return v;
}

struct Model { float pool = 0, need = 0, t = 0; };
// The take of what is left of group gi: rounds t = hp / team damage; need = damage the rotation absorbs in t (200 per live
// monolith a round, less the staggered stuns, shields and heals); pool = hp above each champion's reserve.
Model model(int gi, const std::vector<Mate>& ms) {
    const auto& g = W.groups[gi];
    // trip back for an away ally: recall 5 + the walk from spawn
    const int back = 5 + (W.spawn_ready ? std::min<int>(W.spawn_field.get(g.tiles[0]), 200) : 40);
    float h = 0;
    int nm = 0;
    for (size_t k = 0; k < g.members.size(); ++k) {
        h += std::max(0.f, g.hp_member[k]);
        nm += g.member_str[k] >= HIT && g.hp_member[k] > 0;
    }
    Model m;
    float dmg = 0, st = 0, mit = 0;
    float h0 = 0, d0 = 0; // first pass: damage of those present, for the time the take lasts
    for (const auto& x : ms) if (!x.away) { const Kit& k = kit_of(x.t); d0 += x.str * k.basic + x.foc * k.spell; }
    for (const auto& g2 : g.hp_member) h0 += std::max(0.f, g2);
    const float t0 = h0 / std::max(1.f, 0.85f * d0);
    for (const auto& x : ms) {
        if (x.away && t0 < static_cast<float>(back) + 10.f) continue;
        const Kit& k = kit_of(x.t);
        dmg += x.str * k.basic + x.foc * k.spell;
        st += k.stuns;
        mit += (0.5f * k.shield + 0.7f * k.heal) * x.foc / 8.f;
        m.pool += std::max(0.f, x.hp - reserve(x.mhp, std::max(nm, 1)));
    }
    m.t = h / std::max(1.f, 0.85f * dmg); // 0.85: walking in, misses, spells held for the stagger
    const float f = nm > 0 ? std::min(0.75f, 0.85f * st / (8.f * static_cast<float>(nm))) : 0.f;
    const float in = std::max(0.25f * HIT * static_cast<float>(nm), HIT * static_cast<float>(nm) * (1.f - f) - mit);
    m.need = m.t * in; // the reserves (TAKE_RES of each) pay for an abort's walk out
    return m;
}

// the take is the only way to bank: boss maps (every gem group a monolith), or a monolith group sealing an altar (z-boss seal:
// every altar's free tiles are monolith tiles, so any group next to an altar opens one). Not seal_group(): that is only the
// first group seal_detect met, on 00lanes-sealed (18 altars) the A side's, so side B staged at the far seal and stalled out
// every take (judge debug). gi is always a monolith group here.
bool only_way(int gi) {
    if (W.boss_map) return true;
    if (!SEAL_NOPARK || g_seal != 2) return false;
    const int r = static_cast<int>(bc::constants::submit_gems_range);
    for (auto t : W.groups[gi].tiles) for (auto a : W.altars) if (cheb(t, a) <= r) return true;
    return false;
}
// a full take with margin, or (TAKE_PART) a first session that gets at least TAKE_PART of the way
// (only_way groups only: elsewhere the farm is the alternative, and a session cut short costs the team a round trip home)
bool seal_take(int gi) { return SEAL_TAKE && !W.boss_map && only_way(gi); } // SEAL_TAKE (mono.h): a monolith sealing the altars
bool go_ok(const Model& m, int gi) {
    const float part = seal_take(gi) && W.round >= STAKE_R ? STAKE_PART : TAKE_PART;
    return m.pool >= TAKE_MARGIN * m.need || (TAKE_PART > 0 && only_way(gi) && m.pool >= part * m.need);
}

} // namespace
std::vector<vec2> mono::mono_tiles(int gi) {
    std::vector<vec2> v;
    const auto& g = W.groups[gi];
    for (size_t k = 0; k < g.tiles.size(); ++k) if (g.member_str[k] >= HIT) v.push_back(g.tiles[k]);
    return v;
}

bool mono::enemy_near(const std::vector<vec2>& pts, int r) {
    for (const auto& c : W.enemy_h) {
        bc::detail::champion_info ci{};
        if (bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)), &ci) == 0 && ci.x >= 0
            && (ci.state & 1) != 0 && min_dist({ci.x, ci.y}, pts) <= r) return true;
    }
    return false;
}
namespace {
using mono::mono_tiles;
using mono::enemy_near;

// The judge of a running take (mono_turn): false = abort (an enemy near, a stall while gathering, the model at the stage,
// or, engaged, too few left or the hp above the reserves no longer covering the rest)
int kept_gi = -1, kept_round = -100;
bool keep(int gi) {
    const auto& g = W.groups[gi];
    if (kept_gi != gi || kept_round < W.round - 5) started = W.round; // the stall clock starts at my first keep
    kept_gi = gi, kept_round = W.round;
    if (g.reward) return g.last_seen >= W.round - 10; // guard the capture
    if (g.respawn_at > W.round) return false;          // captured
    const auto mt = mono_tiles(gi);
    // an enemy near ends it, unless (TAKE_FINISH) the kill is a few rounds off (hf6 debug: 5 sessions left monoliths 1-3 rounds
    // from dead to an enemy that came near, and the HoR side took two of them)
    if (mt.empty() || ((enemy_near(mt, 9) || enemy_near({bc::self().position()}, 8))
                       && !(TAKE_FINISH && g.awake && g.last_seen >= W.round - 2 && model(gi, mates()).t <= FINISH_T))) return false;
    // staging (asleep): the team heals and gathers on estimates; once all are at the stage the live model decides, before
    // anyone wakes it (an abort there costs nothing)
    const bool engaged = g.awake && g.last_seen >= W.round - 2;
    const Model m = model(gi, mates());
    if (!engaged) {
        int active = 1;
        for (int i = 0; i < W.n_allies && i < 6; ++i) active += i != W.me_idx && W.alive[i] && !W.slot_away(i);
        const bool all = (min_dist(bc::self().position(), mt) <= TAKE_STAGE + 1) + W.arrived_claims(gi) >= active;
        debug_block({ std::cout << "MKEEP0 R" << W.round << " i" << W.me_idx << " g" << gi << " all=" << all << " pool=" << m.pool << " need=" << m.need << "\n"; });
        return W.round - started < STALL && (!all || go_ok(m, gi));
    }
    debug_block({ std::cout << "MKEEP R" << W.round << " i" << W.me_idx << " g" << gi << " pool=" << m.pool << " need=" << m.need << " t=" << m.t << "\n"; });
    // Engaged: never one alone at it; the walk out of an abort is ESC rounds of hits on the healthiest of the nearest, so the
    // ones not about to leave must hold ESC + 2 of them (cb2 debug: Ryth left at its reserve, Prism 500 + Tavi 375 walked out
    // under 5 hits, Prism died). A rule that the healthiest alone holds ESC + 2 made 00contest 0 deaths but its first take
    // r1229 instead of r666 (cb6 vs cb5): a monolith kill gives the enemy nothing, so speed wins. The pool must cover the need
    // (TAKE_PART: the session goes on while the team has ESC hits left; no regeneration, the damage stays).
    const int nm = live_monos(g);
    const float hit = HIT * static_cast<float>(nm);
    int present = 0, hold = 0;
    for (const auto& x : mates()) {
        if (x.away || x.at.x < 0 || min_dist(x.at, mt) > 2) continue;
        ++present;
        if (x.hp >= reserve(x.mhp, nm) + hit) hold += static_cast<int>((x.hp - 1.f) / hit);
    }
    return present >= 2 && hold >= static_cast<int>(ESC) + 2 && (m.pool >= TAKE_KEEP * m.need || (TAKE_PART > 0 && only_way(gi) && m.pool >= ESC * HIT));
}

// BFS field to the monolith tiles of group gi (walls only), cached per group
int mfield_gi = -1;
const Field& mono_field(int gi, const std::vector<vec2>& mt) {
    static Field mfield; // function-local: no static initialiser in the all-off build
    if (mfield_gi != gi) {
        bc::bitboard roots;
        roots.clear();
        for (auto t : mt) roots.set(t);
        mfield.compute(W.static_walls, roots, W.visited);
        mfield_gi = gi;
    }
    return mfield;
}

} // namespace
// walk the monolith field down to `want` steps (1 = adjacent); never closer than want
bool mono::move_ring(int gi, const std::vector<vec2>& mt, int want) {
    const vec2 p = bc::self().position();
    const auto& f = mono_field(gi, mt);
    const int cur = f.get(p);
    if (cur <= want) return false;
    if (cur > want + 2 && dash_field(f)) return true;
    return best_step([&](vec2 q) -> float {
        const int d = f.get(q);
        return d >= FAR || d < want ? std::numeric_limits<float>::infinity() : static_cast<float>(d);
    }) || (cur > want + 1 && step_field(f));
}
namespace {
using mono::move_ring;
using mono::get_out;
using mono::camp_threat;
using mono::spells;

} // namespace

// Any take that ends (abort or kill) holds every instance off a new start for NO_RESTART rounds: a far leader that never saw
// the abort restarted it each round, and the judge at the monolith aborted it each round (k2 debug: 29 starts a game).
static void note_take(int tg) {
    static int prev = -1;
    if (prev >= 0 && tg < 0) no_start_until = std::max(no_start_until, W.round + (only_way(prev) ? NO_RESTART : NO_RESTART_FARM));
    prev = tg;
}

// maps where nothing here can fire run none of it (the hooks return at once, so they replay the base game for game: the 13
// pool maps without a monolith, and with MONO_TAKE_FARM off every map that is neither a boss map nor sealed)
static bool any_mono() {
    static const bool v = [] { for (const auto& g : W.groups) if (g.monolith) return true; return false; }();
    return v && (MONO_TAKE_FARM || MONO_DENY || W.boss_map || g_seal == 2);
}

int mono::take_group() { // gated too: rr-c2's own help flag on a monolith group is not a take (bot.cpp's mhold hook)
    if (!any_mono() || !(W.sm[6] & 0x80)) return -1;
    const int gi = W.squad_target(0);
    return gi >= 0 && gi < static_cast<int>(W.groups.size()) && W.groups[gi].monolith ? gi : -1;
}

int mono::prio() { return g_prio_round == W.round ? g_prio : -1; }

// choose_target, first: a take running is my target; else squad 0's leader may start one (whole team, help flag)
bool Bot::mono_lead() {
    if (!any_mono()) return false;
    const int cur = mono::take_group();
    if (cur >= 0) { target = cur; W.set_claim(cur); return true; }
    return false;
}

// Squad 0's leader starts a take (whole team: help flag) at the END of its turn, after every decision (main.cpp): fuel does
// not carry over, so the evaluation changes nothing on maps or rounds where it does not fire (no fuel chaos).
void Bot::mono_end() {
    if (!any_mono()) return;
    const int tg = mono::take_group();
    note_take(tg);
    if (tg >= 0 || bc::fuel_remaining() < 25000) return;
    auto& me = bc::self();
    if (!me.is_alive() || W.round != static_cast<int>(bc::round_number()) || W.squad_of[W.me_idx] != 0 || !W.is_leader()
        || W.slot_away(W.me_idx) || healing || W.round < no_start_until || W.round - pvp::last_seen <= 15) return;
    // the start counts the team as the stage will see it: an ally under 80% goes to heal first (away), so it is left out here
    // too (mm1 debug: the pool fell 26% and the need rose 43% at the stage, three starts aborted on 00monolith)
    auto ms = mates();
    for (auto& x : ms) if (x.hp < 0.8f * x.mhp) x.away = true;
    if (ms.size() < 2) return;
    const vec2 p = me.position();
    int best = -1;
    float best_rate = 0;
    for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
        const auto& g = W.groups[gi];
        if (!g.monolith || g.reward || g.respawn_at > W.round || g.skip_until > W.round || (!MONO_TAKE_FARM && !only_way(gi))) continue;
        if (g.awake && g.last_seen >= W.round - 10) continue; // a session just ended there (or someone else is on it)
        const int d = W.dist_to_group(gi, p);
        if (d >= FAR || enemy_near(mono_tiles(gi), 12)) continue;
        const Model m = model(gi, ms);
        if (!go_ok(m, gi)) continue;
        // the whole team's time: walk, gather (~25), the kill and capture, the heal trip home after it, and a trip home and back
        // for every extra session the pool cannot cover (A races: without the gathering and heal costs the take fired on
        // 01big-world / 01fracture at r1240-1610 and made them 220-290 rounds slower unopposed)
        const float home = 5.f + static_cast<float>(W.spawn_ready ? std::min<int>(W.spawn_field.get(g.tiles[0]), 150) : 40);
        const float sessions = std::ceil(TAKE_MARGIN * m.need / std::max(1.f, m.pool));
        const float rate = (g.gems + 0.15f * g.gold) / (m.t + static_cast<float>(d) + 10.f + 25.f + home + (sessions - 1.f) * 2.f * home);
        if (rate > best_rate) best_rate = rate, best = gi;
    }
    if (best < 0) return;
    if (!only_way(best)) {
        // plan.cpp's terms: the best non-monolith job for a squad, value / (walk + kill + capture), per squad (dps_scale as the
        // last update_needs left it: nothing here may change the planner's caches)
        const float dps1 = 50.f * dps_scale * static_cast<float>(std::max(1, W.alive_count() / std::max(1, W.n_squads)));
        float alt = 0;
        for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
            const auto& g = W.groups[gi];
            if (g.monolith || g.skip_until > W.round || g.need > W.alive_count()) continue;
            const int d = W.dist_to_group(gi, p);
            const int wait = g.respawn_at > W.round ? g.respawn_at - W.round : 0;
            if (d >= FAR) continue;
            const float hp = g.reward ? 0.f : g.hp_seen >= 0 ? g.hp_seen : g.max_hp;
            alt = std::max(alt, (g.gems + 0.15f * g.gold) / (static_cast<float>(std::max(d, wait)) + hp / dps1 + 7.f));
        }
        debug_block({ std::cout << "MSTART? R" << W.round << " i" << W.me_idx << " g" << best << " rate=" << best_rate << " alt=" << alt << "x" << W.n_squads << "\n"; });
        if (best_rate <= TAKE_ALT_X * alt * static_cast<float>(W.n_squads)) return;
    }
    debug_block({ const Model m = model(best, ms); std::cout << "MSTART R" << W.round << " i" << W.me_idx << " g" << best << " pool=" << m.pool << " need=" << m.need << " t=" << m.t << " n=" << ms.size() << "\n"; });
    W.set_squad_target(best, true);
}

const char* Bot::mono_turn(const std::vector<bc::champion>& enemies) {
    if (!any_mono()) return nullptr;
    auto& me = bc::self();
    const vec2 p = me.position();
    // healed (cb7 debug: a leader that ended a take hurt never cleared the flag and never started another one)
    if (healing && me.health() >= 0.95f * me.max_health()) healing = false;
    int gi = MONO_TAKE ? mono::take_group() : -1;
    note_take(gi);
    // Who judges: while the team gathers, squad 0's leader; once it is awake, the lowest slot at the monolith (a leader walking
    // back from a heal cannot see the fighters' hp: cb2 debug, nobody judged and the last two walked out with 5 hits to take)
    bool judge = false;
    if (gi >= 0 && !W.slot_away(W.me_idx) && !healing) {
        const auto& tg = W.groups[gi];
        const auto mt = mono_tiles(gi);
        if (!(tg.awake && tg.last_seen >= W.round - 2)) judge = W.squad_of[W.me_idx] == 0 && W.is_leader();
        else if (min_dist(p, mt) <= TAKE_STAGE + 1) {
            judge = true;
            for (int i = 0; i < W.me_idx && i < 6 && judge; ++i) {
                bc::detail::champion_info ci{};
                judge = !(W.alive[i] && !W.slot_away(i) && bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(W.ally_h[i])), &ci) == 0
                          && ci.x >= 0 && min_dist({ci.x, ci.y}, mt) <= TAKE_STAGE + 1);
            }
        }
    }
    if (judge && !keep(gi)) {
        debug_block({ std::cout << "MABORT R" << W.round << " i" << W.me_idx << " g" << gi << "\n"; });
        W.set_squad_target(-1, false);
        if (!W.groups[gi].reward) no_start_until = W.round + NO_RESTART;
        gi = -1;
    }
    if (gi < 0) {
        stage_since = -1;
        // a take I was in just ended short (aborted, not killed): no restart from me either for a while
        if (last_gi >= 0 && last_round == W.round - 1 && !W.groups[last_gi].reward && W.groups[last_gi].respawn_at <= W.round)
            no_start_until = std::max(no_start_until, W.round + NO_RESTART);
        // after an abort: out of range of that monolith unless seen asleep (behind a wall it is often out of sight)
        const auto& lg = W.groups[std::max(last_gi, 0)];
        if (last_gi >= 0 && W.round - last_round <= 15 && !(lg.last_seen == W.round && !lg.awake) && !lg.reward && lg.respawn_at <= W.round) {
            const auto mt = mono_tiles(last_gi);
            if (min_dist(p, mt) <= 5) { g_prio = 100 + static_cast<int>(me.health() / 25.f), g_prio_round = W.round; get_out(mt); return "mout"; }
        }
        if constexpr (MONO_DENY) return mono::deny(enemies, target);
        return nullptr;
    }
    auto& g = W.groups[gi];
    if (g.reward || g.respawn_at > W.round || (W.boss_map && me.gem_count() >= 100)) return nullptr; // capture / boss_bag
    last_gi = gi, last_round = W.round;
    target = gi;
    const auto mt = mono_tiles(gi);
    const int d = min_dist(p, mt);
    bool enemy = false;
    for (const auto& e : enemies) enemy |= cheb(p, e.position()) <= 8;
    // the last one at the monolith walks out alone under fire: it leaves while it still has the hits for that
    bool alone = d <= 2;
    for (int i = 0; i < W.n_allies && i < 6 && alone; ++i) {
        if (i == W.me_idx || !W.alive[i] || W.slot_away(i)) continue;
        bc::detail::champion_info ci{};
        if (bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(W.ally_h[i])), &ci) == 0 && ci.x >= 0
            && min_dist({ci.x, ci.y}, mt) <= 2) alone = false;
    }
    const int nm = live_monos(g);
    // Cassi's death takes Issac too (k3 debug: 01boss-battle C&I died together in 3 of 4 games): she keeps 60%
    const bool cassi = me.champion_name() == bc::champion_type::cassi;
    const float res = std::max(reserve(me.max_health(), nm), cassi ? 0.6f * me.max_health() : 0.f);
    if (me.health() < (alone && g.awake ? std::max(res, (ESC + 1.f) * HIT * static_cast<float>(nm)) : res)) healing = true;
    if (healing && me.health() >= 0.95f * me.max_health()) healing = false;
    // turn priority by hp everywhere in the take: of those nearest the monolith the healthiest takes its hit (walking out too)
    g_prio = 100 + static_cast<int>(me.health() / 25.f), g_prio_round = W.round;
    const bool awake = g.awake && g.last_seen == W.round;
    // busy with another camp (bb3 debug: Issac recalled next to an awake elk at 75% and died recalling): finish that first
    if (d > TAKE_STAGE + 1 && camp_threat(p, gi)) return nullptr;
    if (healing || enemy) {
        W.set_away();
        // the healthiest at 1 takes the hits (priority by hp), so a hurt one is safe from 2 out while others hold 1
        if (d <= 5 && (g.awake || g.last_seen < W.round) && get_out(mt)) return "mout";
        if (enemy) return nullptr;
        if ((!awake || d > 5) && !camp_threat(p, gi)) try { me.recall(); return "mrecall"; } catch (...) {}
        step_field(W.home_field());
        return "mhome";
    }
    W.my_arrived = d <= TAKE_STAGE + 1;
    W.set_claim(gi);
    std::vector<bc::camp> live, ms;
    for (const auto& c : W.camps) if (g.spawns.is_set(c.spawn_position())) { live.push_back(c); if (c.is_monolith()) ms.push_back(c); }
    if (!awake) {
        if (!W.boss_map && d > TAKE_STAGE + 1 && me.gem_count() >= 20) return nullptr; // bank the bag first (normal flow)
        // heal before going in; then stage at 6 until the team is there
        if (me.health() < 0.8f * me.max_health() && d > TAKE_STAGE + 1 && !camp_threat(p, -1)) {
            W.set_away();
            try { me.recall(); return "mheal"; } catch (...) {}
        }
        int active = 1;
        for (int i = 0; i < W.n_allies && i < 6; ++i) active += i != W.me_idx && W.alive[i] && !W.slot_away(i);
        const int ready = (d <= TAKE_STAGE + 1) + W.arrived_claims(gi);
        if (d <= TAKE_STAGE + 1 && stage_since < 0) stage_since = W.round;
        const bool go = ready >= active || (stage_since >= 0 && W.round - stage_since >= GO_WAIT && ready >= std::max(2, active - 1));
        if (!go) {
            go_since = -1;
            move_ring(gi, mt, TAKE_STAGE);
            debug_block({ std::cout << "MSTAGE R" << W.round << " i" << W.me_idx << " d=" << d << " ready=" << ready << "/" << active << "\n"; });
            return "mstage";
        }
    }
    // in: everyone adjacent (it never wakes from proximity); the hits go to the healthiest there (priority by hp, ties to
    // the first in turn order), so the damage spreads and the hurt step out behind the others
    // a bag of 30+ stays one back (boss-battle: Ryth carried 450 unbankable gems into the adjacent tiles and died with them);
    // a ranged champion that 3 rounds of hits would kill, and Cassi, shoot from their range (never the nearest)
    const bool fragile = me.attack_range() > 1 && (cassi || me.max_health() < 3.f * HIT * static_cast<float>(nm) + 50.f);
    const int want = fragile ? std::max(2, static_cast<int>(me.attack_range())) : me.gem_count() >= 30 && !seal_take(gi) ? 2 : 1;
    const bool moved = move_ring(gi, mt, want);
    const vec2 q = me.position();
    const int dq = min_dist(q, mt);
    if (!awake) {
        // nobody wakes it before the team is adjacent (or GO_IN rounds after the go)
        if (go_since < 0) go_since = W.round;
        int in = dq <= want, active = 1; // carriers stand one back, the fragile at 3; 01boss-battle's pair has 2 adjacent tiles
        for (int i = 0; i < W.n_allies && i < 6; ++i) {
            if (i == W.me_idx || !W.alive[i] || W.slot_away(i)) continue;
            ++active;
            bc::detail::champion_info ci{};
            if (bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(W.ally_h[i])), &ci) == 0 && ci.x >= 0)
                in += min_dist({ci.x, ci.y}, mt) <= 3;
        }
        if (in < active && W.round - go_since < GO_IN) return "mgo";
    }
    bool top = dq <= 1; // likely the target next round: no ally at 1 with more hp seen
    for (const auto& a : W.ally_h) {
        bc::detail::champion_info ci{};
        if (a != me && bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(a)), &ci) == 0 && ci.x >= 0
            && ci.health > me.health() && min_dist({ci.x, ci.y}, mt) <= 1) top = false;
    }
    spells(ms, moved, top);
    if (dq <= me.attack_range()) {
        attack_camp(live);
        try_reset_and_attack([&] { return attack_camp(live); });
    }
    debug_block({ std::cout << "MFIGHT R" << W.round << " i" << W.me_idx << " d=" << dq << " hp=" << (int)me.health() << " mh=" << (int)g.hp_seen << " aw=" << awake << " top=" << top << "\n"; });
    return awake ? "mfight" : "mgo";
}
