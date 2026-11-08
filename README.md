# Cambridge Battlecode 2026: team EvilMangohh

Our bot for [Cambridge Battlecode 2026a](https://2026a.battlecode.co.uk) (12 Oct to 18 Nov 2026). It is about 6,400
lines of C++20, compiled to WASI, and every champion on the map runs its own instance of it. Over four weeks we shipped 72
versions to the ladder and peaked at rank 22 of roughly 800 teams.

This repo holds the final bot, platform version v70, plus how we built it: the strategy, the testing workflow, and
what we would do again.

## The game

Each match starts with a draft: both teams pick 3 to 5 champions from a pool of 12. Then the champions farm neutral
camps (wolves, elk, gem and gold clusters, golems, and 20,000 hp monoliths) for gems and gold. Gems only count once a
champion carries them to an altar and channels a tribute. Die on the way and the killer takes the whole bag. The first
team to bank 1000 gems wins. At round 2000, banked gems and then lifetime gold break the tie.

A few rules shaped almost every design decision:

- **No shared brain.** Each champion runs its own bot instance and sees only what it (and its ward) can see. The team
  shares 8 bytes of memory for the whole match, plus 1024 bytes written during the draft.
- **Fuel.** A C++ turn gets 100,000 units of fuel (roughly instructions). Run out and the turn is suspended mid-code
  and resumes next round, so expensive planning costs actions.
- **Deaths are expensive.** A dead champion is out for 60 rounds, and the killer's team gets 100 gold plus every gem
  the victim carried.
- **Gold buys power.** Gold buys stat shards; five matching shards combine into an emblem with a passive effect.
- **Determinism.** The same two bots on the same map and side always play the same game.

The game was patched four times while we played. Three balance patches landed in week 2, then a large patch (2.0.0)
on 25 Oct rebalanced every champion, cut the camp economy, added emblem families and grew the map pool from 35 to 70
maps.

## Results

| When | Version | Ladder |
|---|---|---|
| 20 Oct | v19 | Peak rank **22** (rating 1899) |
| 26 Oct | v20, first build on patch 2.0.0 | Rank 143 at upload, after the patch reshuffled the ladder |
| 1-3 Nov | v30 to v72, one change per upload | From 137 back up to 72 at best |
| 3 Nov | v70 pinned for the 4 Nov international qualifiers | Rank 86 to 96 |
| 6 Nov | v70 | Rank 104 (rating 1961) |

On a fixed 90-game benchmark against the top 10 teams' bots, v28 won 5 games and v70 won 17. Against the top 10's
newest bots in early November we won about 4 in 100. We never closed that gap. The lessons below are mostly about why.

## Repository layout

```
.
├── README.md
├── cambc.toml               workspace config for the cambc CLI (SDK 4.0.0)
└── bots/
    └── evilmangohh/src/     the final bot (platform version v70)
```

| File | What it does |
|---|---|
| `main.cpp` | Turn loop: draft or play, then spend leftover fuel on one-time map analysis |
| `draft.cpp`, `draft.h` | Map-aware draft |
| `bot.cpp`, `bot.h` | A champion's turn: shopping, banking, retreating, the order decisions run in |
| `world.cpp`, `world.h` | Per-champion world model: map data, camp groups, distance fields, shared-memory claims |
| `plan.cpp`, `expanse.cpp` | Camp planning: which group each champion farms, squads on normal maps, solo loops on big open maps |
| `combat.cpp` | Fighting camps, retreating, walking to altars |
| `pvp.cpp`, `pvp_plan.cpp`, `fight.h`, `fight_sdk.h` | Champion fights: a pure damage model and a one-turn planner on top of it |
| `actions.cpp`, `actions.h` | Movement and attack primitives |
| `mono*.cpp`, `mono.h`, `contest.cpp` | Monoliths and contest maps |
| `gate.cpp`, `seal.cpp` | Maps where camps or monoliths wall off the altars |
| `vision.cpp`, `vision.h` | Ward placement |
| `heart.cpp`, `heart.h` | Heart of Ruin, the champion added in the week 2 patch |
| `zreach.cpp`, `zreach.h` | Camp reach and engine stats after the week 2 patch, plus fight switches |
| `prof.h` | Debug-only fuel profiler (compiled out of release builds) |

The code comments are dense on purpose. Each rule carries the measurement that justified it, which version
introduced it, and what happened when it was off. They cite fork names (`p-fix`, `c18b`), internal version numbers
(v58) and notes or run folders from our development workspace (`notes/...`, `runs/...`). Those files are not in this
repo, but the numbers in the comments are the ones we decided on.

## Building and running

You need the competition's `cambc` CLI. We finished on CLI 4.0.0 (engine 2.0.0).

```bash
cambc init
```

```bash
cambc run bots/evilmangohh bots/evilmangohh --map 00ambush -o runs/test --release
```

```bash
cambc bundle evilmangohh --output evilmangohh.zip
```

`cambc init` sets up the SDK and maps in the workspace. Drop `--release` to keep debug output (`std::cout` lands in
the replay). `cambc bundle` builds the zip the platform accepts.

**Switches.** Nearly every behaviour is a compile-time switch, an `inline constexpr` in a header read through
`if constexpr`. The source here is set to exactly what v70 ran. The last four switches we shipped were
`HOR_PRESHIELD = 3` (heart.h, v58), `GATE_WALK` and `WAIT_FUEL` (bot.h, v67), and `ULTRA = 80` (draft.h, v70).

## Strategy

### The turn

`main.cpp` runs the draft or a turn. Whatever fuel is left after the turn's decisions goes to one-time work: ward
tables, finding walled-off altars, distance fields for later turns. Analysis never delays an action.

Inside a turn (`Bot::play_turn`) the order is:

1. Move the ward, update the world model, shop if on spawn.
2. If channelling a tribute, cancel it only if the visible enemies can kill us before it finishes. Otherwise keep
   channelling: damage alone never interrupts a tribute.
3. Carrying 20+ gems next to an open altar, start the tribute before anything else, unless the enemies in reach could
   kill us through it.
4. If enemy champions are close, let the fight planner play the turn (`pvp::turn`).
5. Otherwise: monolith take, retreat if hurt, capture a finished camp, bank the bag, or farm.

### Draft

The whole map is visible during the draft, so the bot reads the map kind from its camp layout and picks from a fixed
table for that kind. It never counter-picks.

| Map kind | Picks, in order |
|---|---|
| Normal | Ren, Marcella, Heart of Ruin, Ryth, Varg |
| Contest (every gem is in a monolith) | Ren, Stellar, Ryth, Tavi, Prism: everyone hits from range 4 |
| Boss (80%+ of gems in groups too strong to tank, 3-4 picks) | Stellar, Tavi, Prism, Cassi & Issac, Ren |
| Open (large 3-pick maps with 80+ camp gems) | Cassi & Issac, Stellar, Varg |
| Rows (one specific layout) | Cassi & Issac, Stellar, Varg, Ren |

We tested other drafts many times. The ones that won came from fixing a measured problem: Stellar caused 31% of our
deaths to champions, so Marcella took her slot, and deaths fell from 8.5 to 5.0 a game in local tests.

### Farming

- **Needs.** Every camp group gets a "need": how many champions it takes to kill it. Camps hit the nearest champion,
  so the need depends on whether the tank's health covers the damage.
- **Claims.** Champions claim groups through shared memory. Each one takes the group where it adds the most gems per
  round, given who else is already there, and comes back to it when it respawns. Normal maps use one or two squads;
  big open maps run solo loops.
- **One capturer per kill.** A camp's reward is split between every ally standing on it when the capture finishes.
  One champion captures and takes it all, while the rest move straight on to the next camp. Marcella gets priority,
  because rewards funnelled to one carry snowball.
- **Fast captures.** A melee champion steps onto the reward tile in the same turn it lands the kill, and the nearest
  ally captures, not the first in draft order. Those two cut the time from kill to capture from 2.7 to 1.4 rounds. A
  champion with gold to spend recalls from the capture tile, so the recall overlaps the capture wait.
- **Sieges.** Groups too strong to tank, like elk herds, get worn down from range 4. Camps don't regenerate, so the
  damage keeps.
- **Builds.** The first emblem is Prospector (+25% on gem and gold cluster rewards), the repeating step is Bruiser,
  and Marcella buys her damage stat first.

### Banking

Most of our late losses were carriers dying near an altar with a big bag, so the banking rules are about bag size and
timing:

- Bank smaller bags on small maps, after round 600 (25 gems at most), and once the enemy has banked 700.
- Any carrier with 20+ gems banks when an altar is within 10 tiles.
- Walk into range and start the tribute in the same turn. We had been wasting a turn next to the altar on every
  tribute. Shipped with the end-game bag cap, this took the benchmark from 18 wins in 100 to 29.
- If the enemy locks an altar, walk to another. If two teams stall next to an altar, a standoff breaker sends the
  carrier elsewhere.

### Fighting

- **A pure fight model** (`fight.h`): no SDK calls, no heap, fixed arrays. It simulates a few rounds of
  champion-vs-champion damage with strength, focus-scaled spells, shields, stuns, and the champion quirks that matter
  (Ren hits back when damaged, Cassi & Issac die together).
- **A one-turn planner** (`pvp_plan.cpp`): every turn it scores each reachable end tile and action set as damage dealt
  minus risk times the damage the enemy can land there before our next turn. It never ends a turn where the enemy can
  kill us first, unless this turn takes a kill.
- **Commit rules**: no fight against a group with twice our strength, no free pokes when not committed, focus
  hurt enemies, chase carriers who are low on health, and have Heart of Ruin shield before contact.
- **Vision**: wards go on bush clusters next to camps and follow the champion through the cluster.

### Map-specific fixes

Many late wins came from maps where a general rule broke:

- **Monoliths** (20,000 hp, 200 gems): the team takes one only when a model of team damage, stuns, shields and heals
  says it finishes with margin, with no enemy near. Everyone stages outside its reach and walks in together.
- **Contest maps**: a 200-gem bag takes 20 turns to bank at the central altar, and walked there alone it was stolen in
  12 of 25 local tests. The carrier now waits at spawn for two escorts.
- **Walled-off altars**: on maps where a ring of camps surrounds the only altar, the bot finds the cheapest camp to
  kill and walks through the gap (`gate.cpp`). Sealed altars, where monoliths stand on every tile in reach, are
  detected from geometry (`seal.cpp`).
- **Stalls**: fixes for champions stepping back and forth, standing inside a camp block waiting for it to respawn, and
  recall loops under fire.

## How we worked

### Many agents, one laptop

We wrote and tested the bot with Claude Code agents, up to eight at a time on one laptop, with us choosing what to try
and what to ship.

- **Forks.** Each agent forked the current bot into its own folder and owned it. Nobody edited another agent's bot.
  A fork added one idea behind one switch, measured it, and wrote down the numbers.
- **Judges.** A separate judge agent re-measured every fork on fresh samples and reviewed its diff before anything was
  merged. Judges caught real bugs several times: our altar locks cancelling our own tributes, a dash that walked
  into enemy reach, an engine error after a same-turn tribute.
- **Workflows.** Longer pipelines (audit a patch, plan forks, run forks and judges, merge, gate, bundle) ran as
  resumable workflows, so a paused or crashed run picked up from its cached steps. Agents also wrote their numbers to
  disk as they went, which is why nothing was lost when a network outage killed every agent mid-task.
- **One wrapper around the CLI.** It capped concurrent matches across every agent and ran them at background priority,
  so parallel experiments left the laptop usable. Fuel meters every match, so throttling changes wall time, never
  results.
- **A version log.** Every upload, measurement and decision went into one log. It made every later decision faster.

### Proving a change works

Our measurement problems were as hard as the game:

1. **Off means identical.** Every switch, set off, had to build the previous version's binary byte for byte (same
   md5). That proved a change did only what it claimed. Some edits fail this in surprising ways: splitting `t += a * b`
   into two statements stops the compiler fusing the multiply-add, so the binary changes with the switch off.
