// Boss maps (contest: the only gems are two 20000-hp monoliths of 200 gems, altars only in the centre).
// A 200-gem bag channels for 20 turns at the central altar; walked there alone it was stolen in 12 of 25 local
// captures and in all 5 platform captures against one top team (which banked 1000 that way without touching a monolith).
// So the carrier never walks alone: it brings the bag home and calls a rally (claim RALLY); allies come home,
// and once 2 of them stand with it the party walks to the altar together (claim ESCORT: allies hold the altar).
#include "bot.h"
#include "zreach.h"
#include "actions.h"
#include "world.h"
#include "pvp.h"
#include <algorithm>
#include <iostream>

namespace {
constexpr int BAG = 100;          // bags this big wait for an escort
constexpr int RALLY = 61, ESCORT = 60; // carrier claim values (group ids no boss map uses)
constexpr int GUARD = 59;              // ally answering a call; arrived bit = next to the carrier / at the altar / home
constexpr int RALLY_MAX = 60;     // rounds a carrier waits at spawn for escorts before going anyway
constexpr int LATE = 1880;        // past this the bag must be banked: go now

int rally_since = -1, stall = 0;
bool escorted = false, left_spawn = false;

int called(int code) {
    for (int s = 0; s < W.n_allies && s < 6; ++s)
        if (s != W.me_idx && W.slot_alive(s) && W.claim_of(s) == code) return s;
    return -1;
}
} // namespace

const char* Bot::boss_bag(int gems, bool at_spawn, bool safe, const std::vector<bc::champion>& allies) {
    if (!W.boss_map || gems < BAG) { rally_since = -1; stall = 0; escorted = left_spawn = false; return nullptr; }
    auto& me = bc::self();
    const vec2 p = me.position();
    if (at_spawn && escorted && left_spawn) { escorted = left_spawn = false; rally_since = W.round; } // pushed back home
    if (!at_spawn && escorted) left_spawn = true;
    int near = 0;
    for (const auto& a : allies) if (cheb(a.position(), p) <= 3) ++near;
    // bushes hide adjacent allies (a carrier at the altar saw 0 of 4 guards next to it): trust their arrived bits too
    near = std::max(near, W.arrived_claims(GUARD));
    const int want = std::min(2, W.alive_count() - 1);
    if (!escorted) {
        if (at_spawn) {
            if (rally_since < 0) rally_since = W.round;
            escorted = near >= want || W.round - rally_since >= RALLY_MAX || W.round >= LATE;
        } else {
            // captured with the siege party around: go straight on together
            escorted = (near >= want && W.round - pvp::last_seen > 5) || W.round >= LATE;
        }
    }
    if (escorted) {
        W.set_claim(ESCORT);
        // walk with the party: hold until a guard is back within 3 (they follow us when they see us)
        const bool at_altar = W.altars_field().get(p) == 0;
        if (!at_altar && near == 0 && W.alive_count() > 1 && W.round < LATE && W.round - rally_since < 2 * RALLY_MAX) {
            cast_support(false);
            return "escortwait";
        }
        // a declined fight masks the tiles in enemy reach: an enemy parked at the altar froze a carrier for 1700
        // rounds. After 10 frozen turns go home and rally again; past LATE walk through.
        if (W.round >= LATE) pvp::avoid_on = false;
        const vec2 before = p;
        tribute();
        stall = (me.position() == before && !me.is_submitting_gems()) ? stall + 1 : 0;
        if (stall > 10) { escorted = left_spawn = false; stall = 0; rally_since = -1; }
        return "escort";
    }
    W.set_claim(RALLY);
    if (at_spawn) { cast_support(false); return "rally"; }
    if (safe && W.round - pvp::last_seen > 3) try { me.recall(); return "rallyrecall"; } catch (...) {}
    step_field(W.home_field());
    return "rallyhome";
}

