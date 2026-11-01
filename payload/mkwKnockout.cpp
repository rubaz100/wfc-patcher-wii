// Knockout Mode — client-side elimination racing for mkwfc.
//
// Active PAL builds use the server-negotiated race plan and one authoritative
// owner for public/private VS. Guests consume ordered server relays; they never
// independently select victims. CPU slots participate in the same frozen grid.
// Ordinary races remain unaffected when the agreed Knockout flag is false.
//
// Elimination uses Vanish's kart-stop effect, then clears its newly added
// retirement flag so online VS does not convert it to a disconnect next frame.
// EndRace is deferred until the survivors finish. See docs/knockout-m3.md.

#if RMC

#  include "mkwKnockout.hpp"
#  include "mkwKnockoutNotice.hpp"
#  include "mkwKnockoutState.hpp"
#  include "mkwOnlineCPU.hpp"

#  include "import/mkw/system/RaceConfig.hpp"
#  include "import/mkw/system/RaceManager.hpp"
#  include "wwfcGPReport.hpp"
#  include "wwfcTypes.h"

#  ifndef WWFC_KNOCKOUT_APPLY
#    define WWFC_KNOCKOUT_APPLY 0
#  endif

#  if WWFC_KNOCKOUT_APPLY && !RMCPD00
#    error "M3 active elimination is verified only for PAL RMCPD00"
#  endif

#  if WWFC_KNOCKOUT_APPLY
#    include "import/mkw/net/NetManager.hpp"
#    include "import/mkw/system/KnockoutSpectator.hpp"
#    include "import/mkw/ui/page/WifiMenuPage.hpp"
#  endif