2. **Fuel chaos.** Decisions depend on how much fuel is left, so any code change re-rolls many games. Two builds with
   identical logic differed by up to 15 games in 1692. A local result counted only if it held on two independent fuel
   samples.
3. **Local screen.** Each candidate played the current version on all 70 maps from both sides (140 games), compared
   game for game, plus 70 games against the starter bot. It had to win all 70 with zero faults. That gate once caught a
   build that banked nothing in 2000 rounds against the starter bot.
4. **Platform benchmark.** After upload, 90 unranked games against the same opponent bot versions, on the same maps and
   sides every time. We compared paired measures (kills minus deaths, banked gems, gold at round 700) instead of win
   counts. A version stayed live only if it was better on balance. Otherwise we reverted it as soon as the benchmark
   finished.
5. **Quota.** The platform allowed about 50 unranked matches an hour, and a 90-game benchmark used about two hours of
   it. That meant roughly a dozen platform tests a day, and that budget decided how many ideas we could try.

### Finding the next improvement

- **Replays of our losses.** A local replay reader built on the engine let scripts classify every death by cause and
  measure tempo: kill-to-capture delay, idle champion-rounds, wasted turns.
- **Generated maps.** A map generator made 30 fair maps the bot had never seen, to check that a gain was not overfit
  to the official pool.