const char* Bot::boss_escort(bool at_spawn, bool safe) {
    if (!W.boss_map) return nullptr;
    auto& me = bc::self();
    if (me.gem_count() >= BAG) return nullptr;
    if (const int s = called(ESCORT); s >= 0) {
        // follow the carrier while we see it (a guard ahead of it walked alone into the enemy at the altar)
        const auto& c = W.ally_h[s];
        const auto& af = W.altars_field();
        const bool seen = c.is_visible();
        W.my_arrived = (seen && cheb(me.position(), c.position()) <= 3) || af.get(me.position()) <= 1;
        W.set_claim(GUARD);
        if (seen) {
            const vec2 cp = c.position();
            if (cheb(me.position(), cp) > 2) { if (!approach(cp, 1)) step_field(af); }
            else cast_support(false);
            return "guard";
        }
        if (af.get(me.position()) > 1) { if (!dash_field(af)) step_field(af); }
        else cast_support(false);
        return "guard";
    }
    if (called(RALLY) >= 0) {
        W.my_arrived = at_spawn;
        W.set_claim(GUARD);
        if (at_spawn) { cast_support(false); return "gather"; }
        if (safe && W.home_field().get(me.position()) > 12 && W.round - pvp::last_seen > 3) try { me.recall(); return "gatherrecall"; } catch (...) {}
        step_field(W.home_field());
        return "gatherhome";
    }
    return nullptr;
}

// y-contest MONO_R: time_of's contest filter. rr-c1's rule (enemy-half gold groups) plus monoliths before MONO_R, unless one is a
// reward or its live hp is under 30%. Out of line (another translation unit) so the other maps run rr-c1's instructions.
bool boss_skip(const Group& g) {
    if (g.enemy_side && !g.monolith) return true;
    if constexpr (MONO_NOSIEGE) return g.monolith && !g.reward; // p-fix F2: no siege, so no MONO_R window either
    return g.monolith && !g.reward && W.round < MONO_R && !(g.hp_seen >= 0 && g.hp_seen < 0.3f * g.max_hp);
}

// y-contest MONO_SAFE gate for a monolith reward gi near me, not under me: 2 = an ally is on its tile (capture started:
// rr-c1's rule), 0 = safe to start, 1 = not safe. Safe = 2 allies within 4 of its tiles (as the escort rule: min(2, alive - 1))
// and no visible enemy champion within 7 of its tiles or of me. Allies are the visible ones (no bush near either monolith).
int Bot::mono_gate(int gi, const std::vector<bc::champion>& allies) const {
    if (!W.boss_map) return 2;
    const auto& g = W.groups[gi];
    const vec2 p = bc::self().position();
    int near = 0;
    for (const auto& a : allies) {
        if (g.spawns.is_set(a.position())) return 2;
        near += min_dist(a.position(), g.tiles) <= 4;
    }
    bool enemy = false;
    if (turn_enemies)
        for (const auto& e : *turn_enemies) enemy |= cheb(p, e.position()) <= 7 || min_dist(e.position(), g.tiles) <= 7;
    const int gate = !enemy && near >= std::min(2, W.alive_count() - 1) ? 0 : 1;
    debug_block({ std::cout << "MONOSAFE R" << W.round << " i" << W.me_idx << " g" << gi << " near=" << near << " enemy=" << enemy << " gate=" << gate << "\n"; });
    return gate;
}

// y-contest MONO_SAFE: waiting at reward gi (consecutive rounds) has lasted under MONO_HOLD rounds
bool Bot::mono_hold(int gi) {
    static int hold_gi = -1, hold_from = 0, hold_last = -10;
    if (hold_gi != gi || hold_last < W.round - 1) hold_gi = gi, hold_from = W.round;
    hold_last = W.round;
    debug_block({ std::cout << "MONOHOLD R" << W.round << " i" << W.me_idx << " g" << gi << " for=" << W.round - hold_from << "\n"; });
    return W.round - hold_from < MONO_HOLD;
}
