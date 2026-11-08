// Wards (port of bots/p-vision; its altar-lock buying was measured harmful here and removed, see CHANGES.md).
// Platform data (144 high-ranked games): 87-100% of wards sit
// on bush clusters of 4+ tiles next to camps (<=3 best, <=8), rarely on the enemy side, placed at reach 2 as
// the champion passes; a champion re-wards when it leaves its ward's vision.
#include "vision.h"
#include "actions.h"
#include "zreach.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>

namespace vis {
namespace {

constexpr bool WARDS = true;
constexpr bool SCOUT = true;                  // W3 scout_altar
constexpr int SCOUT_ALTAR = 8, SCOUT_BUSH = 4; // carrier within SCOUT_ALTAR of an altar wards unseen bush within SCOUT_BUSH of it
constexpr int RE = 4;     // re-ward the same cluster once we are this far from the ward (platform replays: median 4, every ~2 rounds)

bool g_ready = false;
int g_w = 0, g_stage = 0;
std::vector<std::uint8_t> g_cluster; // tile -> bush cluster id, 255 elsewhere
std::vector<float> g_score;          // per cluster ward value, 0 = never ward there
std::vector<char> g_near_alt;        // per cluster: some tile within 8 of an altar
// init state (resumable across turns)
bc::bitboard g_left, g_nonbush, g_near3, g_near8, g_alt8, g_comp, g_tmp;
std::vector<vec2> g_buf;
vec2 g_home, g_away;

int cluster_at(vec2 q) {
    if (!g_ready || !bc::map::is_in_bounds(q)) return -1;
    const int c = g_cluster[static_cast<size_t>(q.y) * g_w + q.x];
    return c == 255 ? -1 : c;
}

vec2 centre(const bc::bitboard& b) {
    long sx = 0, sy = 0, n = 0;
    for (auto q : b.positions()) { sx += q.x; sy += q.y; ++n; }
    return n ? vec2{static_cast<int>(sx / n), static_cast<int>(sy / n)} : vec2{0, 0};
}

// 0 = at our spawn, 1 = at the enemy spawn
float side_of(vec2 q, vec2 home, vec2 away) {
    const float a = static_cast<float>(cheb(q, home)), b = static_cast<float>(cheb(q, away));
    return a + b > 0 ? a / (a + b) : 0.5f;
}

} // namespace

bool ready() { return g_ready; }

// A per-tile flood fill took ~650 fuel/tile (stampede, 456 bush tiles: 3 whole turns). Everything here is
// bitboard host calls instead, one cluster at a time (~5k each) while the turn has fuel to spare.
void init_step() {
    if (g_ready) return;
    if (g_stage == 0) {
        const int w = bc::map::width(), h = bc::map::height();
        g_w = w;
        g_cluster.assign(static_cast<size_t>(w) * h, 255);
        const auto& bush = bc::map::bushes();
        g_left = bush;
        g_nonbush = ~bush;
        g_buf.resize(std::max<size_t>(1, bush.count()));
        g_near3.clear();
        g_near8.clear();
        for (const auto& g : W.groups)
            for (auto t : g.tiles) { g_near3.draw_chebyshev(t, 3); g_near8.draw_chebyshev(t, 8); }
        g_alt8.clear();
        for (auto a : W.altars) g_alt8.draw_chebyshev(a, 8);
        g_home = centre(bc::map::spawn_region(team::ally));
        g_away = centre(bc::map::spawn_region(team::enemy));
        g_stage = 1;
        return;
    }
    while (bc::fuel_remaining() > 20000) {
        if (g_left.is_empty() || g_score.size() >= 254) { g_ready = true; break; }
        vec2 seed{};
        (void)g_left.positions_into(std::span<vec2>(&seed, 1));
        g_nonbush.reachable_tiles_into(seed, 255, g_comp); // the 8-connected bush cluster of seed
        const auto n = std::min<std::uint32_t>(g_comp.positions_into(std::span<vec2>(g_buf)), static_cast<std::uint32_t>(g_buf.size()));
        const auto id = static_cast<std::uint8_t>(g_score.size());
        long sx = 0, sy = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const vec2 q = g_buf[i];
            g_cluster[static_cast<size_t>(q.y) * g_w + q.x] = id;
            sx += q.x; sy += q.y;
        }
        auto touches = [&](const bc::bitboard& b) { g_tmp = g_comp; g_tmp &= b; return !g_tmp.is_empty(); };
        const int size = static_cast<int>(std::max<std::uint32_t>(1, n));
        const float side = side_of({static_cast<int>(sx / size), static_cast<int>(sy / size)}, g_home, g_away);
        float sc = 0;
        const bool near_alt = touches(g_alt8);
        if (side <= 0.6f) { // never ward on the enemy side (3-5% of top-team wards)
            sc = size >= 4 ? 2.f : 1.f;
            sc *= touches(g_near3) ? 3.f : touches(g_near8) ? 2.f : 0.5f; // nearest camp tile <=3 / <=8 / farther
            if (near_alt) sc *= 1.5f;
            if (side >= 0.4f) sc *= 1.2f; // middle clusters are warded most (81% vs 69% own side)
        }
        g_score.push_back(sc);
        g_near_alt.push_back(near_alt);
        g_tmp = ~g_comp;
        g_left &= g_tmp;
    }
    debug_block({ if (g_ready) std::cout << "vis::init clusters=" << g_score.size() << " R" << bc::round_number() << "\n"; });
}

