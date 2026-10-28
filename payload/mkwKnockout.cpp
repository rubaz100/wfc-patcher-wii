// Knockout Mode — client-side elimination racing for mkwfc.
//
// STATUS: M2 position sanity-check. This reads a finishing player's live rank
// (RaceManager::Player::m_position, offset 0x20) and reports it so it can be
// confirmed in the server log before it is used to drive elimination. No one is
// eliminated yet.
//
// Design (full client-side, no homebrew — delivered in the payload at connect):
//   The game syncs every player's race standing to every console each frame via
//   RKNetController's RACE packets, so the "who is in last" decision can be made
//   deterministically on each client from already-synced state at a well-defined
//   boundary (a lap completion) — no new netcode.
//
// Remaining after the sanity-check (tracked in HANDOFF.md / TODO.md):
//   - M2: compute eliminations (last place at each lap boundary) from m_position.
//   - M3: apply elimination (force the kart to a DNF/spectator state).
//   - M4: elimination UI.
//   - M5: gate on a real "Knockout Mode" room (a wl: flag from the server),
//     not just the renamed Continental button.

#if RMC

#  include "mkwKnockout.hpp"

#  include "import/mkw/system/RaceManager.hpp"
#  include "wwfcGPReport.hpp"
#  include "wwfcTypes.h"

namespace wwfc::Knockout
{

using namespace mkw;

void OnPlayerFinished(RaceManager::Player& player)
{
    // Pack id (high byte) + position (low byte) so the server log shows both.
    // On a real Wii this reports the local player's final rank; the last
    // finisher reports the highest position, which is exactly the elimination
    // input. Expected values: position in 1..playerCount.
    u32 record = (static_cast<u32>(player.m_id) << 8) |
                 static_cast<u32>(player.m_position);
    GPReport::ReportU32("wl:ko_pos", record);
}

} // namespace wwfc::Knockout

#endif // RMC
