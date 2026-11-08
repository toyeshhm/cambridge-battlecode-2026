#include "world.h"
#include "zreach.h"
#include "prof.h"
#include "heart.h"
#include "draft.h"
#include <algorithm>
#include <cmath>
#include <variant>
#include <iostream>

World W;

// p-fix F6 CAMP_STATS: engine hp / str per kind, copied once at the top of World::init (a call per lookup cost 28k fuel of
// 01boss-battle's first turn). rows' elk 60.f test in bot.cpp still holds: engine elk str 60.
static float s_hp[6], s_str[6];
static float camp_hp(bc::camp_type t) {
    if constexpr (CAMP_STATS) { const auto i = static_cast<unsigned>(t); return i < 6 ? s_hp[i] : 500.f; }
    switch (t) {
    case bc::camp_type::wolf: return 1350;
    case bc::camp_type::elk: return 2400;
    case bc::camp_type::monolith: return 20000;
    default: return 500;
    }
}
static float camp_str(bc::camp_type t) {
    if constexpr (CAMP_STATS) { const auto i = static_cast<unsigned>(t); return i < 6 ? s_str[i] : 0.f; }
    switch (t) {
    case bc::camp_type::wolf: return 20;
    case bc::camp_type::elk: return 60;
    case bc::camp_type::monolith: return 200;
    default: return 0;
    }
}

// Camp damage per champion-round at base stats, measured on platform replays before r100 (notes/openings.md 4.6).
static float camp_dps_of(bc::champion_type t) {
    if constexpr (META_KIT) { if (t == bc::champion_type::vendetta) return META_VEN; if (t == bc::champion_type::marcella) return META_MAR;
        if (t == bc::champion_type::vesper_vale) return META_VES; } // META (draft.h)
    switch (t) {
    case bc::champion_type::ren: return 60;
    case bc::champion_type::issac: return 50;
    case bc::champion_type::cassi: return 58;
    case bc::champion_type::ryth: return 90;
    case bc::champion_type::varg: return 90;
    case bc::champion_type::stellar: return 120;
    case bc::champion_type::tavi: case bc::champion_type::prism: return 100; // ponytail: casters like Stellar, unmeasured
    default: return 60;
    }
}

