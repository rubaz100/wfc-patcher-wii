// Knockout Mode — client-side elimination racing for mkwfc.
//
// STATUS: M1 scaffold. This compiles against proven interfaces only
// (RaceManager state + GPReport) and does not yet read player positions or
// eliminate anyone. It establishes the feature, the race lifecycle tracking,
// and an observable report so the pipeline can be verified on the server.
//
// Design (full client-side, no homebrew — delivered in the payload at connect):
//   The game already syncs every player's race standing to every console each
//   frame via RKNetController's RACE packets. So the "who is in last" decision
//   can be computed DETERMINISTICALLY on each client from that already-synced
//   state at a well-defined boundary (a lap completion), with no new netcode.
//
// Remaining before this does anything (tracked in HANDOFF.md / TODO.md):
//   - M2: a validated Raceinfo import (players[] + RaceinfoPlayer.position) with
//     5-region addresses. The static RE notes in .phase0/raceinfo.h disagree with
//     the patcher's proven RaceManager offsets, so these MUST be confirmed in
//     Dolphin before use — wrong offsets crash the game. The reads are isolated
//     behind GetLivePositions() below so only that function changes.
//   - M3: apply elimination (force the kart to a DNF/spectator state).
//   - M4: elimination UI.
//   - M5: gate on an actual "Knockout Mode" room (a wl: flag from the server),
//     not just the renamed Continental button.

#if RMC

#  include "import/mkw/system/RaceManager.hpp"
#  include "wwfcGPReport.hpp"
#  include "wwfcPatch.hpp"
#  include "wwfcTypes.h"

namespace wwfc::Knockout
{

using namespace mkw;

namespace
{

// Whether this race is a knockout race. M5 will set this from the room/server;
// for now it is driven only by the explicit lifecycle calls below so the
// scaffold is inert in normal rooms.
constinit bool s_active = false;

// Finish order recorded as players cross the line, by player id, in order.
constinit u8  s_finishOrder[12] = {};
constinit u32 s_finishCount     = 0;

void ResetRace()
{
    s_finishCount = 0;
    for (u8& id : s_finishOrder) {
        id = 0xFF;
    }
}

} // namespace

// Called once when a knockout race begins. (Hook wired in M2 at the proven
// RACE-start transition used by mkwTime.cpp.)
void OnRaceStart()
{
    s_active = true;
    ResetRace();
}

// Called as each player crosses the finish line, in finish order. (Hook wired
// in M2 at the per-player finish point, its own instruction near mkwTime's
// FixRaceFinishTime hook.)
void OnPlayerFinished(u8 playerId)
{
    if (!s_active || s_finishCount >= 12) {
        return;
    }
    s_finishOrder[s_finishCount++] = playerId;
}

// Called when the race finishes. For M1 this only reports the finish order so
// the pipeline is observable on the server; M2 computes and applies the
// last-place elimination here.
void OnRaceFinished()
{
    if (!s_active) {
        return;
    }
    s_active = false;

    // Observable in the server log, like wl:mkw_finish_time.
    GPReport::ReportB64Encode(
        "wl:knockout_order", s_finishOrder, sizeof(s_finishOrder)
    );
}

} // namespace wwfc::Knockout

#endif // RMC
