// p-monolith MONO_DENY (mono.h): an enemy take pulls the team in (kills near a pinned tank).
#include "bot.h"
#include "actions.h"
#include "world.h"
#include <iostream>
using mono::mono_tiles;
using mono::enemy_near;
using mono::move_ring;

// MONO_DENY: an enemy team on a monolith (awake, an enemy within its range 5: adjacent takers and the old ring-4 siegers alike)
// pulls us there: their tank is low and pinned, and a
// kill near it is 100 gold. The one who sees it claims the group (claims never name a monolith otherwise, MONO_NOSIEGE);
// allies seeing such a claim join once (DENY_JOIN rounds). We walk in until an enemy is within 7; pvp::turn fights from there.
namespace {
constexpr int DENY_SEEN = 20, DENY_JOIN = 25;
int deny_gi = -1, deny_until = 0, join_round = -100;
} // namespace

const char* mono::deny(const std::vector<bc::champion>& enemies, int& target) {
    auto& me = bc::self();
    for (int gi = 0; gi < static_cast<int>(W.groups.size()); ++gi) {
        const auto& g = W.groups[gi];
        if (g.monolith && g.last_seen == W.round && g.awake && !g.reward && g.hp_seen >= 0 && g.hp_seen < g.max_hp && enemy_near(mono_tiles(gi), 5))
            deny_gi = gi, deny_until = W.round + DENY_SEEN;
    }
    if (deny_until <= W.round && W.round - join_round > 2 * DENY_JOIN)
        for (int i = 0; i < W.n_allies && i < 6; ++i) {
            const int c = i == W.me_idx || !W.alive[i] ? -1 : W.claim_of(i);
            if (c >= 0 && c < static_cast<int>(W.groups.size()) && W.groups[c].monolith && !W.groups[c].reward) {
                deny_gi = c, deny_until = W.round + DENY_JOIN, join_round = W.round;
                break;
            }
        }
    if (deny_gi < 0 || deny_until <= W.round || me.health() < 0.6f * me.max_health() || (W.boss_map && me.gem_count() >= 100)) return nullptr;
    const auto& g = W.groups[deny_gi];
    if (g.reward || g.respawn_at > W.round) { deny_until = 0; return nullptr; } // dead: capture() / farm take it from here
    for (const auto& e : enemies) if (cheb(me.position(), e.position()) <= 7) return nullptr;
    const auto mt = mono_tiles(deny_gi);
    // at its stage with no enemy in sight: they are gone (dh1 debug: 1495 champion-turns stood at 6 waiting); drop it
    if (min_dist(me.position(), mt) <= TAKE_STAGE + 1 && !enemy_near(mt, 9)) { deny_until = 0; return nullptr; }
    target = -1;
    W.set_claim(deny_gi);
    move_ring(deny_gi, mt, TAKE_STAGE);
    debug_block({ std::cout << "MDENY R" << W.round << " i" << W.me_idx << " g" << deny_gi << " d=" << min_dist(me.position(), mt) << "\n"; });
    return "mdeny";
}

