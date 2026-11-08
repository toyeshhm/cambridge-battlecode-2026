// z-boss: sealed-altar maps (bot.h SEAL_*). Geometry only: an altar is sealed when every standable tile within submit
// range of it is a monolith spawn tile; a map is sealed when every altar is.
#include "bot.h"
#include "world.h"
#include <algorithm>
#include <cstdint>
#include <iostream>

namespace {
int n_tiles;           // sealing tiles found (group, member), up to 8
int s_gi[8], s_k[8];
bool seen_open;        // my last look at a sealing tile found it open
int seen_bank;         // gems banked by both teams at that look
std::uint64_t behind;  // SEAL_NOPARK: groups reachable from our spawn only through the seal's tiles (bit = group index)
bool behind_due;       // SEAL_NOPARK: find_behind waits for a turn end with BEHIND_FUEL left
// judge (j-boss): find_behind costs 13.2-13.5k on 01boss_battle and ~26k on a sealed 00lanes (90x30, 42 groups); run inside
// seal_detect's 10k guard it suspended 5 of 6 champions a round on that map (vis::init_step leaves ~15-20k at turn end).
// ponytail: flat bound (a 100x100 BFS alone is up to 25k); a per-map estimate if a bigger sealed map shows up.
constexpr int BEHIND_FUEL = 40000;
constexpr int NEVER = 1 << 30;

// Groups fields reach only through the seal (camp tiles are no walls): 01boss_battle's halves meet at the monolith pair
// alone, and with no trip to park on, debug vs idle had Varg and Cassi stand at (23,19) for 600+ rounds on enemy-half
// targets (390-425 gold each of 43530). One BFS from our spawn with the seal's tiles as walls: 13k+ fuel, sealed maps only.
void find_behind() {
    bc::bitboard walls = W.static_walls, scratch;
    for (int i = 0; i < n_tiles; ++i) for (auto t : W.groups[s_gi[i]].tiles) walls.set(t);
    bc::distance_field f;
    f.compute(walls, bc::map::spawn_region(team::ally), scratch);
    for (int gi = 0; gi < static_cast<int>(W.groups.size()) && gi < 64; ++gi) {
        bool reach = false, seal = false;
        for (int i = 0; i < n_tiles; ++i) seal |= s_gi[i] == gi;
        for (auto t : W.groups[gi].tiles) if (f.get(t) != bc::distance_field::unreachable) { reach = true; break; }
        if (!reach && !seal) behind |= std::uint64_t{1} << gi;
    }
    debug_block({ std::cout << "SEALBEHIND R" << W.round << " i" << W.me_idx << " behind=" << std::hex << behind << std::dec << "\n"; });
}
} // namespace

// No allocation and no host call but is_in_bounds/is_set: 0.7k fuel on 01boss_battle (one altar). A sealed altar scans all 9
// tiles (~600 fuel each altar: 10.7k on a sealed 18-altar 00lanes), hence the guard grows with the altar count.
void seal_detect() {
    if (g_seal || W.ally_h.empty() || bc::fuel_remaining() < 10000 + 700 * static_cast<int>(W.altars.size())) return;
    const int r = static_cast<int>(bc::constants::submit_gems_range);
    int sealed = 0;
    for (auto a : W.altars) {
        bool open = false;
        for (int dx = -r; dx <= r && !open; ++dx)
            for (int dy = -r; dy <= r && !open; ++dy) {
                const vec2 q{a.x + dx, a.y + dy};
                if (!bc::map::is_in_bounds(q) || W.static_walls.is_set(q)) continue;
                const int gi = W.group_of_spawn(q);
                if (gi < 0 || !W.groups[gi].monolith) { open = true; continue; }
                for (size_t k = 0; k < W.groups[gi].tiles.size() && n_tiles < 8; ++k)
                    if (W.groups[gi].tiles[k] == q) s_gi[n_tiles] = gi, s_k[n_tiles++] = static_cast<int>(k);
            }
        sealed += !open; // ponytail: an altar with no standable tile at all counts as sealed too (nothing opens it)
    }
    g_seal = !W.altars.empty() && sealed == static_cast<int>(W.altars.size()) ? 2 : 1;
    behind_due = SEAL_NOPARK && g_seal == 2;
    debug_block({ if (g_seal == 2) std::cout << "SEAL i" << W.me_idx << " tiles=" << n_tiles << " g" << seal_group() << "\n"; });
}

int seal_group() { return n_tiles > 0 ? s_gi[0] : -1; }

bool seal_closed() {
    if (g_seal != 2) return false;
    const int bank = static_cast<int>(bc::global_submitted_gems(team::ally) + bc::global_submitted_gems(team::enemy));
    bool looked = false, open = false;
    for (int i = 0; i < n_tiles; ++i) {
        const auto& g = W.groups[s_gi[i]];
        if (g.last_seen != W.round) continue;
        looked = true;
        // respawn_at != 0 on a fresh look = respawning (tiles free; S0 while someone stands on one)
        open |= g.reward || g.respawn_at != 0 || (g.hp_member[s_k[i]] <= 0 && !g.awake);
    }
    if (looked) seen_open = open, seen_bank = bank;
    return !seen_open && bank <= seen_bank;
}

// SEAL_SIEGE, at the end of my turn (shared bytes 6/7 are read at every turn start). Before SEAL_R the seal is skipped (time_of),
// so no squad wakes the pair. From SEAL_R, while I believe it alive, both squad targets are the seal with squad 0's help flag:
// the leaders keep a whole-team job (choose_target: help && valid(cur, all)) and squad 1 follows the flag; range-4 casters
// siege it from the ring, the rest take side jobs within SIDE_RANGE. A dead seal (reward, respawning) is left to capture().
void seal_turn_end() {
    if (W.ally_h.empty()) return;
    if (behind_due && bc::fuel_remaining() > BEHIND_FUEL) behind_due = false, find_behind();
    for (int gi = 0; gi < static_cast<int>(W.groups.size()) && gi < 64; ++gi)
        if (behind >> gi & 1) W.groups[gi].skip_until = NEVER; // SEAL_NOPARK: never a target (time_of skips it)
    if (!SEAL_SIEGE || n_tiles == 0) return;
    auto& g = W.groups[s_gi[0]];
    if (W.round < SEAL_R) { g.skip_until = std::max(g.skip_until, SEAL_R); return; }
    if (g.reward || g.respawn_at > W.round) return;
    const int v = s_gi[0] + 1;
    W.write_sm(6, static_cast<std::uint8_t>((v & 0x3f) | (W.sm[6] & 0x40) | 0x80));
    W.write_sm(7, static_cast<std::uint8_t>((v & 0x3f) | (W.sm[7] & 0xc0)));
}
