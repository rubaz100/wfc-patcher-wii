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
#  include "import/revolution.h"
#  include "wwfcGPReport.hpp"
#  include "wwfcPatch.hpp"
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
#    include "import/mkw/system/KnockoutSpectatorInput.hpp"
#    include "import/mkw/ui/page/WifiMenuPage.hpp"
#  endif

namespace wwfc::Knockout
{

using namespace mkw;

#  if WWFC_KNOCKOUT_APPLY
[[gnu::longcall]] bool NativeIsOnlineVSInputIdle(void*, u32, u32, u32) AT(0x805235D4);
[[gnu::longcall]] bool NativeCheckOnlineVSLowSpeed(void*) AT(0x8053EE50);
[[gnu::longcall]] bool NativeCheckOnlineVSProgress(void*) AT(0x8053EFD4);
[[gnu::longcall]] bool NativeCanOnlineVSRaceEnd(void*) AT(0x8053FA10);
[[gnu::longcall]] bool NativeIsConnectedToAnyone(NetManager*) AT(0x80656F00);
[[gnu::longcall]] bool NativeOnlineVSFinishRule(void*, u32, u32) AT(0x8053FB38);

WWFC_DEFINE_CTR_STUB(
    0x8053EC50, bool NativeFinishGracePeriod(void*), stwu r1, -0x20(r1); mflr r0; lis r4, 0x809C;
    stw r0, 0x24(r1);
)

// Original PAL constructors/leaf entry words, before the replacement jump.
// The relocated constant bases in the player constructor are 0x8089xxxx.
WWFC_DEFINE_CTR_STUB(
    0x80533EE8, RaceManager::Player* NativeConstructRacePlayer(RaceManager::Player*, u32, u32),
    stwu r1, -0x20(r1);
    mflr r0; lis r6, 0x8089; lis r7, 0x8089;
)
WWFC_DEFINE_CTR_STUB(
    0x805349C8, void NativeEndLap(RaceManager::Player*), stwu r1, -0x50(r1); mflr r0;
    stw r0, 0x54(r1); addi r11, r1, 0x50;
)
WWFC_DEFINE_CTR_STUB_SAVE_LR(
    0x805336B4, u32 NativeLapCount(), lis r4, 0x809C; li r3, 3; lwz r4, -0x28D8(r4);
    lwz r0, 0xB90(r4);
)
#  endif

namespace
{

constinit RaceState    s_raceState;
constinit u32          s_finalBaseMs        = 0;
constinit wchar_t      s_notice[128]        = {};
constinit u32          s_noticeGeneration   = 0;
constinit u8           s_gridCount          = 0;
constinit RaceManager* s_boundRace          = nullptr;
constinit u32          s_boundEpoch         = 0;
constinit u16          s_carriedMask        = 0;
constinit bool         s_carriedSeeded      = false;
constinit bool         s_raceStarted        = false;
constinit u32          s_appliedSequence    = 0;
constinit u16          s_retireEffectMask   = 0;
constinit u16          s_resultReportedMask = 0;
constinit u8           s_afkReportedMask    = 0;
constinit bool         s_stallGuardReported = false;
constinit u8           s_spectatorPlayer    = MAX_PLAYERS;
constinit u8           s_spectatorTarget    = MAX_PLAYERS;
constinit bool         s_followLeader       = true;
constinit bool         s_inputReady         = false;
constinit u32          s_previousInput      = 0;
constinit u8           s_noticePlacement    = 0;
constinit u8           s_noticeTarget       = 0;
constinit bool         s_noticeCPU          = false;
constinit bool         s_noticeAutomatic    = true;

void Reset()
{
    s_raceState.reset();
    s_finalBaseMs        = 0;
    s_notice[0]          = 0;
    s_gridCount          = 0;
    s_boundRace          = nullptr;
    s_boundEpoch         = 0;
    s_carriedMask        = 0;
    s_carriedSeeded      = false;
    s_raceStarted        = false;
    s_appliedSequence    = 0;
    s_retireEffectMask   = 0;
    s_resultReportedMask = 0;
    s_afkReportedMask    = 0;
    s_stallGuardReported = false;
    s_spectatorPlayer    = MAX_PLAYERS;
    s_spectatorTarget    = MAX_PLAYERS;
    s_followLeader       = true;
    s_inputReady         = false;
    s_previousInput      = 0;
    s_noticePlacement    = 0;
    s_noticeTarget       = 0;
    s_noticeCPU          = false;
    s_noticeAutomatic    = true;
}

#  if WWFC_KNOCKOUT_APPLY

void PublishNotice(const wchar_t (&notice)[128])
{
    // UI glyph construction reads a separate snapshot. Only the fixed copy
    // and generation commit run with interrupts disabled, never UI/SDK work.
    const bool interrupts = RVL::OSDisableInterrupts();
    std::memcpy(s_notice, notice, sizeof(s_notice));
    ++s_noticeGeneration;
    RVL::OSRestoreInterrupts(interrupts);
}

bool IsLocalSpectator(RaceManager& race, RaceConfig& config)
{
    if (!IsSpectatorTestRoom() || &race != s_boundRace ||
        race.m_state != RaceManager::EState::RACE || config.playerCount() != s_gridCount ||
        s_spectatorPlayer >= s_gridCount || !race.m_players ||
        config.getPlayer(s_spectatorPlayer)->m_type != RaceConfigPlayer::EPlayerType::MASTER)
        return false;
    const auto* player = race.m_players[s_spectatorPlayer];
    return player && player->m_id == s_spectatorPlayer &&
           s_raceState.placement(s_spectatorPlayer) &&
           !(player->m_stateFlags &
             (RaceManager::Player::FLAG_FINISHED | RaceManager::Player::FLAG_DISCONNECTED |
              RaceManager::Player::FLAG_RETIRED));
}

bool IsOnlineVSInputIdle(void* input, u32 controllerIndex, u32 idleLimit, u32 unchangedLimit)
{
    const bool idle = NativeIsOnlineVSInputIdle(input, controllerIndex, idleLimit, unchangedLimit);
    if (!idle || controllerIndex > 3 || !IsSpectatorTestRoom())
        return idle;
    // The native check can expire before the first KO decision. Suppress only
    // this input-idle decision for the agreed KO race, leaving the separate
    // hard race limit and ordinary network/disconnect handling intact.
    const u8 bit = 1u << controllerIndex;
    if (!(s_afkReportedMask & bit) && GPReport::ReportU32("wl:ko_afk", controllerIndex))
        s_afkReportedMask |= bit;
    return false;
}

void CheckOnlineVSStalling(void* mode)
{
    auto* race    = RaceManager::Instance();
    auto* configs = RaceConfigManager::Instance();
    if (race && configs && IsLocalSpectator(*race, configs->getConfig())) {
        // Vanish intentionally stops an eliminated kart. Native low-speed and
        // no-progress checks would treat that stationary spectator as idle.
        // This report records bypassing both checks, not either one expiring.
        if (!s_stallGuardReported &&
            GPReport::ReportU32("wl:ko_stall_guard", (3u << 8) | s_spectatorPlayer))
            s_stallGuardReported = true;
        return;
    }
    NativeCheckOnlineVSLowSpeed(mode);
    NativeCheckOnlineVSProgress(mode);
}

bool FinishGracePeriod(void* mode)
{
    auto* race    = RaceManager::Instance();
    auto* configs = RaceConfigManager::Instance();
    if (OnlineCPU::RaceKnockoutEnabled() && !OnlineCPU::KnockoutRoundCommitted() && race &&
        configs && IsLocalSpectator(*race, configs->getConfig()))
        return false;
    return NativeFinishGracePeriod(mode);
}

u32 Milliseconds(const mkw::Time& time)
{
    return time.m_minutes * 60000u + time.m_seconds * 1000u + time.m_milliseconds;
}

RaceManager::Player* ConstructRacePlayer(RaceManager::Player* player, u32 id, u32 nativeLaps)
{
    const u8 laps = ContinuousLapCount();
    if (laps)
        nativeLaps = laps;
    else if (OnlineCPU::RaceKnockoutEnabled())
        nativeLaps = 3;
    auto* configs = RaceConfigManager::Instance();
    if (configs && configs->getConfig().isWifiVSRace())
        // Native scenario Init can retain the previous KO lap byte. Match
        // metadata to the effective constructor bound on ordinary VS too,
        // preserving the native competition choice instead of assuming three.
        configs->getConfig().configureOnlineLapCount(static_cast<u8>(nativeLaps));
    // The native constructor allocates lapCount * sizeof(Time) entries, so
    // every later lap-split write has a matching allocation from the start.
    return NativeConstructRacePlayer(player, id, nativeLaps);
}

u32 LapCount()
{
    if (OnlineCPU::RaceKnockoutEnabled())
        return 3;
    const u8 laps = ContinuousLapCount();
    return laps ? laps : NativeLapCount();
}

u32 EndLapLimit(u32 nativeLaps)
{
    if (OnlineCPU::RaceKnockoutEnabled())
        return 3;
    const u8 laps = ContinuousLapCount();
    return laps ? laps : nativeLaps;
}

void EndLap(RaceManager::Player* player)
{
    const u8 laps = ContinuousLapCount();
    if (laps && player && player->m_currentLap >= laps) {
        // Hold the final allocated lap while ordered decisions/winner commit
        // drain. RaceState consumes any crossed boundaries missed during an
        // acknowledgement. Never advance into an unallocated split or let a
        // stock finish substitute for the canonical winner event.
        *reinterpret_cast<u8*>(reinterpret_cast<u8*>(player) + 0x28) = 0;
        return;
    }
    NativeEndLap(player);
}

u32 RaceTimeBudget()
{
    const u8 laps = ContinuousLapCount();
    return 300000u * (laps ? laps : 1);
}

bool RaceFrameBudgetExpired()
{
    const auto* race = RaceManager::Instance();
    if (!race)
        return false;
    const u8  laps   = ContinuousLapCount();
    const u32 budget = laps ? laps * 18000u + 3600u : 21605u;
    return reinterpret_cast<const u32*>(race)[0x20 / sizeof(u32)] > budget;
}

bool CanOnlineVSRaceEnd(void* mode)
{
    const auto* race          = RaceManager::Instance();
    auto*       network       = NetManager::Instance();
    const bool awaitingCommit = OnlineCPU::RaceKnockoutEnabled()
                                    ? !OnlineCPU::KnockoutRoundCommitted()
                                    : ContinuousLapCount() && OnlineCPU::KnockoutWinnerId() == 0xFF;
    if (awaitingCommit && race && race->m_state == RaceManager::EState::RACE &&
        OnlineCPU::OwnerAlive() && network && NativeIsConnectedToAnyone(network) && race->m_timer &&
        Milliseconds(race->m_timer->m_time[0]) <= RaceTimeBudget() && !RaceFrameBudgetExpired())
        return false;
    return NativeCanOnlineVSRaceEnd(mode);
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
    if (OnlineCPU::KnockoutWinnerId() != 0xFF)
        snapshot.disconnectedMask |= OnlineCPU::KnockoutFinalDisconnectedMask();
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

bool ApplyElimination(RaceManager& race, u8 id, u8 placement, bool queueNotice = true)
{
    // Record/consume the agreed event independently of transient kart readiness.
    // Retrying its native effect must not block subsequent ordered decisions.
    if (ApplyRetirementEffect(race, id))
        s_retireEffectMask |= 1u << id;
    s_raceState.markEliminated(id, placement);
    auto& config = RaceConfigManager::Instance()->getConfig();
    if (config.getPlayer(id)->m_type == RaceConfigPlayer::EPlayerType::MASTER) {
        s_spectatorPlayer   = id;
        s_spectatorTarget   = MAX_PLAYERS;
        s_followLeader      = true;
        s_inputReady        = false;
        s_previousInput     = 0;
        wchar_t notice[128] = {};
        if (FormatEliminationNotice(notice, placement)) {
            PublishNotice(notice);
            wchar_t result[128];
            if (queueNotice && FormatEliminationNotice(result, placement, false)) {
                UI::WifiMenuPage::QueueNotice(result);
            }
        }
    }
    return true;
}

u8 CycleSpectatorTarget(const Snapshot& snapshot, u16 eligible, u8 current, bool next)
{
    u8 start = next ? snapshot.playerCount - 1 : 0;
    for (u8 position = 0; position < snapshot.playerCount; ++position) {
        if (snapshot.order[position] == current) {
            start = position;
            break;
        }
    }
    for (u8 step = 1; step <= snapshot.playerCount; ++step) {
        const u8 position =
            (start + (next ? step : snapshot.playerCount - step)) % snapshot.playerCount;
        const u8 id = snapshot.order[position];
        if (eligible & (1u << id))
            return id;
    }
    return MAX_PLAYERS;
}

void UpdateSpectatorNotice(u8 targetNumber, bool cpu)
{
    const u8 placement = s_raceState.placement(s_spectatorPlayer);
    if (s_notice[0] && placement == s_noticePlacement && targetNumber == s_noticeTarget &&
        cpu == s_noticeCPU && s_followLeader == s_noticeAutomatic)
        return;
    wchar_t notice[128] = {};
    if (FormatSpectatorNotice(notice, placement, targetNumber, cpu, s_followLeader)) {
        PublishNotice(notice);
        s_noticePlacement = placement;
        s_noticeTarget    = targetNumber;
        s_noticeCPU       = cpu;
        s_noticeAutomatic = s_followLeader;
    }
}

void FollowSurvivor(RaceManager& race, RaceConfig& config, const Snapshot& snapshot)
{
    if (!IsLocalSpectator(race, config)) {
        s_inputReady    = false;
        s_previousInput = 0;
        return;
    }
    // Native race ordering can be transient during setup/teardown. Never use
    // an incomplete permutation to select or index a camera target.
    u16 seen = 0;
    for (u8 position = 0; position < snapshot.playerCount; ++position) {
        const u8 id = snapshot.order[position];
        if (id >= snapshot.playerCount || (seen & (1u << id)))
            return;
        seen |= 1u << id;
    }
    const u16 eligible =
        seen & ~(s_raceState.eliminatedMask() | snapshot.disconnectedMask | snapshot.finishedMask);
    const auto input   = ReadSpectatorInput(config.getPlayer(s_spectatorPlayer)->m_inputIndex);
    u32        pressed = 0;
    if (input.available) {
        if (s_inputReady)
            pressed = input.held & ~s_previousInput;
        s_previousInput = input.held;
        s_inputReady    = true;
    } else {
        s_previousInput = 0;
        s_inputReady    = false;
    }
    if (!eligible) {
        s_spectatorTarget = MAX_PLAYERS;
        UpdateSpectatorNotice(0, false);
        return;
    }
    if (s_followLeader || s_spectatorTarget >= snapshot.playerCount ||
        !(eligible & (1u << s_spectatorTarget)))
        s_spectatorTarget = CycleSpectatorTarget(snapshot, eligible, MAX_PLAYERS, true);
    if (pressed & SpectatorInput::LEADER) {
        s_followLeader    = true;
        s_spectatorTarget = CycleSpectatorTarget(snapshot, eligible, MAX_PLAYERS, true);
    } else if (
        (input.held & (SpectatorInput::PREVIOUS | SpectatorInput::NEXT)) !=
            (SpectatorInput::PREVIOUS | SpectatorInput::NEXT) &&
        bool(pressed & SpectatorInput::PREVIOUS) != bool(pressed & SpectatorInput::NEXT)
    ) {
        s_followLeader    = false;
        s_spectatorTarget = CycleSpectatorTarget(
            snapshot, eligible, s_spectatorTarget, (pressed & SpectatorInput::NEXT) != 0
        );
    }
    if (s_spectatorTarget >= snapshot.playerCount ||
        !ExistingCameraManager::Follow(s_spectatorTarget))
        return;
    const bool cpu    = OnlineCPU::IsCPUSlot(s_spectatorTarget);
    u8         number = s_spectatorTarget + 1;
    if (cpu) {
        const auto* plan = OnlineCPU::CurrentPlan();
        if (!plan || s_spectatorTarget < plan->humanCount)
            return;
        number = s_spectatorTarget - plan->humanCount + 1;
    }
    UpdateSpectatorNotice(number, cpu);
}

void FinalizeCourseDNFSurvivors(RaceManager& race, const Snapshot& snapshot)
{
    const auto* plan  = OnlineCPU::CurrentPlan();
    const u32   stage = static_cast<u32>(race.m_state);
    const u32   base  = OnlineCPU::KnockoutFinalBase();
    if (!OnlineCPU::RaceKnockoutEnabled() || !OnlineCPU::KnockoutRoundCommitted() ||
        &race != s_boundRace || !plan || plan->epoch != s_boundEpoch ||
        snapshot.playerCount != s_gridCount || stage < 2 || stage > 4 || !race.m_timer || !base ||
        base >= 63u * 60000u)
        return;
    const u16 dnf         = OnlineCPU::KnockoutCourseDNFMask() & ~s_raceState.eliminatedMask() &
                            ~snapshot.disconnectedMask;
    auto      finish      = race.m_timer->m_time[0];
    finish.m_minutes      = base / 60000;
    finish.m_seconds      = (base / 1000) % 60;
    finish.m_milliseconds = base % 1000;
    finish.m_valid        = true;
    for (u8 id = 0; id < snapshot.playerCount; ++id) {
        auto* player = race.m_players[id];
        if (!(dnf & (1u << id)) || !player || !player->m_raceFinishTime)
            continue;
        // Native DNF times use minute 99, which cannot be carried by an
        // online finished frame. F supplies the same valid replacement on
        // every peer, including guests that never received a finished frame.
        // Eliminated DNF racers retain their series placement time below.
        if (player->m_stateFlags & RaceManager::Player::FLAG_FINISHED) {
            player->m_raceFinishTime->m_minutes      = finish.m_minutes;
            player->m_raceFinishTime->m_seconds      = finish.m_seconds;
            player->m_raceFinishTime->m_milliseconds = finish.m_milliseconds;
            player->m_raceFinishTime->m_valid        = true;
        } else {
            player->endRace(finish);
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

void ApplyCanonicalWinner(RaceManager& race, const Snapshot& snapshot)
{
    const u8  winner = OnlineCPU::KnockoutWinnerId();
    const u32 base   = OnlineCPU::KnockoutFinalBase();
    if (winner >= snapshot.playerCount || !base || !race.m_timer ||
        s_raceState.soleSurvivor(snapshot) != winner)
        return;
    auto* player = race.m_players[winner];
    if (!player || !player->m_raceFinishTime ||
        (player->m_stateFlags &
         (RaceManager::Player::FLAG_FINISHED | RaceManager::Player::FLAG_DISCONNECTED)))
        return;
    auto finish           = race.m_timer->m_time[0];
    finish.m_minutes      = base / 60000;
    finish.m_seconds      = (base / 1000) % 60;
    finish.m_milliseconds = base % 1000;
    finish.m_valid        = true;
    player->endRace(finish);
}

void FinishCommittedCourse(RaceManager& race, const Snapshot& snapshot)
{
    if (!OnlineCPU::RaceKnockoutEnabled() || !OnlineCPU::KnockoutRoundCommitted() ||
        &race != s_boundRace || race.m_state != RaceManager::EState::RACE || !race.m_timer ||
        !race.m_timer->m_time[0].m_valid || !Milliseconds(race.m_timer->m_time[0]) ||
        !OnlineCPU::KnockoutFinalBase() || !s_raceState.racersResolved(snapshot, 0))
        return;
    auto* mode = reinterpret_cast<void**>(&race)[0x10 / sizeof(void*)];
    // Every natural EndPlayerRace already ran its counter/input bookkeeping.
    // The first course can have no parked karts left to finish after F arrives.
    // Reproduce only native 80533D1C..80533D68's final CanRaceEnd/vf24 success
    // branch: for a fully resolved grid vf24 has no local-player side effects.
    if (mode && NativeCanOnlineVSRaceEnd(mode) &&
        NativeOnlineVSFinishRule(mode, snapshot.playerCount, snapshot.playerCount))
        race.m_state = RaceManager::EState::FINISHED;
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

u8 ContinuousLapCount()
{
#  if WWFC_KNOCKOUT_APPLY
    auto*       configs = RaceConfigManager::Instance();
    const auto* plan    = OnlineCPU::CurrentPlan();
    if (!configs || !plan || !IsSpectatorTestRoom() || OnlineCPU::RaceKnockoutEnabled())
        return 0;
    const u8 count = configs->getConfig().playerCount();
    return count >= 2 && count <= MAX_PLAYERS && count == plan->humanCount + plan->cpuCount ? count
                                                                                            : 0;
#  else
    return 0;
#  endif
}

bool ContinuousRaceInfo(u8& remaining, u8& total, u8& winner)
{
    remaining = 0;
    total     = 0;
    winner    = 0xFF;
#  if WWFC_KNOCKOUT_APPLY
    auto*    race    = RaceManager::Instance();
    auto*    configs = RaceConfigManager::Instance();
    const u8 count   = configs ? configs->getConfig().playerCount() : 0;
    if (!IsSpectatorTestRoom() || !race || race != s_boundRace || !count || count != s_gridCount)
        return false;
    if (OnlineCPU::RaceKnockoutEnabled() && !s_carriedSeeded)
        return false;
    Snapshot snapshot = {};
    if (!ReadSnapshot(*race, count, snapshot))
        return false;
    const u16 survivors = s_raceState.survivingMask(snapshot);
    for (u8 id = 0; id < count; ++id) {
        if (survivors & (1u << id))
            ++remaining;
    }
    total  = count;
    winner = OnlineCPU::KnockoutWinnerId();
    return true;
#  else
    return false;
#  endif
}

bool IsEliminated(u8 id)
{
    return EliminationPlacement(id) != 0;
}

u8 EliminationPlacement(u8 id)
{
    return IsSpectatorTestRoom() ? s_raceState.placement(id) : 0;
}

const wchar_t* EliminationNotice(u8 hudSlot)
{
#  if WWFC_KNOCKOUT_APPLY
    auto* race   = RaceManager::Instance();
    auto* config = RaceConfigManager::Instance();
    return hudSlot == 0 && race && config && IsLocalSpectator(*race, config->getConfig()) &&
                   s_notice[0]
               ? s_notice
               : nullptr;
#  else
    (void) hudSlot;
    return nullptr;
#  endif
}

bool CopyEliminationNotice(u8 hudSlot, wchar_t (&output)[128], u32& generation)
{
    output[0]  = 0;
    generation = 0;
    if (!EliminationNotice(hudSlot))
        return false;
    const bool interrupts = RVL::OSDisableInterrupts();
    std::memcpy(output, s_notice, sizeof(output));
    generation = s_noticeGeneration;
    RVL::OSRestoreInterrupts(interrupts);
    return output[0] != 0;
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
    bool beforeRace = false;
    bool afterRace  = false;
    u32  epoch      = 0;
#  if WWFC_KNOCKOUT_APPLY
    const bool  raceMode = OnlineCPU::RaceKnockoutEnabled();
    const auto* plan     = OnlineCPU::CurrentPlan();
    epoch                = plan ? plan->epoch : 0;
    beforeRace           = raceMode && static_cast<u32>(race->m_state) < 2;
    afterRace =
        raceMode && static_cast<u32>(race->m_state) >= 3 && static_cast<u32>(race->m_state) <= 4;
#  endif
    if (race->m_state != RaceManager::EState::RACE && !beforeRace && !afterRace) {
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
    if (s_boundRace != race || s_gridCount != count || s_boundEpoch != epoch) {
        Reset();
        s_boundRace  = race;
        s_gridCount  = count;
        s_boundEpoch = epoch;
    }
    if (race->m_players == nullptr || race->m_playerIdInEachPosition == nullptr) {
        return;
    }
    Snapshot snapshot = {};
#  if WWFC_KNOCKOUT_APPLY
    if (!ReadSnapshot(*race, count, snapshot))
        return;
#  else
    snapshot.playerCount = count;
    for (u8 id = 0; id < count; ++id) {
        const auto* player = race->m_players[id];
        if (!player || player->m_id != id)
            return;
        snapshot.order[id] = race->m_playerIdInEachPosition[id];
        snapshot.laps[id]  = player->m_currentLap;
        if (player->m_stateFlags & RaceManager::Player::FLAG_FINISHED)
            snapshot.finishedMask |= 1u << id;
        if (player->m_stateFlags & RaceManager::Player::FLAG_DISCONNECTED)
            snapshot.disconnectedMask |= 1u << id;
    }
#  endif
#  if WWFC_KNOCKOUT_APPLY
    if (IsSpectatorTestRoom()) {
        s_raceState.bind(count);
        if (raceMode && !s_carriedSeeded) {
            if (!OnlineCPU::RaceKnockoutRosterReady())
                return;
            u16 mask = 0, places = 0;
            for (u8 id = 0; id < count; ++id) {
                const u8 placement = OnlineCPU::InitialKnockoutPlacement(id);
                if (!placement)
                    continue;
                if (placement < 2 || placement > count || (places & (1u << placement)))
                    return;
                mask |= 1u << id;
                places |= 1u << placement;
            }
            if (mask != OnlineCPU::InitialKnockoutMask() || mask == (1u << count) - 1)
                return;
            s_carriedMask = mask;
            for (u8 id = 0; id < count; ++id) {
                const u8 placement = OnlineCPU::InitialKnockoutPlacement(id);
                if (placement && !ApplyElimination(*race, id, placement, false))
                    return;
            }
            s_carriedSeeded = true;
        }
        if (!beforeRace && !s_raceStarted) {
            // Native kart/countdown initialization may reset the stop effect.
            // Reapply carried retirements once at the first racing frame.
            s_retireEffectMask &= ~s_carriedMask;
            s_raceStarted = true;
        }
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
        }
        if (beforeRace)
            return;
        FollowSurvivor(*race, scenario, snapshot);
        if (raceMode) {
            // Race Knockout resolves only after the entire active field has
            // finished or disconnected. Carried spectators never count as
            // unfinished contenders or as this course's new victims.
            if (race->m_timer && s_raceState.racersResolved(snapshot, s_carriedMask) &&
                !OnlineCPU::KnockoutRoundCommitted()) {
                if (OnlineCPU::IsOwner() && OnlineCPU::OwnerAlive()) {
                    const u32 base = OwnerFinalBase(*race, snapshot);
                    if (base)
                        OnlineCPU::PublishRaceKnockoutResult(base);
                } else if (OnlineCPU::OwnerAlive()) {
                    OnlineCPU::RequestRaceKnockoutCommit();
                }
            }
            if (OnlineCPU::KnockoutRoundCommitted()) {
                FinalizeCourseDNFSurvivors(*race, snapshot);
                if (!ReadSnapshot(*race, count, snapshot))
                    return;
                FinalizeRetired(*race, snapshot);
                if (ReadSnapshot(*race, count, snapshot))
                    FinishCommittedCourse(*race, snapshot);
            }
            return;
        }
        const u8 survivor = s_raceState.soleSurvivor(snapshot);
        if (race->m_timer && survivor < count && OnlineCPU::IsOwner() && OnlineCPU::OwnerAlive() &&
            !OnlineCPU::KnockoutFinalBase() && !OnlineCPU::KnockoutDecisionPending()) {
            const u32 base = OwnerFinalBase(*race, snapshot);
            if (base)
                OnlineCPU::PublishKnockoutWinner(
                    base, survivor, snapshot.disconnectedMask & ~s_raceState.eliminatedMask()
                );
        }
        ApplyCanonicalWinner(*race, snapshot);
        if (!ReadSnapshot(*race, count, snapshot))
            return;
        FinalizeRetired(*race, snapshot);
        if (race->m_state != RaceManager::EState::RACE || !OnlineCPU::IsOwner() ||
            !OnlineCPU::OwnerAlive() || OnlineCPU::KnockoutDecisionPending() ||
            OnlineCPU::KnockoutFinalBase())
            return;
        Decision decision = {};
        if (!s_raceState.evaluate(snapshot, decision))
            return;
        // Naturally finished racers still occupy places ahead of the next
        // victim. Decision::survivors contains only unfinished racers, so it
        // cannot determine the final placement on its own.
        u8        placement = 0;
        const u16 excluded  = s_raceState.eliminatedMask() | snapshot.disconnectedMask;
        for (u8 id = 0; id < count; ++id) {
            if (!(excluded & (1u << id)))
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

#  if WWFC_KNOCKOUT_APPLY
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x80533ED8, ConstructRacePlayer);
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x805349B8, EndLap);
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x805336A4, LapCount);
// A carried spectator intentionally stays unfinished until the course commit;
// do not turn it into a 30-second DNF while active entrants are still racing.
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8053EC40, FinishGracePeriod);
// GMDataOnlineVS::CanRaceEnd, including its no-network-peer early return.
// A committed KO winner owns completion while the authority is still alive.
WWFC_DEFINE_PATCH = Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808B3930, CanOnlineVSRaceEnd);

// EndLap's shared comparison entry. Preserve the race-config pointer in r5;
// replay the comparison and its alternate successor outside this block.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x80534BD8,
    ASM_LAMBDA(
        ( : ASM_IMPORT(i, EndLapLimit)),
        // clang-format off
        stwu r1, -0x20(r1);
        mflr r0;
        stw r0, 0x24(r1);
        stw r5, 0x08(r1);
        mr r3, r4;
        bl %[EndLapLimit];
        mr r0, r3;
        lwz r5, 0x08(r1);
        lwz r12, 0x24(r1);
        mtlr r12;
        addi r1, r1, 0x20;
        lbz r3, 0x26(r29);
        cmplw r3, r0;
        bge 1f;
        lis r12, 0x8053;
        ori r12, r12, 0x4C58;
        mtctr r12;
        bctr;
        1: blr;
        // clang-format on
    )
);

// Replace only the inline time-budget argument setup, then restore the native
// Raceinfo/timer pointers. Ordinary VS retains its five-minute limit.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x8053F3B4,
    ASM_LAMBDA(
        ( : ASM_IMPORT(i, RaceTimeBudget)),
        // clang-format off
        stwu r1, -0x20(r1);
        mflr r0;
        stw r0, 0x24(r1);
        bl %[RaceTimeBudget];
        mr r4, r3;
        lwz r5, 0x04(r31);
        lwz r6, 0x14(r5);
        lwz r0, 0x24(r1);
        mtlr r0;
        addi r1, r1, 0x20;
        blr;
        // clang-format on
    )
);
// Preserve the following native branch and its sole GM+114 write. KO's frame
// budget is its time budget plus sixty seconds, bounded below 63 minutes.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x8053F468,
    ASM_LAMBDA(
        ( : ASM_IMPORT(i, RaceFrameBudgetExpired)),
        // clang-format off
        stwu r1, -0x20(r1);
        mflr r0;
        stw r0, 0x24(r1);
        bl %[RaceFrameBudgetExpired];
        lwz r0, 0x24(r1);
        mtlr r0;
        addi r1, r1, 0x20;
        cmplwi r3, 0;
        blr;
        // clang-format on
    )
);

// Preserve the complete native mr/bl/mr/bl pair. Both incoming branches target
// its first word at 0x8053F444; none enters the interior. The displaced BL
// relocations reference the two original watchdogs, and no symbol, incoming
// relocation, or data pointer targets the interior. The following reload at
// 0x8053F454 and the independent input/time/network checks remain native.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x8053F444,
    ASM_LAMBDA(
        ( : ASM_IMPORT(i, CheckOnlineVSStalling)),
        // clang-format off
        mr r3, r31;
        b %[CheckOnlineVSStalling];
        // clang-format on
    )
);

// Replace clrlwi/li/li/bl as one block. The input-manager load at 0x8053F234
// and the result compare/branch at 0x8053F248/0x8053F24C stay native. No native
// branch, symbol, relocation, or data pointer enters the displaced block.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x8053F238,
    ASM_LAMBDA(
        ( : ASM_IMPORT(i, IsOnlineVSInputIdle)),
        // clang-format off
        clrlwi r4, r0, 24;
        li r5, 1800;
        li r6, 3600;
        b %[IsOnlineVSInputIdle];
        // clang-format on
    )
);
#  endif

} // namespace wwfc::Knockout

#endif // RMC