## Lessons

**Measuring changes**

- Local A/B tests against your own old bots lie. Our frozen bots never contested a monolith or punished a walk to the
  altar, so local wins (38 of 64) turned into platform losses (0 of 25). Use local runs to check that nothing broke.
- Judge on the real platform, against pinned opponent bots, on the same maps and sides every time. Compare game for game.
  At 5 to 15 wins in 100, win counts are mostly noise.
- Refresh the benchmark opponents. Our week-old pinned set said 17 in 90; the current top 10 said 4 in 100.
- Test one change at a time. Seven locally screened changes merged together hid the four that hurt.
- Deterministic engines amplify tiny code changes. When a fix only targets some maps, make it a true no-op everywhere
  else, so the test shows only its effect.
- Ranked results cannot separate close versions. You would need about 1000 games each.
- Test versions play real ranked games while live and cost rating. Keep tests short and revert fast.

**What improved the bot**

- Turns we threw away mattered more than clever fighting: tributing in the same turn as the walk, walking after a dash,
  stepping onto the reward on the kill.
- Bug-like failures found in replays of losses, like stuck champions, oscillating walks, standoffs, recall loops under
  fire, or the wrong draft table on one map type. We kept those fixes almost every time.
- Economy and banking: funnel rewards to the carry, bank smaller loads late, place wards on cooldown.
- Actions before contact helped (a shield cast before the fight). Changing behaviour inside fights almost never did, and
  every "play safer, back off" rule cost kills.

**Process**

- Benchmark against the strongest current bots early and often. Patches and bots change daily.
- Run many agents in parallel, but cap local matches so the laptop stays usable.
- Write down every measurement and decision as you go.