void World::init() {
    if constexpr (CAMP_STATS)
        for (int i = 0; i < 6; ++i) s_hp[i] = camp_stat(static_cast<bc::camp_type>(i)).hp, s_str[i] = camp_stat(static_cast<bc::camp_type>(i)).str;
    const auto& me = bc::self();
    ally_h = bc::champions(team::ally);
    enemy_h = bc::champions(team::enemy);
    n_allies = static_cast<int>(ally_h.size());
    for (int i = 0; i < n_allies; ++i)
        if (ally_h[i] == me) me_idx = i;
    PROF("i.allies");
    for (int i = 0; i < n_allies && i < 8; ++i) {
        // damage per round from abilities castable at distance 4 (monster range is 3), from the champion design
        const auto& d = bc::champion_designs[static_cast<size_t>(ally_h[i].champion_name())];
        const float focus = d.base_stats().base_focus;
        for (const auto& a : d.abilities()) {
            if (!a.info()) continue;
            std::visit([&](const auto& x) {
                using X = std::decay_t<decltype(x)>;
                float hits = 0;
                if constexpr (std::is_same_v<X, bc::ability_damage> || std::is_same_v<X, bc::ability_cc>) hits = 1;
                if constexpr (std::is_same_v<X, bc::ability_aoe_damage>) hits = 2;
                if constexpr (requires { x.range; x.modifier; })
                    if (x.range >= 4 && x.max_cooldown > 0) siege_dps[i] += hits * x.modifier * focus / x.max_cooldown;
            }, *a.info());
        }
    }
    {
        std::vector<float> hps;
        float sum = 0;
        for (int i = 0; i < n_allies && i < 8; ++i) {
            const auto t = ally_h[i].champion_name();
            camp_dps[i] = camp_dps_of(t);
            base_hp[i] = bc::champion_designs[static_cast<size_t>(t)].base_stats().base_health;
            sum += camp_dps[i];
            hps.push_back(base_hp[i]);
        }
        team_dps = sum / std::max(1, std::min(n_allies, 8));
        std::sort(hps.rbegin(), hps.rend());
        top_hp = {hps.empty() ? 900.f : hps[0], hps.size() > 1 ? hps[1] : 0.f};
    }
    PROF("i.siege");
    static_walls = bc::map::walls() | bc::map::holes() | bc::map::altars() | bc::map::spawn_region(team::enemy);
    altars = bc::map::altars().positions();

    PROF("i.walls");
    const auto cgs = bc::map::camp_groups();
    PROF("i.cgs");
    tile_group.assign(static_cast<size_t>(bc::map::width()) * bc::map::height(), -1);
    groups.reserve(cgs.size());
    for (const auto& cg : cgs) {
        Group g;
        g.gems = static_cast<int>(cg.gem_value());
        g.gold = static_cast<int>(cg.gold_value());
        g.respawn = cg.max_respawn_timer();
        if (!R1_ACT) g.spawns.clear(); // r-round1: the bitboard ctor already zeroes it (189 fuel a group on the first turn)
        const auto ms = cg.camps();
        g.members.assign(ms.begin(), ms.end());
        if (!ms.empty()) bc::detail::sdk_map_camp_group_of(static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(ms[0])), &g.handle);
        g.tiles.reserve(ms.size()), g.member_hp.reserve(ms.size()), g.member_str.reserve(ms.size());
        g.hp_member.reserve(ms.size()), g.by_str.reserve(ms.size());
        long sx = 0, sy = 0;
        for (const auto& c : ms) {
            const auto p = c.spawn_position();
            const auto k = c.kind();
            g.tiles.push_back(p);
            g.spawns.set(p);
            tile_group[static_cast<size_t>(p.y) * bc::map::width() + p.x] = static_cast<std::int8_t>(groups.size());
            sx += p.x; sy += p.y;
            g.member_hp.push_back(camp_hp(k));
            g.member_str.push_back(camp_str(k));
            g.hp_member.push_back(camp_hp(k));
            g.by_str.push_back({camp_hp(k), camp_str(k)});
            g.max_hp += camp_hp(k);
            g.str += camp_str(k);
            if (k == bc::camp_type::monolith) g.monolith = true;
        }
        std::sort(g.by_str.begin(), g.by_str.end(), [](auto x, auto y) { return x.second > y.second; });
        int n = std::max<int>(1, static_cast<int>(g.tiles.size()));
        g.center = {static_cast<int>(sx / n), static_cast<int>(sy / n)};
        g.harmless = g.str <= 0;
        groups.push_back(std::move(g));
    }
    PROF("i.groups");
    // Two squads with 5+ bodies, unless our half has at most 3 gem groups (rows: three 66-gem wolf packs; pilgrimage):
    // there a team rotating group to group kills each in a few rounds (recapture every 50-60
    // rounds vs our ~100 with two squads).
    {
        auto centre = [](const bc::bitboard& b) {
            long sx = 0, sy = 0, n = 0;
            for (auto q : b.positions()) { sx += q.x; sy += q.y; ++n; }
            return n ? vec2{static_cast<int>(sx / n), static_cast<int>(sy / n)} : vec2{0, 0};
        };
        const vec2 home = centre(bc::map::spawn_region(team::ally)), away = centre(bc::map::spawn_region(team::enemy));
        auto d2 = [](vec2 a, vec2 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); };
        int ours = 0;
        for (const auto& g : groups)
            if (g.gems > 0 && !g.monolith && d2(g.center, home) < d2(g.center, away)) ++ours;
        if (n_allies >= 5 && ours > 3) {
            n_squads = 2;
            for (int i = 0; i < n_allies && i < 8; ++i) {
                const auto t = ally_h[i].champion_name();
                squad_of[i] = (t == bc::champion_type::cassi || t == bc::champion_type::issac || t == bc::champion_type::varg) ? 1 : 0;
            }
            if constexpr (HOR_PLAY) hor::squad(ally_h, squad_of); // h-heart: HoR tanks for squad 1 while it has < 3 bodies
            if constexpr (ONE_SQUAD) {
                // SQ_FAR_D: only where our nearest gem group is at least that far (ambush 39, lanes 33; every other map <= 15)
                int near = FAR;
                if (SQ_FAR_D > 0)
                    for (const auto& g : groups)
                        if (g.gems > 0 && !g.monolith && d2(g.center, home) < d2(g.center, away)) near = std::min(near, cheb(g.center, home));
                if (near >= SQ_FAR_D && (SQ_MAX_OWN <= 0 || ours <= SQ_MAX_OWN))
                    planned_squads = n_squads, planned_of = squad_of, n_squads = 1, squad_of = {};
            }
        }
    }
    boss_map = std::any_of(groups.begin(), groups.end(), [](const Group& g) { return g.gems > 0; })
        && std::all_of(groups.begin(), groups.end(), [](const Group& g) { return g.gems <= 0 || g.monolith; });
    {
        int total = 0, boss = 0;
        for (const auto& g : groups) { total += g.gems; if (g.monolith || g.str >= 240) boss += g.gems; }
        herd_map = total > 0 && boss >= 0.8f * total;
    }
    place_anchors();
    if constexpr (EXP_SOLO && (Y_BIG || Y_MID || Y_SMALL)) {
        const int area = static_cast<int>(bc::map::width() * bc::map::height());
        const bool band = area >= EXP_AREA ? Y_BIG || n_allies <= EXP_BODIES : area >= Y_MID_AREA ? Y_MID : Y_SMALL;
        if (band && !boss_map && !herd_map) { // a band switched off costs nothing at init (fuel: identical games off-class)
            float mhp = 0;
            for (int i = 0; i < n_allies; ++i) mhp += base_hp[i];
            mhp /= static_cast<float>(std::max(1, n_allies));
            // Own groups by exact centroids, in halves: a group on the midline (equidistant) is shared, half a job per side.
            // g.center and the spawn centres truncate, so enemy_side broke midline ties by side (01corridors: own=11 on
            // side A, 4 on B; MID fired on A only). Where MID/SMALL would fire, only groups walkable from our spawn count
            // (01islands: 14 of 17 own groups sit behind holes; solo_target's walk fields never reach them and 3 of 6
            // champions idled at spawn r25-130). Judge jy-solo, bots/y-solo/CHANGES.md.
            auto mid = [](const std::vector<vec2>& v) {
                double sx = 0, sy = 0;
                for (auto q : v) sx += q.x, sy += q.y;
                const double n = static_cast<double>(std::max<size_t>(1, v.size()));
                return std::pair<double, double>{sx / n, sy / n};
            };
            std::vector<vec2> hv, av;
            for (auto q : bc::map::spawn_region(team::ally).positions()) hv.push_back(q);
            for (auto q : bc::map::spawn_region(team::enemy).positions()) av.push_back(q);
            const auto [hx, hy] = mid(hv);
            const auto [ax, ay] = mid(av);
            int solo = 0, own = 0; // halves
            auto count = [&](bool walk) {
                solo = own = 0;
                for (const auto& g : groups) {
                    if (g.gems <= 0 || g.monolith) continue;
                    const auto [gx, gy] = mid(g.tiles);
                    const double edge = std::max(std::abs(gx - ax), std::abs(gy - ay)) - std::max(std::abs(gx - hx), std::abs(gy - hy));
                    if (edge < -0.01) continue;
                    if (walk && std::none_of(g.tiles.begin(), g.tiles.end(), [&](vec2 t) { return home_field().get(t) < FAR; })) continue;
                    const int w = edge > 0.01 ? 2 : 1;
                    own += w;
                    float rs = g.str, taken = 0;
                    for (auto [hp, st] : g.by_str) if (hp > 0) { taken += rs * hp / team_dps; rs -= st; }
                    if (taken <= 0.75f * mhp) solo += w; // GATE_HP (bot.h)
                }
            };
            auto fires = [&] { return area >= EXP_AREA ? solo >= 2 * EXP_MIN_SOLO && (!Y_BIG_OWN || own >= 2 * n_allies) : area >= Y_MID_AREA ? own >= 2 * n_allies : solo >= 2 * n_allies; };
            count(false);
            if (area < EXP_AREA && fires()) count(true); // the spawn field (fuel) only where MID/SMALL would fire
            solo_map = fires();
            debug_block({ if (me_idx == 0) std::cout << "SOLOMAP solo=" << solo / 2.0 << " own=" << own / 2.0 << " map=" << solo_map << "\n"; });
        }
    } else if constexpr (EXP_SOLO) {
        if (n_allies <= EXP_BODIES && static_cast<int>(bc::map::width() * bc::map::height()) >= EXP_AREA && !boss_map && !herd_map) {
            float mhp = 0;
            for (int i = 0; i < n_allies; ++i) mhp += base_hp[i];
            mhp /= static_cast<float>(std::max(1, n_allies));
            int solo = 0;
            for (const auto& g : groups) {
                if (g.gems <= 0 || g.monolith || g.enemy_side) continue;
                float rs = g.str, taken = 0;
                for (auto [hp, st] : g.by_str) if (hp > 0) { taken += rs * hp / team_dps; rs -= st; }
                solo += taken <= 0.75f * mhp; // GATE_HP (bot.h)
            }
            solo_map = solo >= EXP_MIN_SOLO;
            debug_block({ if (me_idx == 0) std::cout << "SOLOMAP solo=" << solo << " map=" << solo_map << "\n"; });
        }
    }
}

