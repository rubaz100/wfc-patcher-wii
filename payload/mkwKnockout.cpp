// Knockout Mode — client-side elimination racing for mkwfc.
//
// STATUS: M2 elimination decision (leader-lap, last-out), report-only.
//   - M1 (done, validated): read a finishing player's live rank
//     (RaceManager::Player::m_position @ 0x20) and report wl:ko_pos.
//   - M2 (this): every frame during an online VS race, watch the leader's lap;
//     when the leader completes a lap, compute the current last-place player id
//     from Raceinfo::m_playerIdInEachPosition and report wl:ko_eliminate. Still
//     report-only — no one is eliminated yet — so the decision can be confirmed
//     in the server log (same de-risking as M1) before M3 applies it.
//
// Design (full client-side, no homebrew — delivered in the payload at connect):
//   The game syncs every player's race standing to every console each frame via
//   RKNetController's RACE packets, so the "who is in last" decision can be made
//   deterministically on each client from already-synced state at a well-defined
//   boundary (the leader completing a lap) — no new netcode.
//
// Remaining (tracked in HANDOFF.md / TODO.md):
//   - M3: apply elimination (force the eliminated kart to a DNF/spectator state).
//   - M4: elimination UI.
//   - M5: gate on a real "Knockout Mode" room (a wl: flag from the server),
//     not just the renamed Continental button.

#if RMC

#  include "mkwKnockout.hpp"

#  include "import/mkw/system/RaceConfig.hpp"
#  include "import/mkw/system/RaceManager.hpp"
#  include "wwfcGPReport.hpp"
#  include "wwfcTypes.h"

namespace wwfc::Knockout
{

using namespace mkw;

namespace
{

// Leader's lap at the last frame we evaluated, so we fire once per lap. Not yet
// armed until we have seen the first racing frame (to baseline against the
// leader's starting lap and re-baseline each new race).
constinit u16  s_lastLeaderLap = 0;
constinit bool s_armed         = false;

} // namespace

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

void Tick()
{
    RaceManager* raceManager = RaceManager::Instance();
    if (raceManager == nullptr || raceManager->m_state != RaceManager::EState::RACE) {
        // Not racing: re-baseline so the next race fires from its own lap 1.
        s_armed = false;
        return;
    }

    RaceConfig& scenario = RaceConfigManager::Instance()->getConfig();
    if (!scenario.isWifiVSRace()) {
        return;
    }

    u32 playerCount = scenario.playerCount();
    if (playerCount < 2) {
        return;
    }

    const u8*             order   = raceManager->m_playerIdInEachPosition;
    RaceManager::Player** players = raceManager->m_players;
    if (order == nullptr || players == nullptr) {
        return;
    }

    RaceManager::Player* leader = players[order[0]];
    if (leader == nullptr) {
        return;
    }
    u16 leaderLap = leader->m_currentLap;

    if (!s_armed) {
        // First racing frame we see: baseline the leader's lap so we only fire
        // on subsequent lap completions.
        s_lastLeaderLap = leaderLap;
        s_armed         = true;
        return;
    }

    if (leaderLap > s_lastLeaderLap) {
        s_lastLeaderLap = leaderLap;

        u8 lastPlaceId = order[playerCount - 1];

        // report-only: pack the leader's new lap, the last-place player id, and
        // the player count so the decision can be decoded from the server log
        // (record = (leaderLap << 16) | (lastPlaceId << 8) | playerCount).
        u32 record = (static_cast<u32>(leaderLap) << 16) |
                     (static_cast<u32>(lastPlaceId) << 8) | playerCount;
        GPReport::ReportU32("wl:ko_eliminate", record);
    }
}

} // namespace wwfc::Knockout

#endif // RMC
