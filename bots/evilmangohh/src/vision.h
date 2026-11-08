#pragma once
// Wards (and reading enemy altar locks). Self-contained: it only needs
// World's static map data (walls, altars, camp groups) and movement helpers, so it ports into other bots.
#include "world.h"
#include <vector>

namespace vis {
// Bush clusters and their ward scores, built a cluster at a time from spare fuel at the end of turns.
void init_step();
bool ready();
// Place the free ward on the best scored bush cluster within 2 when the current one no longer covers us
// (or one 1.5x better is in reach), or move it along its cluster once we are 4+ from it.
// `tribute`: we carry a bag, clusters within 8 of an altar count double. Call BEFORE W.update(): moving the
// ward drops the old ward's tiles from vision, and camps cached from them throw 97 later in the turn.
void ward(bool tribute);
// W3 (f-bank): a gem carrier (20+) within 8 of an altar wards an unseen bush tile within 4 of that altar and 2 of
// us (a ward on a bush shows the whole bush): enemies channelling or waiting in altar bushes killed our carriers.
// Call before W.update() like ward(). Returns true when it placed the ward.
bool scout_altar();
// Altar `a` carries an enemy lock (visible); hits left, 0 if none.
int enemy_lock_hits(vec2 a);
} // namespace vis