// Squad anchors: the best gem group on our half (value over distance from spawn), and for a second squad
// the best one at least 12 tiles from it. Targets far from the anchor pay for the walk back.
void World::place_anchors() {
    auto centre = [](const bc::bitboard& b) {
        long sx = 0, sy = 0, n = 0;
        for (auto q : b.positions()) { sx += q.x; sy += q.y; ++n; }
        return n ? vec2{static_cast<int>(sx / n), static_cast<int>(sy / n)} : vec2{0, 0};
    };
    const vec2 home = home_c = centre(bc::map::spawn_region(team::ally)), away = away_c = centre(bc::map::spawn_region(team::enemy));
    std::vector<std::pair<float, int>> c;
    for (auto& g : groups) g.enemy_side = cheb(g.center, away) < cheb(g.center, home);
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        const auto& g = groups[i];
        if (g.gems <= 0 || cheb(g.center, home) > cheb(g.center, away)) continue;
        c.push_back({-(g.gems + 0.2f * g.gold) / (cheb(g.center, home) + 20.f), i});
    }
    std::sort(c.begin(), c.end());
    anchor[0] = anchor[1] = c.empty() ? home : groups[c[0].second].center;
    for (auto [s, i] : c)
        if (cheb(groups[i].center, anchor[0]) >= 12) { anchor[1] = groups[i].center; break; }
}