void ward(bool tribute) {
    if (!WARDS || !g_ready) return;
    auto& me = bc::self();
    try {
        if (me.ward_cooldown() > 0) return;
        // p-fix F9 WARD_KEEP (cooldown 10, 1 charge): a carrier keeps the charge for reveal() / scout_altar()
        if (WARD_KEEP && me.gem_count() >= 20) return;
        const vec2 p = me.position();
        const auto wp = me.ward_positions();
        const int cur = wp.empty() ? -1 : cluster_at(wp[0]);
        const int dw = wp.empty() ? 99 : cheb(p, wp[0]);
        if (WARD_KEEP && dw <= 6) return; // the current ward still covers us: no follow re-ward
        std::optional<bc::bitboard> seen; // WARD_KEEP: only a cluster nobody sees into yet
        // W2: carrying a bag, the clusters next to an altar count double (tributes under our own ward)
        auto val = [&](int c) { return g_score[c] * (tribute && g_near_alt[c] ? 2.f : 1.f); };
        // the current ward still covers a scored cluster near us: another cluster must be much better
        const float keep = cur >= 0 && g_score[cur] >= 1.f && dw <= 6 ? 1.5f * val(cur) : 0.f;
        float best = 0;
        vec2 bq = p;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const vec2 q{p.x + dx, p.y + dy};
                const int c = cluster_at(q);
                if (c < 0 || g_score[c] < 1.f) continue;
                const float s = val(c) + 0.01f * cheb(p, q); // reach 2
                // same cluster: follow us through it once we are RE away (the whole bush stays seen either way;
                // the radius-6 view around the ward moves with us)
                if (c == cur ? dw < RE || cheb(q, wp[0]) < RE : s <= keep) continue;
                if (WARD_KEEP && (c == cur || (seen ? *seen : *(seen = bc::vision::visible_tiles())).is_set(q))) continue;
                if (s > best) { best = s; bq = q; }
            }
        if (best > 0) me.place_ward(bq);
    } catch (...) {}
}

bool scout_altar() {
    if (!SCOUT) return false;
    auto& me = bc::self();
    try {
        if (me.ward_cooldown() > 0 || me.gem_count() < 20) return false;
        const vec2 p = me.position();
        const auto& bush = bc::map::bushes();
        bc::bitboard seen;
        bool have = false;
        for (auto a : W.altars) {
            if (cheb(p, a) > SCOUT_ALTAR) continue;
            if (!have) seen = bc::vision::visible_tiles(), have = true;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const vec2 q{p.x + dx, p.y + dy};
                    if (bc::map::is_in_bounds(q) && cheb(q, a) <= SCOUT_BUSH && bush.is_set(q) && !seen.is_set(q) && me.place_ward(q)) return true;
                }
        }
    } catch (...) {}
    return false;
}

int enemy_lock_hits(vec2 a) {
    try {
        if (!bc::vision::has_altar_lock(a)) return 0;
        const auto l = bc::vision::altar_lock_at(a);
        return l.is_enemy() ? static_cast<int>(l.hits_remaining()) : 0;
    } catch (...) { return 0; }
}

} // namespace vis
