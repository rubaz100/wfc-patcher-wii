#include "mkwKnockoutNotice.hpp"
#include "mkwKnockoutState.hpp"
#include <cwchar>

#include <cassert>
#include <cstdio>

using namespace wwfc;
using namespace wwfc::Knockout;

static Snapshot race(u8 count, u16 lap)
{
    Snapshot snapshot    = {};
    snapshot.playerCount = count;
    for (u8 i = 0; i < count && i < MAX_PLAYERS; ++i) {
        snapshot.order[i] = i;
        snapshot.laps[i]  = lap;
    }
    return snapshot;
}

int main()
{
    assert(IsPacketRaceTime(0, 0, 0, true));
    assert(IsPacketRaceTime(62, 59, 999, true));
    assert(!IsPacketRaceTime(63, 0, 0, true));
    assert(!IsPacketRaceTime(99, 99, 999, true));
    assert(!IsPacketRaceTime(1, 60, 0, true));
    assert(!IsPacketRaceTime(1, 0, 1000, true));
    assert(!IsPacketRaceTime(1, 0, 0, false));
    for (u8 placement = 2; placement <= MAX_PLAYERS; ++placement) {
        u32 ms = FinalPlacementTimeMs(180000, placement);
        assert(ms == 180000u + placement * 10u);
        assert(ms / 60000 < 63 && (ms / 1000) % 60 < 60);
        wchar_t message[128];
        assert(FormatEliminationNotice(message, placement));
        assert(std::wcsstr(message, L"Eliminated"));
        assert(std::wcsstr(message, L"Now spectating"));
    }
    assert(FinalPlacementTimeMs(0xffffffffu, 2) == 0);
    assert(FinalPlacementTimeMs(0, 2) == 0);
    assert(FinalPlacementTimeMs(63u * 60000u - 10, 2) == 0);
    wchar_t ordinal[128];
    assert(FormatEliminationNotice(ordinal, 2) && std::wcsstr(ordinal, L"2nd"));
    assert(FormatEliminationNotice(ordinal, 3) && std::wcsstr(ordinal, L"3rd"));
    assert(FormatEliminationNotice(ordinal, 11) && std::wcsstr(ordinal, L"11th"));
    assert(FormatEliminationNotice(ordinal, 12) && std::wcsstr(ordinal, L"12th"));
    wchar_t tiny[3];
    assert(!FormatEliminationNotice(tiny, 4) && tiny[2] == 0);
    assert(!FormatEliminationNotice(ordinal, 0));
    assert(FormatEliminationNotice(ordinal, 4, false));
    assert(std::wcsstr(ordinal, L"4th place") && !std::wcsstr(ordinal, L"spectating"));

    RaceState state;
    Decision  decision = {};
    auto      snapshot = race(4, 0);
    assert(!state.evaluate(snapshot, decision));
    snapshot.laps[0] = 1;
    assert(!state.evaluate(snapshot, decision)); // start line is not a full lap
    snapshot.laps[0] = 2;
    assert(state.evaluate(snapshot, decision));
    assert(decision.playerId == 3 && decision.leaderLap == 2 && decision.survivors == 0x7);
    assert(!state.evaluate(snapshot, decision)); // one decision per lap

    state.markEliminated(3, 4);
    snapshot.finishedMask = 1u << 3;
    // Even if the game's finish sorting puts the eliminated kart first, it
    // must not become our leader or be eliminated again.
    snapshot.order[0] = 3;
    snapshot.order[1] = 0;
    snapshot.order[2] = 1;
    snapshot.order[3] = 2;
    snapshot.laps[0]  = 3;
    assert(state.evaluate(snapshot, decision));
    assert(decision.playerId == 2 && decision.survivors == 0x3);
    state.markEliminated(2, 3);

    // Retirement does not set the native finished bit. Do not finalize any
    // retired kart while even one survivor is still racing.
    snapshot.finishedMask = 0;
    assert(!state.survivorsFinished(snapshot));
    snapshot.finishedMask = 1;
    assert(!state.survivorsFinished(snapshot));
    snapshot.finishedMask = 3;
    assert(state.survivorsFinished(snapshot));
    assert(state.placement(3) == 4 && state.placement(2) == 3);
    assert(state.placement(0) == 0 && state.placement(99) == 0);
    snapshot.finishedMask = 0;

    // A disconnected kart is not a candidate, and a single survivor is never
    // eliminated by the lap policy.
    snapshot.disconnectedMask = 1u << 1;
    snapshot.laps[0]          = 4;
    assert(!state.evaluate(snapshot, decision));

    // State resets between races, even when player count and order are reused.
    state.reset();
    snapshot = race(4, 0);
    assert(!state.evaluate(snapshot, decision));
    assert(state.eliminatedMask() == 0 && state.placement(3) == 0);
    snapshot.laps[0] = 2;
    assert(state.evaluate(snapshot, decision) && decision.playerId == 3);

    // A mid-race leader change must not repeat an already processed lap.
    snapshot.order[0] = 1;
    snapshot.order[1] = 0;
    snapshot.laps[1]  = 2;
    assert(!state.evaluate(snapshot, decision));
    snapshot.laps[1] = 3;
    assert(state.evaluate(snapshot, decision));

    // Bad network/game snapshots must not consume an otherwise valid lap.
    snapshot.laps[1]  = 4;
    snapshot.order[3] = 12;
    assert(!state.evaluate(snapshot, decision));
    snapshot.order[3] = 1; // duplicate
    assert(!state.evaluate(snapshot, decision));
    snapshot.order[3] = 3;
    assert(state.evaluate(snapshot, decision));
    snapshot.playerCount = 13;
    assert(!state.evaluate(snapshot, decision));
    snapshot.playerCount = 1;
    assert(!state.evaluate(snapshot, decision));

    state.reset();
    snapshot              = race(3, 1);
    snapshot.finishedMask = 1;
    assert(!state.evaluate(snapshot, decision));
    snapshot.laps[1] = 2;
    assert(state.evaluate(snapshot, decision));
    assert(decision.playerId == 2 && decision.survivors == 2);

    // A changed grid count establishes a fresh baseline, never an old lap's
    // elimination applied to a different grid.
    snapshot = race(2, 2);
    assert(!state.evaluate(snapshot, decision));
    snapshot.laps[0] = 3;
    assert(state.evaluate(snapshot, decision) && decision.playerId == 1);
    puts("Knockout state regression cases passed");
}