int World::group_of_spawn(vec2 p) const {
    if (!bc::map::is_in_bounds(p)) return -1;
    return tile_group[static_cast<size_t>(p.y) * bc::map::width() + p.x];
}

void World::update() {
    round = static_cast<int>(bc::round_number());
    if constexpr (ONE_SQUAD) if (n_squads < planned_squads) {
        const int t0p = squad_target(0) + 1;
        const bool moved = SPLIT_CAP && round >= SPLIT_MIN && seen_t0p > 0 && t0p != seen_t0p;
        seen_t0p = t0p;
        if (round >= SPLIT_R || moved) {
            n_squads = planned_squads, squad_of = planned_of, split_round = round;
            split_keep = -1;
            debug_block({ std::cout << "SPLIT R" << round << " i" << me_idx << " sq" << squad_of[me_idx] << " t0=" << squad_target(0) << "\n"; });
        }
    }
    const auto visible = bc::vision::visible_tiles();
    std::vector<char> fresh(groups.size(), 0);
    for (size_t i = 0; i < groups.size(); ++i) {
        auto& g = groups[i];
        bool seen = false;
        for (auto t : g.tiles) if (visible.is_set(t)) { seen = true; break; }
        if (!seen) continue;
        // what camp_group::state() returns, without its vision re-check (done above): 1 alive, 2 reward, 3 respawning
        std::uint32_t st = 0;
        std::int32_t timer = 0;
        if (bc::detail::sdk_camp_group_state(g.handle, &st) != 0 || st < 1 || st > 3) continue;
        if (st == 3 && bc::detail::sdk_camp_group_cooldown(g.handle, &timer) != 0) continue;
        g.last_seen = round;
        fresh[i] = 1;
        g.awake = false;
        g.reward = st == 2;
        g.respawn_at = st == 3 ? round + timer : 0;
        if (st != 2) g.est_respawn = 0;
        // Members whose spawn tile is out of sight keep their last value (design max until first seen),
        // so a half-seen group is not mistaken for an almost dead one. Visible tiles without a live camp are dead.
        const bool full = g.reward || g.respawn_at != 0;
        for (size_t k = 0; k < g.tiles.size(); ++k)
            if (full || visible.is_set(g.tiles[k])) g.hp_member[k] = full ? g.member_hp[k] : 0.f;
    }
    // Live visible camps from one raw record per member of the seen, alive groups (camps never leave their spawn
    // tile). vision::camps() + is_visible/health/is_awake cost ~400 per camp (lanes: 42 wolves in view, ~15k).
    camps.clear();
    for (size_t i = 0; i < groups.size(); ++i) {
        auto& g = groups[i];
        if (!fresh[i] || g.reward || g.respawn_at != 0) continue;
        for (size_t k = 0; k < g.members.size(); ++k) {
            const auto h = static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(g.members[k]));
            bc::detail::camp_state cs{};
            if (bc::detail::sdk_camp(h, &cs) != 0 || cs.health <= 0) continue;
            camps.push_back(g.members[k]);
            g.hp_member[k] = cs.health;
            std::uint32_t st = 0;
            if (!g.awake && bc::detail::sdk_camp_state(h, &st) == 0 && st == 1) g.awake = true;
        }
    }
    for (size_t i = 0; i < groups.size(); ++i) {
        if (!fresh[i]) continue;
        auto& g = groups[i];
        if (g.reward || g.respawn_at != 0) { g.hp_seen = -1; continue; }
        g.hp_seen = 0;
        for (float h : g.hp_member) g.hp_seen += h;
    }
}