namespace wwfc::Knockout
{

using namespace mkw;

namespace
{

constinit RaceState    s_raceState;
constinit u32          s_finalBaseMs        = 0;
constinit wchar_t      s_notice[128]        = {};
constinit u32          s_noticeGeneration   = 0;
constinit u8           s_gridCount          = 0;
constinit RaceManager* s_boundRace          = nullptr;
constinit u32          s_appliedSequence    = 0;
constinit u16          s_retireEffectMask   = 0;
constinit u16          s_resultReportedMask = 0;

void Reset()
{
    s_raceState.reset();
    s_finalBaseMs        = 0;
    s_notice[0]          = 0;
    s_gridCount          = 0;
    s_boundRace          = nullptr;
    s_appliedSequence    = 0;
    s_retireEffectMask   = 0;
    s_resultReportedMask = 0;
}

#  if WWFC_KNOCKOUT_APPLY

u32 Milliseconds(const mkw::Time& time)
{
    return time.m_minutes * 60000u + time.m_seconds * 1000u + time.m_milliseconds;
}

bool ReadSnapshot(RaceManager& race, u8 count, Snapshot& snapshot)
{
    if (count < 2 || count > MAX_PLAYERS || !race.m_players || !race.m_playerIdInEachPosition) {
        return false;
    }
    snapshot             = {};
    snapshot.playerCount = count;
    for (u8 id = 0; id < count; ++id) {
        auto* player = race.m_players[id];
        if (!player || player->m_id != id) {
            return false;
        }
        snapshot.order[id] = race.m_playerIdInEachPosition[id];
        snapshot.laps[id]  = player->m_currentLap;
        if (player->m_stateFlags & RaceManager::Player::FLAG_FINISHED)
            snapshot.finishedMask |= 1u << id;
        if (player->m_stateFlags & RaceManager::Player::FLAG_DISCONNECTED)
            snapshot.disconnectedMask |= 1u << id;
    }
    return true;
}

u32 FinalBase(RaceManager& race, const Snapshot& snapshot)
{
    if (OnlineCPU::KnockoutEnabled()) {
        const u32 canonical = OnlineCPU::KnockoutFinalBase();
        if (canonical || OnlineCPU::OwnerAlive())
            return canonical;
        // If the authority disconnects before the final event, complete the
        // already eliminated racers after the remaining survivors finish.
        // This fallback affects only owner-loss teardown, not agreed results.
    }
    if (s_finalBaseMs)
        return s_finalBaseMs;
    const auto& elapsed = race.m_timer->m_time[0];
    u32         base    = IsPacketRaceTime(
                              elapsed.m_minutes, elapsed.m_seconds, elapsed.m_milliseconds, elapsed.m_valid
                          )
                              ? Milliseconds(elapsed)
                              : 0;
    for (u8 id = 0; id < snapshot.playerCount; ++id) {
        auto* player = race.m_players[id];
        if (!s_raceState.placement(id) && player->m_raceFinishTime &&
            (snapshot.finishedMask & (1u << id)) && !(snapshot.disconnectedMask & (1u << id))) {
            const auto& finish = *player->m_raceFinishTime;
            if (!IsPacketRaceTime(
                    finish.m_minutes, finish.m_seconds, finish.m_milliseconds, finish.m_valid
                )) {
                continue;
            }
            u32 time = Milliseconds(finish);
            if (time > base)
                base = time;
        }
    }
    s_finalBaseMs = base;
    return base;
}

u32 OwnerFinalBase(RaceManager& race, const Snapshot& snapshot)
{
    const auto& elapsed = race.m_timer->m_time[0];
    u32         base    = IsPacketRaceTime(
                              elapsed.m_minutes, elapsed.m_seconds, elapsed.m_milliseconds, elapsed.m_valid
                          )
                              ? Milliseconds(elapsed)
                              : 0;
    for (u8 id = 0; id < snapshot.playerCount; ++id) {
        const auto* player = race.m_players[id];
        if (!s_raceState.placement(id) && player->m_raceFinishTime &&
            (snapshot.finishedMask & (1u << id)) && !(snapshot.disconnectedMask & (1u << id))) {
            const auto& time = *player->m_raceFinishTime;
            if (IsPacketRaceTime(
                    time.m_minutes, time.m_seconds, time.m_milliseconds, time.m_valid
                ) &&
                Milliseconds(time) > base)
                base = Milliseconds(time);
        }
    }
    return base;
}

bool ApplyRetirementEffect(RaceManager& race, u8 id)
{
    auto& player = *race.m_players[id];
    // An authoritative decision remains valid if this peer's native state
    // has already finished or retired the racer. Preserve its bookkeeping.
    if (player.m_stateFlags &
        (RaceManager::Player::FLAG_FINISHED | RaceManager::Player::FLAG_DISCONNECTED |
         RaceManager::Player::FLAG_RETIRED))
        return true;
    if (!KartManagerForRetirement::CanRetire(id))
        return false;
    player.retire();
    const bool didRetire = (player.m_stateFlags & RaceManager::Player::FLAG_RETIRED) != 0;
    // This bit was absent on entry. Keep Vanish's kart-stop effect without
    // introducing the online VS race-level disconnect on the next frame.
    player.m_stateFlags &= ~RaceManager::Player::FLAG_RETIRED;
    return didRetire;
}

bool ApplyElimination(RaceManager& race, u8 id, u8 placement)
{
    // Record/consume the agreed event independently of transient kart readiness.
    // Retrying its native effect must not block subsequent ordered decisions.
    if (ApplyRetirementEffect(race, id))
        s_retireEffectMask |= 1u << id;
    s_raceState.markEliminated(id, placement);
    auto& config = RaceConfigManager::Instance()->getConfig();
    if (config.getPlayer(id)->m_type == RaceConfigPlayer::EPlayerType::MASTER) {
        if (FormatEliminationNotice(s_notice, placement)) {
            ++s_noticeGeneration;
            wchar_t result[128];
            if (FormatEliminationNotice(result, placement, false)) {
                UI::WifiMenuPage::QueueNotice(result);
            }
        }
    }
    return true;
}

void FollowSurvivor(const Snapshot& snapshot)
{
    if (!s_notice[0])
        return;
    u16 excluded = s_raceState.eliminatedMask() | snapshot.disconnectedMask | snapshot.finishedMask;
    for (u8 position = 0; position < snapshot.playerCount; ++position) {
        u8 id = snapshot.order[position];
        if (id < snapshot.playerCount && !(excluded & (1u << id))) {
            ExistingCameraManager::Follow(id);
            return;
        }
    }
}

void FinalizeRetired(RaceManager& race, const Snapshot& snapshot)
{
    const u16  survivors = ((1u << snapshot.playerCount) - 1) &
                           ~(s_raceState.eliminatedMask() | snapshot.disconnectedMask);
    const bool abandoned = !OnlineCPU::OwnerAlive() && !survivors && s_raceState.eliminatedMask();
    if (!race.m_timer || (!s_raceState.survivorsFinished(snapshot) && !abandoned))
        return;
    u32 base = FinalBase(race, snapshot);
    for (u8 id = 0; id < snapshot.playerCount; ++id) {
        auto* player    = race.m_players[id];
        u8    placement = s_raceState.placement(id);
        if (!placement || !player->m_raceFinishTime ||
            (player->m_stateFlags &
             (RaceManager::Player::FLAG_DISCONNECTED | RaceManager::Player::FLAG_RETIRED)) ||
            !(s_retireEffectMask & (1u << id)))
            continue;
        u32 ms = FinalPlacementTimeMs(base, placement);
        if (!ms)
            continue;
        auto finish           = race.m_timer->m_time[0];
        finish.m_minutes      = ms / 60000;
        finish.m_seconds      = (ms / 1000) % 60;
        finish.m_milliseconds = ms % 1000;
        finish.m_valid        = true;
        if (player->m_stateFlags & RaceManager::Player::FLAG_FINISHED) {
            player->m_raceFinishTime->m_minutes      = finish.m_minutes;
            player->m_raceFinishTime->m_seconds      = finish.m_seconds;
            player->m_raceFinishTime->m_milliseconds = finish.m_milliseconds;
            player->m_raceFinishTime->m_valid        = true;
        } else {
            player->endRace(finish);
        }
        if (!(s_resultReportedMask & (1u << id))) {
            GPReport::ReportU32("wl:ko_result", (static_cast<u32>(id) << 8) | placement);
            s_resultReportedMask |= 1u << id;
        }
    }
}
#  endif

} // namespace

void OnPlayerFinished(RaceManager::Player& player)
{
    // Pack id (high byte) + position (low byte) so the server log shows both.
    // On a real Wii this reports the local player's final rank; the last
    // finisher reports the highest position, which is exactly the elimination
    // input. Expected values: position in 1..playerCount.
    u32 record = (static_cast<u32>(player.m_id) << 8) | static_cast<u32>(player.m_position);
    GPReport::ReportU32("wl:ko_pos", record);
}

bool IsSpectatorTestRoom()
{
#  if WWFC_KNOCKOUT_APPLY
    auto* configs = RaceConfigManager::Instance();
    return configs && configs->getConfig().isWifiVSRace() &&
           configs->getConfig().localPlayerCount() == 1 && OnlineCPU::KnockoutEnabled();
#  else
    return false;
#  endif
}

bool IsEliminated(u8 id)
{
    return IsSpectatorTestRoom() && s_raceState.placement(id) != 0;
}

const wchar_t* EliminationNotice(u8 hudSlot)
{
    return hudSlot == 0 && IsSpectatorTestRoom() && s_notice[0] ? s_notice : nullptr;
}

u32 NoticeGeneration()
{
    return s_noticeGeneration;
}

bool NormalizeRetiredFinish(RaceManager::Player& player)
{
#  if WWFC_KNOCKOUT_APPLY
    auto* race = RaceManager::Instance();
    if (!IsSpectatorTestRoom() || !race || race != s_boundRace || !race->m_timer ||
        player.m_id >= s_gridCount || !player.m_raceFinishTime ||
        !(player.m_stateFlags & RaceManager::Player::FLAG_FINISHED) ||
        !s_raceState.placement(player.m_id))
        return false;
    Snapshot snapshot = {};
    if (!ReadSnapshot(*race, s_gridCount, snapshot) || !s_raceState.survivorsFinished(snapshot))
        return false;
    u32 ms = FinalPlacementTimeMs(FinalBase(*race, snapshot), s_raceState.placement(player.m_id));
    if (!ms)
        return false;
    player.m_raceFinishTime->m_minutes      = ms / 60000;
    player.m_raceFinishTime->m_seconds      = (ms / 1000) % 60;
    player.m_raceFinishTime->m_milliseconds = ms % 1000;
    player.m_raceFinishTime->m_valid        = true;
    return true;
#  else
    (void) player;
    return false;
#  endif
}

void Tick()
{
    RaceManager*       race    = RaceManager::Instance();
    RaceConfigManager* configs = RaceConfigManager::Instance();
    if (race == nullptr || configs == nullptr) {
        Reset();
        return;
    }
    RaceConfig& scenario = configs->getConfig();
    if (!scenario.isWifiVSRace()) {
        Reset();
        return;
    }
    u8 count = scenario.playerCount();
    if (count < 2 || count > MAX_PLAYERS) {
        Reset();
        return;
    }
    if (race->m_state != RaceManager::EState::RACE) {
        // Native EndPlayerRace can enter finishing stage 3 inside retirement
        // finalization. Preserve placements/times through stages 3/4 so the
        // finish-time hook and results handshake still have their KO state.
        const u32 stage = static_cast<u32>(race->m_state);
        if (race == s_boundRace && count == s_gridCount && stage >= 3 && stage <= 4 &&
            OnlineCPU::KnockoutEnabled())
            return;
        Reset();
        return;
    }
    if (s_boundRace != race || s_gridCount != count) {
        Reset();
        s_boundRace = race;
        s_gridCount = count;
    }
    if (race->m_players == nullptr || race->m_playerIdInEachPosition == nullptr) {
        return;
    }
    Snapshot snapshot    = {};
    snapshot.playerCount = count;
    for (u8 id = 0; id < count; ++id) {
        RaceManager::Player* player = race->m_players[id];
        if (player == nullptr || player->m_id != id) {
            return;
        }
        snapshot.order[id] = race->m_playerIdInEachPosition[id];
        snapshot.laps[id]  = player->m_currentLap;
        if ((player->m_stateFlags & RaceManager::Player::FLAG_FINISHED) != 0) {
            snapshot.finishedMask |= 1u << id;
        }
        if ((player->m_stateFlags & RaceManager::Player::FLAG_DISCONNECTED) != 0) {
            snapshot.disconnectedMask |= 1u << id;
        }
    }
#  if WWFC_KNOCKOUT_APPLY
    if (IsSpectatorTestRoom()) {
        s_raceState.bind(count);
        OnlineCPU::KnockoutEvent event;
        while (OnlineCPU::ReadKnockoutEvent(s_appliedSequence, event)) {
            if (event.playerId >= count ||
                !ApplyElimination(*race, event.playerId, event.placement))
                return;
            s_appliedSequence = event.sequence;
            const u32 record  = (static_cast<u32>(event.leaderLap) << 16) |
                                (static_cast<u32>(event.playerId) << 8) | count;
            GPReport::ReportU32("wl:ko_apply", record);
            if (scenario.getPlayer(event.playerId)->m_type == RaceConfigPlayer::EPlayerType::MASTER)
                GPReport::ReportU32("wl:ko_spectate", record);
        }
        for (u8 id = 0; id < count; ++id) {
            if (s_raceState.placement(id) && !(s_retireEffectMask & (1u << id)) &&
                ApplyRetirementEffect(*race, id))
                s_retireEffectMask |= 1u << id;
            if (s_raceState.placement(id) &&
                scenario.getPlayer(id)->m_type == RaceConfigPlayer::EPlayerType::MASTER &&
                !(race->m_players[id]->m_stateFlags &
                  (RaceManager::Player::FLAG_FINISHED | RaceManager::Player::FLAG_DISCONNECTED)))
                KeepSpectatorInputActive();
        }
        FollowSurvivor(snapshot);
        if (race->m_timer && s_raceState.survivorsFinished(snapshot) && OnlineCPU::IsOwner() &&
            OnlineCPU::OwnerAlive() && !OnlineCPU::KnockoutFinalBase() &&
            !OnlineCPU::KnockoutDecisionPending()) {
            const u32 base = OwnerFinalBase(*race, snapshot);
            if (base)
                OnlineCPU::PublishKnockoutFinal(base);
        }
        FinalizeRetired(*race, snapshot);
        if (race->m_state != RaceManager::EState::RACE || !OnlineCPU::IsOwner() ||
            !OnlineCPU::OwnerAlive() || OnlineCPU::KnockoutDecisionPending() ||
            OnlineCPU::KnockoutFinalBase())
            return;
        Decision decision = {};
        if (!s_raceState.evaluate(snapshot, decision))
            return;
        u8 placement = 1;
        for (u8 id = 0; id < count; ++id) {
            if (decision.survivors & (1u << id))
                ++placement;
        }
        OnlineCPU::PublishKnockout(decision.leaderLap, decision.playerId, placement);
        return;
    }
#  endif
    Decision decision = {};
    if (!s_raceState.evaluate(snapshot, decision)) {
        return;
    }
    u32 record = (static_cast<u32>(decision.leaderLap) << 16) |
                 (static_cast<u32>(decision.playerId) << 8) | count;
    GPReport::ReportU32("wl:ko_eliminate", record);
}

} // namespace wwfc::Knockout

#endif // RMC
