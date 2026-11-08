#pragma once
// Champion-vs-champion turns (p-combat). Self-contained so it can be ported: needs fight.h / fight_sdk.h,
// actions.h (best_step, hp_frac) and world.h (W: static walls, home field, camps, shared memory bytes 6/7).
#include "world.h"
#include <vector>

namespace pvp {

// Plays the whole turn when a fight is on and returns true. When it declines a fight it returns false and,
// if enemies could hit us nearby, fills `avoid` (tiles inside enemy reach) so farming moves stay out of it.
bool turn(const std::vector<bc::champion>& enemies);

// Hit since our last turn by nobody we can see (no visible enemy within 5, no awake camp within 3): ward the
// nearest hidden bush tile within 2. A ward on a bush shows the whole connected bush, so we can answer this turn.
void reveal();
void note_end_hp(); // end of every turn: hp to compare against next turn
// Turn priority while fighting (bot.cpp sets it at the end of every turn; it applies from the next round).
int combat_priority();

extern bc::bitboard avoid;
extern bool avoid_on;    // movement primitives skip `avoid` tiles while this is set (reset every turn)
extern int last_seen;    // last round an enemy was within 7
extern int last_contact; // last round we were inside enemy reach or hit an enemy
// k-hold H1: a big reward group (>= HOLD_GEMS gems, or a monolith) within 2 of us while we win the fight around it
// (power ratio near it, recent trade): the planner holds its tiles instead of kiting home. Set by turn() this round.
extern int hold_gi, hold_round;
inline constexpr bool HOLD = true;
inline constexpr int HOLD_GEMS = 70, TRADE_R = 20;
inline constexpr bool HOLD_ON_RETREAT = true; // bot.cpp: no post-skirmish retreat (hp < 60%) within 2 of such a reward
inline constexpr bool HOLD_WALK = true;       // plan.cpp capture(): the walk to it ignores the avoid mask while holding
// o-altar 2b: reward group whose spawn tile we ended last turn on (bot.cpp sets it before turn(), -1: none). Plans keep us on
// its tiles (PEND_ON > any non-lethal risk, < plan::LETHAL_PEN) and drop the home pull; it outranks H1's hold.
extern int pend_gi;
inline constexpr float PEND_ON = 2000.f;

} // namespace pvp
