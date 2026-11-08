#pragma once

// x-rows: a draft and build for the rows map only: 4 picks, no elk, 50x50, 844 gems with 448 in the monolith group.
// Judge (19 Oct): the broad signature (4 picks, no elk, gems in a monolith and in wolf groups) also fires on unseen
// maps; there db2 lost +13/-31 (z -2.71, 9 fresh signature maps, runs/x-rows-judge), so it is pinned to rows' layout.
// Every switch off builds rr-r1's binary.
inline constexpr bool ROWS_DRAFT = true; // rows draft C&I > Stellar > Varg > Ren (MapKind::race)
inline constexpr bool ROWS_BUILD = true; // rows builds: Monster x5 (Focus: casters and Ren; Strength: melee), then Power
// META (v21, notes/meta-oct26.md): an alternative draft on normal maps, Vesper > Vendetta > HoR > Marcella > C&I
// (4 picks drop C&I, 3 picks Marcella too), with Bruiser builds and Marcella / Vendetta tanking. Off: worse on the platform.
inline constexpr bool META = false;
// META camp damage per round for the solo gate (world.cpp camp_dps_of; unlisted champions get 60, which kept them off gem
// groups: md on 00ribs 28-1037, gold clusters only). Vendetta 70 str + resets on kills, Marcella 45 str + 2 x focus spell.
inline constexpr int META_VEN = 90;
inline constexpr int META_MAR = 75;
inline constexpr int META_VES = 75;
// STEL_SWAP (v23): replace Stellar in the normal table (3-5 pick maps): 0 keep, 1 Vesper, 2 Vendetta, 3 Marcella. On v22
// Stellar is 31% of our champion-kill deaths vs the top 10 (650 hp, strikes first 68%, pokes from 5; their pick rate 1%).
inline constexpr int STEL_SWAP = 3;
// META_KIT: the META builds, tank priorities and camp damage apply to the new champions whenever we draft them
inline constexpr bool META_KIT = META || STEL_SWAP > 0;
// BOSS_NORM (notes/losses-nov1.md #2): the pre-2.0.0 boss and contest tables lose on the platform (00stampede, 01starvation,
// 00contest 26/82 = 32% vs 51% for the normal table). 1: the boss table only on 3-4 pick maps (00rows keeps it, 23/29), so
// 00stampede and 01starvation draft the normal table; 2: also no contest table (00contest drafts the normal table); 3 (c8):
// as 2 on 4-5 pick maps, the contest table on 3-pick contest maps (03homonoliths: with SPAWN_SIEGE, Ren / Stellar / Ryth
// take the spawn triple by r773-894, Ren / Marcella / HoR by r953-1029). 0 = v28.
inline constexpr int BOSS_NORM = 1;

// c19j ULTRA (bughunt-fresh-nov3 #5): MapKind::open (3 picks, area >= 4000: the expanse table C&I / Stellar / Varg) also needs
// the camp groups' gem_value to sum to ULTRA+. Built for the old 3-pick 00expanse (334 gems in 24 groups); 03ultrastarvation
// (67x73, 36 gems in 6 groups: a kill map) drafted it, 0 kills / 106 deaths in 2 fresh games, 0/7 ranked since 1 Nov. The
// big 3-pick generated maps (maps-gen) sum to 116-454 and keep it. 0 = off (v67); v70 ships 80.
inline constexpr int ULTRA = 80;

enum class MapKind { normal, boss, open, contest, race };
MapKind map_kind(int team_size); // from the static camp layout (works in the draft and in play)

void play_draft();
