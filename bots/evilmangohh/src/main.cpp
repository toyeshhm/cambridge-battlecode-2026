// Dispatch draft and gameplay; a failed turn is logged and still yields.
#include <cambc.h>
#include <iostream>
#include "bot.h"
#include "draft.h"
#include "vision.h"

int main() {
    Bot bot;
    while (true) {
        try {
            if (cambc::is_draft())
                play_draft();
            else {
                bot.play_turn();
                // ward tables from each turn's spare fuel (a bush cluster at a time) so no turn loses its actions
                if (!vis::ready() && cambc::round_number() >= 3 && cambc::fuel_remaining() > 40000) vis::init_step();
                // z-boss: after every decision of the turn (fuel does not carry over), so other maps replay rr-c1
                if constexpr (SEAL_NOPARK || SEAL_GOLD || SEAL_SIEGE) if (!g_seal) seal_detect();
                if constexpr (SEAL_NOPARK || SEAL_SIEGE) if (g_seal == 2) seal_turn_end();
                if constexpr (GATE) gate_detect(); // c7m: altars walled in by camps (gate.cpp)
                if constexpr (MONO_TAKE) bot.mono_end(); // p-monolith: the take's start, the turn's last code (no fuel chaos)
            }
        } catch (const std::exception& error) {
            std::cout << "bot error: " << error.what() << std::endl;
        } catch (...) {
            std::cout << "bot error: unknown exception" << std::endl;
        }

        cambc::end_turn();
    }
}