// Spend spare fuel precomputing static distance fields (~26k fuel each on 100x100). Called at the END of the
// turn (bot.cpp), so a fight turn gets the budget first (an earlier bot did this at the start of update()).
void World::precompute() {
    for (size_t i = 0; i < groups.size() && bc::fuel_remaining() > 70000; ++i)
        if (!groups[i].field_ready) group_field(static_cast<int>(i));
    if (bc::fuel_remaining() > 70000) altars_field();
    if (bc::fuel_remaining() > 70000) home_field();
}

void Field::compute(const bc::bitboard& walls, const bc::bitboard& roots, bc::bitboard& visited) {
    bc::distance_field f;
    f.compute(walls, roots, visited);
    w = bc::map::width(), h = bc::map::height();
    d.reset(new std::uint16_t[static_cast<size_t>(w) * h]); // no zero fill: the export writes every tile
    f.export_linear_buffer(std::span<std::uint16_t>(d.get(), static_cast<size_t>(w) * h));
}

const Field& World::group_field(int gi) {
    auto& g = groups[gi];
    if (!g.field_ready) {
        g.field.compute(static_walls, g.spawns, visited);
        g.field_ready = true;
    }
    return g.field;
}

const Field& World::altars_field() {
    if (!altar_ready) {
        bc::bitboard roots;
        roots.clear();
        for (auto a : altars) roots.draw_chebyshev(a, static_cast<float>(bc::constants::submit_gems_range));
        roots &= ~static_walls;
        altar_field.compute(static_walls, roots, visited);
        altar_ready = true;
        for (auto s : bc::map::spawn_region(team::ally).positions()) spawn_altar = std::min<int>(spawn_altar, altar_field.get(s));
    }
    return altar_field;
}

const Field& World::home_field() {
    if (!spawn_ready) {
        spawn_field.compute(static_walls, bc::map::spawn_region(team::ally), visited);
        spawn_ready = true;
    }
    return spawn_field;
}

std::vector<vec2> World::danger_tiles(int gi) const {
    std::vector<vec2> out;
    const auto& g = groups[gi];
    for (size_t k = 0; k < g.tiles.size(); ++k)
        if (g.member_str[k] > 0 && g.hp_member[k] > 0) out.push_back(g.tiles[k]);
    return out;
}

int World::dist_to_group(int gi, vec2 p) const {
    const auto& g = groups[gi];
    if (g.field_ready) return g.field.get(p);
    int best = FAR;
    for (auto t : g.tiles) best = std::min(best, cheb(p, t) * 5 / 4); // estimate until the field exists
    return best;
}

