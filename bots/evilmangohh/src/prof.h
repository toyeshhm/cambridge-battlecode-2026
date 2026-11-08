#pragma once
// Debug-only fuel profiler: PROF("name") marks a checkpoint inside debug_block (fuel paused, compiled out in
// release). prof_end() prints the turn's segments when it was heavy or got suspended (round changed mid-turn).
#include <cambc.h>
#include <cstdint>
#include <cstdio>
#include <string>

struct Prof {
    static constexpr int N = 32;
    const char* name[N];
    std::uint64_t fuel[N];
    std::uint32_t round[N];
    int n = 0;
    void mark(const char* s) {
        if (n >= N) return;
        name[n] = s, fuel[n] = cambc::fuel_remaining(), round[n] = cambc::round_number();
        ++n;
    }
};
inline Prof g_prof;

#define PROF(s) debug_block({ g_prof.mark(s); })

// Segment cost: same round = fuel difference; across a suspension = rest of the old budget + used of the new one.
inline void prof_end(int idx) {
    debug_block({
        g_prof.mark("end");
        std::string champ = "?";
        try { champ = cambc::to_string(cambc::self().champion_name()); } catch (...) {}
        const Prof& p = g_prof;
        std::uint64_t tot = 0;
        bool susp = false;
        char buf[1024];
        int k = 0;
        for (int i = 1; i < p.n; ++i) {
            const bool cross = p.round[i] != p.round[i - 1];
            const std::uint64_t d = cross ? p.fuel[i - 1] + (100000 - p.fuel[i]) : p.fuel[i - 1] - p.fuel[i];
            susp |= cross;
            tot += d;
            if (k < 900) k += std::snprintf(buf + k, sizeof buf - k, " %s%s:%llu", cross ? "!" : "", p.name[i], static_cast<unsigned long long>(d));
        }
        if (susp || tot > 60000)
            std::printf("FP R%u i%d %s tot=%llu susp=%d%s\n", p.round[0], idx, champ.c_str(), static_cast<unsigned long long>(tot), susp ? 1 : 0, buf);
        g_prof.n = 0;
    });
}

