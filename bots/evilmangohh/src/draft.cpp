#include <cambc.h>
#include "draft.h"
#include "heart.h"
#include "zreach.h"
#include <algorithm>
#include <span>

// Map-aware draft: one fixed table per map kind and no counter-picking;
// the map is fully visible in the draft, so the map kind is read from its camp layout.
MapKind map_kind(int team_size) {
    int total = 0, boss = 0, mono_gems = 0, elk = 0;
    for (const auto& g : cambc::map::camp_groups()) {
        float str = 0;
        bool mono = false;
        for (const auto& c : g.camps()) {
            if constexpr (CAMP_STATS) { // p-fix F6: golem 120 (was 0)
                str += camp_stat(c.kind()).str;
                if (c.kind() == cambc::camp_type::monolith) mono = true;
                if (ROWS_DRAFT && c.kind() == cambc::camp_type::elk) ++elk;
                continue;
            }
            switch (c.kind()) {
            case cambc::camp_type::wolf: str += 20; break;
            case cambc::camp_type::elk: str += 60; if constexpr (ROWS_DRAFT) ++elk; break;
            case cambc::camp_type::monolith: str += 200; mono = true; break;
            default: break;
            }
        }
        const int gems = static_cast<int>(g.gem_value());
        total += gems;
        if (mono || str >= 240) boss += gems; // monoliths and 4+ elk herds: too strong to tank, sieged
        if (mono) mono_gems += gems;
    }
    const int area = static_cast<int>(cambc::map::width() * cambc::map::height());
    if (total > 0 && mono_gems == total) return MapKind::contest;        // every gem in monoliths (contest)
    if (ROWS_DRAFT && team_size == 4 && elk == 0 && total == 844 && mono_gems == 448 && area == 2500) // x-rows: rows only
        return MapKind::race;
    if (total > 0 && boss >= 0.8f * total) return MapKind::boss;          // stampede
    if (team_size <= 3 && area >= 4000 && (ULTRA == 0 || total >= ULTRA)) return MapKind::open; // expanse (80x80, 3 players)
    return MapKind::normal;
}

void play_draft() {
    using D = cambc::champion_draft;
    // Default table: Ren > Stellar > C&I > Ryth > Varg (prefix on 3/4-player maps), casters on the boss maps,
    // C&I/Stellar/Varg on expanse. Alternatives tested locally and not used: C&I > Stellar > Varg > Ren as the
    // default 6/14 vs 11/14; on contest, other caster tables 9/14, Prism > Stellar > C&I > Ryth > Tavi 6/14 and
    // Tavi > Willow > Stellar > Ren > Prism 4/14; Varg > Stellar > C&I > Ryth > Tavi on 5-player maps 68/98 vs
    // 71/98; Varg > Ryth > C&I > Stellar on 4-player maps 36/56 vs 40/56.
    // h-heart (heart.h): HoR in place of one pick, or first (dropping the last); every switch off: rr-c2's table
    static constexpr D H = D::heart_of_ruin, C3 = HD_CI ? H : D::cassi_and_issac, Y4 = HD_RYTH ? H : D::ryth, V5 = HD_VARG ? H : D::varg;
    static constexpr D normal[] = {HD_FIRST ? H : D::ren, HD_FIRST ? D::ren : STEL_SWAP == 1 ? D::vesper_vale : STEL_SWAP == 2 ? D::vendetta : STEL_SWAP == 3 ? D::marcella : D::stellar, HD_FIRST ? D::stellar : C3,
                                   HD_FIRST ? D::cassi_and_issac : Y4, HD_FIRST ? D::ryth : V5};
    static constexpr D meta[] = {D::vesper_vale, D::vendetta, H, D::marcella, D::cassi_and_issac}; // META (draft.h)
    static constexpr D small[] = {D::ren, D::stellar, H}; // HD_SMALL: 3-pick maps
    static constexpr D boss[] = {D::stellar, D::tavi, D::prism, D::cassi_and_issac, D::ren};
    static constexpr D open[] = {D::cassi_and_issac, D::stellar, D::varg};
    // f-contest K15: contest is a 2000-round brawl sieged from range 4. Cassi&Issac die together (one kill takes
    // both, ~40% of our contest deaths); every pick here hits from range 4 (contest set 29/40 with contest.cpp).
    static constexpr D contest[] = {D::ren, D::stellar, D::ryth, D::tavi, D::prism};
    // x-rows: the rows draft (from platform v11-v15 rows games: C&I + Varg drafts did best there)
    static constexpr D race[] = {D::cassi_and_issac, D::stellar, D::varg, D::ren};
    static constexpr D rest[] = {D::ren, D::stellar, D::cassi_and_issac, D::ryth, D::varg,
                                 D::tavi, D::prism, D::willow, D::vesper_vale, D::marcella, D::vendetta};
    static MapKind kind = map_kind(static_cast<int>(cambc::draft::team_size()));
    std::span<const D> table = normal;
    if constexpr (META) table = meta;
    if constexpr (HD_SMALL) if (kind == MapKind::normal && cambc::draft::team_size() == 3) table = small;
    if (kind == MapKind::boss && !(BOSS_NORM > 0 && cambc::draft::team_size() > 4)) table = boss;
    else if (kind == MapKind::open) table = open;
    else if (kind == MapKind::contest && (BOSS_NORM < 2 || (BOSS_NORM == 3 && cambc::draft::team_size() <= 3))) table = contest;
    else if (ROWS_DRAFT && kind == MapKind::race) table = race;
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    auto free = [&](D p) { return std::find(chosen.begin(), chosen.end(), p) == chosen.end(); };
    for (auto p : table) if (free(p)) { cambc::draft::pick(p); return; }
    for (auto p : rest) if (free(p)) { cambc::draft::pick(p); return; }
}