// Liveness comes from the engine (is_alive works map-wide); shared bytes 0..5 hold each slot's claim.
void World::refresh_alive(bool enemies) {
    const int r = static_cast<int>(bc::round_number());
    for (int i = 0; i < n_allies && i < 6; ++i) {
        const bool was = alive[i];
        try { alive[i] = i == me_idx || ally_h[i].is_alive(); } catch (...) { alive[i] = false; }
        if (was && !alive[i]) ally_died[i] = r;
    }
    if (!enemies) return;
    const bool gap = r - enemy_tracked > 1;
    enemy_tracked = r;
    for (int i = 0; i < static_cast<int>(enemy_h.size()) && i < 8; ++i) {
        const bool was = enemy_alive[i];
        try { enemy_alive[i] = enemy_h[i].is_alive(); } catch (...) { enemy_alive[i] = false; }
        if (was && !enemy_alive[i] && !gap) enemy_died[i] = r;
    }
}

int World::trade(int rounds) const {
    const int r = static_cast<int>(bc::round_number());
    int t = 0;
    for (int i = 0; i < 8; ++i) t += (r - enemy_died[i] <= rounds) - (r - ally_died[i] <= rounds);
    return t;
}

bool World::slot_alive(int i) const { return i >= 0 && i < 6 && alive[i]; }

bool World::is_leader() const {
    for (int i = 0; i < me_idx && i < 6; ++i)
        if (alive[i] && squad_of[i] == squad_of[me_idx] && !slot_away(i)) return false;
    return true;
}

int World::squad_active(int s) const {
    int n = 0;
    for (int i = 0; i < n_allies && i < 6; ++i)
        if (alive[i] && squad_of[i] == s && (i == me_idx || !slot_away(i))) ++n;
    return std::max(n, 1);
}

int World::squad_alive(int s) const {
    int n = 0;
    for (int i = 0; i < n_allies && i < 6; ++i) if (alive[i] && squad_of[i] == s) ++n;
    return n;
}

int World::alive_count() const {
    int n = 0;
    for (int i = 0; i < n_allies && i < 6; ++i) if (alive[i]) ++n;
    return std::max(n, 1);
}

float World::siege_of(int squad) const {
    float s = 0;
    for (int i = 0; i < n_allies && i < 6; ++i)
        if (alive[i] && (squad < 0 || squad_of[i] == squad)) s += siege_dps[i];
    return s;
}

int World::claims(int gi) const {
    int n = 0;
    for (int i = 0; i < n_allies && i < 6; ++i)
        if (i != me_idx && alive[i] && claim_of(i) == gi) ++n;
    return n;
}

void World::set_claim(int gi) const {
    if (me_idx < 6) write_sm(me_idx, static_cast<std::uint8_t>((my_arrived && gi >= 0 ? 0x80 : 0) | (my_bucket << 6) | (gi < 0 || gi >= 62 ? 0 : gi + 1)));
}

int World::squad_target(int s) const {
    return static_cast<int>(sm[6 + (s & 1)] & 0x3f) - 1;
}
bool World::squad_help() const { return (sm[6] & 0x80) != 0; }
void World::set_squad_target(int gi, bool help) const {
    // bits 6 (byte 6) and 6-7 (byte 7) hold the pvp focus (pvp.cpp): keep them
    const int b = 6 + (squad_of[me_idx] & 1);
    const auto keep = sm[b] & (b == 6 ? 0x40 : 0xc0);
    write_sm(b, static_cast<std::uint8_t>(((gi < 0 ? 0 : gi + 1) & 0x3f) | keep | (help && b == 6 ? 0x80 : 0)));
}

int World::claim_of(int slot) const {
    if (slot < 0 || slot >= 6) return -1;
    const int v = sm[slot] & 63;
    return v == 63 ? -1 : v - 1;
}

bool World::slot_away(int slot) const {
    return slot >= 0 && slot < 6 && (sm[slot] & 63) == 63;
}

void World::set_away() const {
    if (me_idx < 6) write_sm(me_idx, static_cast<std::uint8_t>((my_bucket << 6) | 63));
}

int World::gold_bucket(int slot) const {
    return slot >= 0 && slot < 6 ? (sm[slot] >> 6) & 1 : 1;
}

int World::arrived_claims(int gi) const {
    int n = 0;
    for (int i = 0; i < n_allies && i < 6; ++i)
        if (i != me_idx && alive[i] && claim_of(i) == gi && (sm[i] & 0x80)) ++n;
    return n;
}

int World::slot_of(const bc::champion& c) const {
    for (int i = 0; i < n_allies; ++i) if (ally_h[i] == c) return i;
    return -1;
}
