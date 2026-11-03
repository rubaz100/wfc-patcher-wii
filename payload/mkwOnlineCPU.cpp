#include "mkwOnlineCPU.hpp"

#if RMC
#  include "mkwOnlineCPUProtocol.hpp"

#  if WWFC_ONLINE_CPU_APPLY && !RMCPD00
#    error "Online CPU native adapters have only been verified for PAL retail"
#  endif

#  if WWFC_ONLINE_CPU_APPLY
#    include "import/dwc.h"
#    include "import/gamespy.h"
#    include "import/mkw/net/NetManager.hpp"
#    include "import/mkw/system/RaceConfig.hpp"
#    include "import/mkw/system/RaceManager.hpp"
#    include "import/mkw/system/System.hpp"
#    include "mkwEliminatedVoting.hpp"
#    include "mkwInactivity.hpp"
#    include "mkwKnockdownMode.hpp"
#    include "mkwKnockout.hpp"
#    include "mkwPenalty.hpp"
#    include "wwfcGPReport.hpp"
#    include "wwfcLibC.hpp"
#    include "wwfcPatch.hpp"

namespace wwfc::OnlineCPU
{
namespace Transport
{
constinit Plan s_pending                                   = {};
constinit Plan s_active                                    = {};
constinit u8   s_frames[12][Wire::FRAME_SIZE]              = {};
constinit u16  s_valid                                     = 0;
alignas(4) constinit u8 s_items[12][Wire::ITEM_STATE_SIZE] = {};
constinit u16           s_itemValid = 0, s_itemDirty = 0;
constinit u32           s_itemSequence           = 0;
constinit u32           s_lastItemSend[12]       = {};
constinit u32           s_lastItemReceive[12]    = {};
constexpr u32           ITEM_STATE_TIMEOUT_TICKS = 300;
constinit u32           s_ticks = 0, s_lastOwnerTick = 0, s_nonce = 0, s_sequence = 0;
constinit u32           s_lastPrepareTick = 0;
constinit u32           s_lastAckTick     = 0;
constinit u16           s_finishSent      = 0;
constinit bool          s_protocol = false, s_waiting = false, s_committed = false;
constinit bool          s_grid = false, s_alive = false, s_sawRaceScene = false;
constinit bool          s_allocated = false, s_gridCanceled = false;
constexpr u32           KNOCKOUT_EVENT_SIZE                    = 24;
constinit KnockoutEvent s_knockoutEvents[12]                   = {};
constinit u8            s_knockoutCount                        = 0;
constinit u32           s_knockoutFinalBase                    = 0;
constinit u8            s_knockoutWinnerId                     = 0xff;
constinit u16           s_knockoutFinalDisconnected            = 0;
constinit u16           s_courseDNF                            = 0;
constinit u8            s_pendingKnockout[KNOCKOUT_EVENT_SIZE] = {};
constinit u32           s_lastKnockoutSend                     = 0;
constinit bool          s_tournamentReady                      = false;
constinit u32           s_completeSeriesId = 0, s_completeEpoch = 0;
constinit u8            s_carriedCPU[12] = {}, s_carriedHumanPlace[12] = {};
constinit u32           s_carriedHumanPID[12] = {}, s_racerPID[12] = {};
constinit u8            s_racerLocal[12]                  = {};
constinit u8            s_carriedHumanCount               = 0;
constexpr u32           COURSE_RESULT_MAX                 = 12 + 12 * 16;
constinit u8            s_courseResult[COURSE_RESULT_MAX] = {};
constinit u32           s_courseResultLength = 0, s_lastCourseSend = 0;

bool RaceScene();

void ResetTournament()
{
    s_tournamentReady    = false;
    s_carriedHumanCount  = 0;
    s_courseResultLength = 0;
    std::memset(s_carriedCPU, 0, sizeof(s_carriedCPU));
    std::memset(s_carriedHumanPID, 0, sizeof(s_carriedHumanPID));
    std::memset(s_carriedHumanPlace, 0, sizeof(s_carriedHumanPlace));
    std::memset(s_racerPID, 0, sizeof(s_racerPID));
    std::memset(s_racerLocal, 0, sizeof(s_racerLocal));
}

bool CacheRacerIdentity(u8 id)
{
    const auto* plan    = CurrentPlan();
    auto*       network = mkw::NetManager::Instance();
    if (!plan || id >= plan->humanCount || !network)
        return false;
    if (s_racerPID[id])
        return true;
    const auto* race    = mkw::RaceManager::Instance();
    auto*       configs = mkw::RaceConfigManager::Instance();
    if (!RaceScene() || !race || !configs || !race->m_players ||
        configs->getConfig().playerCount() != plan->humanCount + plan->cpuCount ||
        static_cast<u32>(race->m_state) > 2)
        return false;
    // Do not cache a previous course's native ID/AID map during allocation.
    for (u8 human = 0; human < plan->humanCount; ++human)
        if (!race->m_players[human] || race->m_players[human]->m_id != human)
            return false;
    const auto* aids = reinterpret_cast<const u8*>(network) + 0x2920;
    const u8    aid  = aids[id];
    if (aid >= MAX_PLAYERS)
        return false;
    u32 pid = 0;
    if (aid == network->myAid()) {
        const auto* user = DWC::DWCi_GetUserData();
        if (user)
            pid = user->profileId;
    } else if (network->availableAids() & (1u << aid)) {
        const auto* node = DWC::DWCi_NodeInfoList_GetNodeInfoForAid(aid);
        if (node)
            pid = node->profileId;
    }
    u8 local = 0;
    for (u8 previous = 0; previous < id; ++previous)
        if (aids[previous] == aid)
            ++local;
    if (!pid || local != 0)
        return false; // KO supports one local human.
    s_racerPID[id]   = pid;
    s_racerLocal[id] = local;
    return true;
}

void ResetItemStates()
{
    s_itemValid = 0;
    s_itemDirty = 0;
    std::memset(s_items, 0, sizeof(s_items));
    std::memset(s_lastItemSend, 0, sizeof(s_lastItemSend));
    std::memset(s_lastItemReceive, 0, sizeof(s_lastItemReceive));
}

void ResetKnockoutEvents()
{
    s_knockoutCount             = 0;
    s_knockoutFinalBase         = 0;
    s_knockoutWinnerId          = 0xff;
    s_knockoutFinalDisconnected = 0;
    s_courseDNF                 = 0;
    std::memset(s_pendingKnockout, 0, sizeof(s_pendingKnockout));
}

u32 Self()
{
    auto* user = DWC::DWCi_GetUserData();
    return user ? user->profileId : 0;
}

bool SnapshotBudgetAvailable()
{
    // Replaceable state must not grow the GP output queue while its socket
    // is stalled. Keep the latest CPU snapshot locally until it drains.
    const auto* match = DWC::stpMatchCnt;
    if (!match || !match->connection || !*match->connection)
        return false;
    const auto* connection = reinterpret_cast<const GameSpy::GPIConnection*>(*match->connection);
    const s32   output     = connection->outputBuffer.length;
    const u32   pending    = GPReport::PendingBytes();
    return output >= 0 && output < 4096 && pending < 4096 - static_cast<u32>(output);
}

bool RaceScene()
{
    auto* scenes = mkw::System::Instance().getSceneManager();
    return scenes && mkw::System::Instance().isCurrentSceneID(mkw::Scene::ESceneID::RACE);
}

template <class T>
T Load(const void* object, u32 offset)
{
    T result;
    std::memcpy(&result, static_cast<const u8*>(object) + offset, sizeof(T));
    return result;
}

template <class T>
void Store(void* object, u32 offset, T value)
{
    std::memcpy(static_cast<u8*>(object) + offset, &value, sizeof(T));
}

bool InPlan(const Plan& plan, u8 id)
{
    return plan.epoch && id >= plan.humanCount && id < plan.humanCount + plan.cpuCount;
}

void Message(const char* text)
{
    // Messages are bounded by the callback before entering this parser.
    if (std::strncmp(text, "LCPU3:", 6))
        return;
    const char* p = text + 8;
    u32         a, b, c, d, e, f;
    switch (text[6]) {
    case 'S':
        if (text[7] == ':' && Wire::Number(p, a, ':') && Wire::Number(p, b, ':') &&
            Wire::Number(p, c, '\0') && (a == 0 || a == PROTOCOL_VERSION) && b <= 1 && c >= 1 &&
            c <= 11) {
            s_protocol = a == PROTOCOL_VERSION;
            // Policy/count are frozen by the server's per-selection plan.
        }
        break;
    case 'P':
        if (text[7] != ':' || !s_waiting || !Wire::Number(p, a, ':') || !Wire::Number(p, b, ':') ||
            !Wire::Number(p, c, ':') || !Wire::Number(p, d, ':') || !Wire::Number(p, e, ':') ||
            !Wire::Number(p, f, '\0') || f > 2 || !a || !b || c == 0 || c > 12 || d > 11 ||
            c + d > 12)
            break;
        if (s_pending.epoch && a != s_pending.epoch && !Wire::Newer(a, s_pending.epoch))
            break;
        if (a != s_pending.epoch) {
            s_valid      = 0;
            s_finishSent = 0;
            s_committed  = false;
            ResetKnockoutEvents();
            ResetItemStates();
            ResetTournament();
            // A fresh physical room receives a fresh server proposal. A
            // completed room emits only Z and never creates another grid.
            s_completeSeriesId = s_completeEpoch = 0;
        }
        s_pending.epoch           = a;
        s_pending.ownerProfileId  = b;
        s_pending.seed            = e;
        s_pending.humanCount      = static_cast<u8>(c);
        s_pending.cpuCount        = static_cast<u8>(d);
        s_pending.knockoutEnabled = f != 0;
        s_pending.knockoutMode    = static_cast<u8>(f);
        if (f != 2) {
            s_pending.knockoutEliminationCount = 1;
            s_pending.seriesId                 = 0;
            s_pending.seriesRound              = 0;
            s_pending.seriesState              = 0;
        }
        s_lastAckTick = s_ticks;
        if (!s_completeSeriesId && (f != 2 || s_tournamentReady))
            GPReport::ReportU32("wl:cpu_ack", a);
        break;
    case 'T': {
        if (text[7] != ':' || !s_waiting || s_pending.knockoutMode != 2 ||
            !Wire::Number(p, a, ':') || a != s_pending.epoch || !Wire::Number(p, b, ':') || !b ||
            !Wire::Number(p, c, ':') || c < 1 || c > 12 || !Wire::Number(p, d, ':') || d < 1 ||
            d > 2 || !Wire::Number(p, e, ':') || e > 2)
            break;
        const u8 state    = static_cast<u8>(e);
        u8       cpus[12] = {}, places[12] = {};
        u32      humans[12] = {};
        bool     valid      = true;
        for (u8 i = 0; i < s_pending.cpuCount; ++i) {
            if (!Wire::Number(p, e, i + 1 == s_pending.cpuCount ? ':' : ',') ||
                (e != 0 && e != 0xFF && (e < 2 || e > MAX_PLAYERS))) {
                valid = false;
                break;
            }
            cpus[i] = static_cast<u8>(e);
        }
        if (!valid)
            break;
        if (!s_pending.cpuCount && (!Wire::Number(p, e, ':') || e != 0))
            break;
        u8 count = 0;
        if (!(p[0] == '0' && p[1] == '\0')) {
            for (;;) {
                if (count >= s_pending.humanCount || !Wire::Number(p, e, '.') || !e) {
                    valid = false;
                    break;
                }
                const char* end = p;
                while (*end && *end != ',')
                    ++end;
                const char separator = *end;
                if (!Wire::Number(p, f, separator) || (f != 0xFF && (f < 2 || f > MAX_PLAYERS))) {
                    valid = false;
                    break;
                }
                for (u8 i = 0; i < count; ++i)
                    if (humans[i] == e)
                        valid = false;
                if (!valid)
                    break;
                humans[count]   = e;
                places[count++] = static_cast<u8>(f);
                if (!separator)
                    break;
            }
        }
        if (!valid)
            break;
        std::memcpy(s_carriedCPU, cpus, sizeof(cpus));
        std::memcpy(s_carriedHumanPID, humans, sizeof(humans));
        std::memcpy(s_carriedHumanPlace, places, sizeof(places));
        s_carriedHumanCount                = count;
        s_pending.seriesId                 = b;
        s_pending.seriesRound              = static_cast<u8>(c);
        s_pending.knockoutEliminationCount = static_cast<u8>(d);
        s_pending.seriesState              = state;
        s_tournamentReady                  = true;
        s_lastAckTick                      = s_ticks;
        if (state == 2) {
            s_completeSeriesId = b;
            s_completeEpoch    = a;
        } else {
            s_completeSeriesId = s_completeEpoch = 0;
            GPReport::ReportU32("wl:cpu_ack", a);
        }
        break;
    }
    case 'Z':
        if (text[7] == ':' && Wire::Number(p, a, ':') && a && Wire::Number(p, b, '\0') && b &&
            ((s_allocated && s_active.epoch == a && s_active.seriesId == b) ||
             (s_pending.epoch == a && s_pending.seriesId == b) ||
             (s_waiting && !s_pending.epoch))) {
            // This tombstone only holds the NEXT selection. Keep the current
            // course's K/F stream and native winner/results flow untouched.
            s_completeSeriesId = b;
            s_completeEpoch    = a;
        }
        break;
    case 'C':
        if (text[7] == ':' && s_waiting && Wire::Number(p, a, '\0') && s_pending.epoch == a &&
            (s_pending.knockoutMode != 2 || (s_tournamentReady && s_pending.seriesState != 2)) &&
            !s_completeSeriesId) {
            s_committed     = true;
            s_alive         = true;
            s_lastOwnerTick = s_ticks;
        }
        break;
    case 'H':
        if (text[7] == ':' && Wire::Number(p, a, '\0') && s_committed && s_alive &&
            a == (s_allocated ? s_active.epoch : s_pending.epoch))
            s_lastOwnerTick = s_ticks;
        break;
    case 'X':
        if (text[7] == ':' && Wire::Number(p, a, '\0')) {
            if (s_allocated && s_active.epoch == a) {
                s_alive = false;
                ResetItemStates();
            }
            if (s_pending.epoch == a && !s_allocated) {
                // A replacement proposal, including a zero-CPU plan, must
                // commit before SELECT can continue.
                s_committed = false;
                // Once SELECT is destroyed, peers may already be allocating
                // their agreed grid. Fail closed instead of loading a
                // different, human-only roster on this console.
                s_gridCanceled = !s_waiting;
                s_waiting      = false;
            }
        }
        break;
    case 'I': {
        if (text[7] != ':')
            break;
        u8        bytes[Wire::ITEM_STATE_SIZE];
        const s32 size =
            DWC::DWC_Base64Decode(p, std::strlen(p), reinterpret_cast<char*>(bytes), sizeof(bytes));
        const Plan& plan = s_allocated ? s_active : s_pending;
        if (size != static_cast<s32>(sizeof(bytes)) || !s_committed || !s_alive ||
            !Wire::ValidItemState(bytes, plan.epoch, plan.humanCount, plan.cpuCount))
            break;
        const u8 id = bytes[8];
        if ((s_itemValid & (1u << id)) &&
            !Wire::Newer(Wire::Read32(bytes + 4), Wire::Read32(s_items[id] + 4)))
            break;
        // Buffering is safe during loading. Native adapters cannot consume
        // these bytes until HasItemState confirms an actual race is active.
        std::memcpy(s_items[id], bytes, sizeof(bytes));
        s_itemValid |= 1u << id;
        s_lastItemReceive[id] = s_ticks;
        s_lastOwnerTick       = s_ticks;
        break;
    }
    case 'E': {
        if (text[7] != ':')
            break;
        u8        bytes[KNOCKOUT_EVENT_SIZE];
        const s32 size =
            DWC::DWC_Base64Decode(p, std::strlen(p), reinterpret_cast<char*>(bytes), sizeof(bytes));
        const Plan& plan = s_allocated ? s_active : s_pending;
        if (size != static_cast<s32>(sizeof(bytes)) || !s_committed || !plan.knockoutEnabled ||
            Wire::Read32(bytes) != plan.epoch || s_knockoutFinalBase ||
            Wire::Read32(bytes + 4) != s_knockoutCount + 1u || bytes[15])
            break;
        const u16 previousMask = s_knockoutCount
                                     ? s_knockoutEvents[s_knockoutCount - 1].eliminatedMask
                                     : InitialKnockoutMask();
        const u16 lap          = Wire::Read16(bytes + 8);
        const u8  id = bytes[10], place = bytes[11], kind = bytes[14];
        const u8  total        = plan.humanCount + plan.cpuCount;
        const u32 base         = Wire::Read32(bytes + 16);
        const u32 disconnected = Wire::Read32(bytes + 20);
        if (kind == 1) {
            if ((plan.knockoutMode != 2 && disconnected) || (disconnected & ~((1u << total) - 1)) ||
                !previousMask || lap || id != 0xff || place ||
                Wire::Read16(bytes + 12) != previousMask || !base ||
                base > 63u * 60000u - 1 - MAX_PLAYERS * 10u)
                break;
            s_knockoutFinalBase = base;
            s_courseDNF         = static_cast<u16>(disconnected);
        } else if (kind == 2) {
            const u32 grid = (1u << total) - 1;
            if (plan.knockoutMode == 2 || lap || id >= total || place != 1 ||
                Wire::Read16(bytes + 12) != previousMask || (disconnected & ~grid) ||
                (grid & ~(previousMask | disconnected)) != (1u << id) || !base ||
                base > 63u * 60000u - 1 - MAX_PLAYERS * 10u)
                break;
            s_knockoutFinalBase         = base;
            s_knockoutWinnerId          = id;
            s_knockoutFinalDisconnected = static_cast<u16>(disconnected);
        } else if (kind == 0) {
            if (disconnected || s_knockoutCount >= 11 || base || lap < 2 || lap > total ||
                id >= total || place < 2 || place > total || (previousMask & (1u << id)) ||
                Wire::Read16(bytes + 12) != (previousMask | (1u << id)) ||
                (s_knockoutCount && (lap <= s_knockoutEvents[s_knockoutCount - 1].leaderLap ||
                                     place >= s_knockoutEvents[s_knockoutCount - 1].placement)))
                break;
            s_knockoutEvents[s_knockoutCount] = { s_knockoutCount + 1u, lap, id, place,
                                                  Wire::Read16(bytes + 12) };
            ++s_knockoutCount;
        } else {
            break;
        }
        if (Wire::Read32(s_pendingKnockout + 4) <= Wire::Read32(bytes + 4))
            std::memset(s_pendingKnockout, 0, sizeof(s_pendingKnockout));
        s_lastOwnerTick = s_ticks;
        break;
    }
    case 'F': {
        if (text[7] != ':')
            break;
        alignas(4) u8 bytes[Wire::FRAME_SIZE];
        s32           size =
            DWC::DWC_Base64Decode(p, std::strlen(p), reinterpret_cast<char*>(bytes), sizeof(bytes));
        const Plan& plan = s_allocated ? s_active : s_pending;
        if (size != static_cast<s32>(sizeof(bytes)) || !s_committed ||
            !Wire::ValidFrame(bytes, plan.epoch, plan.humanCount, plan.cpuCount))
            break;
        u8 id = bytes[8];
        if ((s_valid & (1u << id)) &&
            !Wire::Newer(Wire::Read32(bytes + 4), Wire::Read32(s_frames[id] + 4)))
            break;
        std::memcpy(s_frames[id], bytes, sizeof(bytes));
        s_valid |= 1u << id;
        s_lastOwnerTick = s_ticks;
        break;
    }
    default:
        break;
    }
}

struct BuddyMessage {
    u32         profileId;
    u32         date;
    const char* message;
};

// Original first four instructions of the PAL DWC buddy callback. Its existing
// security patch remains in the original body after this entry trampoline.
WWFC_DEFINE_CTR_STUB(
    0x800D3658, void OriginalBuddy(void* connection, const BuddyMessage* message, void* param),
    stwu r1, -0x30(r1);
    mflr r0; stw r0, 0x34(r1); addi r11, r1, 0x30;
)

void Buddy(void* connection, const BuddyMessage* argument, void* param)
{
    if (argument && argument->profileId == Self() && argument->message) {
        const char* text   = argument->message;
        u32         length = 0;
        while (length < 256 && text[length])
            ++length;
        if (length < 256 && length >= 8) {
            if (Penalty::HandleServerMessage(text))
                return;
            if (Inactivity::HandleServerMessage(text))
                return;
            if (EliminatedVoting::HandleServerMessage(text))
                return;
            if (!std::strncmp(text, "LCPU3:", 6)) {
                Message(text);
                return;
            }
            if (!std::strncmp(text, "LKD3:", 5)) {
                Knockdown::HandleServerMessage(text);
                return;
            }
        }
    }
    OriginalBuddy(connection, argument, param);
}
} // namespace Transport

using namespace Transport;

const Plan* CurrentPlan()
{
    return s_committed ? (s_allocated ? &s_active : &s_pending) : nullptr;
}

bool KnockoutEnabled()
{
    return s_committed && s_allocated && s_active.knockoutEnabled;
}

bool RaceKnockoutEnabled()
{
    return KnockoutEnabled() && s_active.knockoutMode == 2 && s_tournamentReady;
}

bool RaceKnockoutSeriesComplete()
{
    return s_completeSeriesId != 0 && s_completeEpoch != 0;
}

bool RaceKnockoutRosterReady()
{
    const auto* plan = CurrentPlan();
    if (!plan || plan->knockoutMode != 2 || !s_tournamentReady)
        return false;
    for (u8 id = 0; id < plan->humanCount; ++id)
        if (!CacheRacerIdentity(id))
            return false;
    return true;
}

bool IsLocalProfileSeriesEliminated()
{
    if (!s_tournamentReady)
        return false;
    const u32 self = Self();
    for (u8 i = 0; i < s_carriedHumanCount; ++i) {
        if (s_carriedHumanPID[i] == self && s_carriedHumanPlace[i] != 0)
            return true;
    }
    return false;
}

u32 EliminatedSeriesVotingAids(u32 candidates, bool committedOnly)
{
    // Only the committed upcoming SELECT metadata identifies this course's
    // spectators. In-race grid IDs/AIDs can still belong to the previous race.
    if (!s_protocol || !s_waiting || (committedOnly && !s_committed) || s_allocated || s_gridCanceled ||
        !s_pending.epoch || s_pending.knockoutMode != 2 || !s_pending.seriesId ||
        !s_tournamentReady || RaceKnockoutSeriesComplete())
        return 0;
    const auto* network = mkw::NetManager::Instance();
    if (!network || network->myAid() >= MAX_PLAYERS)
        return 0;
    const u8  own       = network->myAid();
    const u32 connected = network->availableAids();
    const u32 self      = Self();
    u32       excluded  = 0;
    for (u8 aid = 0; aid < MAX_PLAYERS; ++aid) {
        const u32 bit = 1u << aid;
        if (!(candidates & bit))
            continue;
        u32 pid = 0;
        if (aid == own) {
            pid = self;
        } else if (connected & bit) {
            const auto* node = DWC::DWCi_NodeInfoList_GetNodeInfoForAid(aid);
            if (node)
                pid = node->profileId;
        }
        if (!pid)
            continue;
        for (u8 i = 0; i < s_carriedHumanCount; ++i) {
            if (s_carriedHumanPID[i] == pid && s_carriedHumanPlace[i] != 0) {
                excluded |= bit;
                break;
            }
        }
    }
    return excluded;
}

u8 InitialKnockoutPlacement(u8 id)
{
    const auto* plan = CurrentPlan();
    if (!plan || plan->knockoutMode != 2 || !s_tournamentReady ||
        id >= plan->humanCount + plan->cpuCount)
        return 0;
    if (id >= plan->humanCount)
        return s_carriedCPU[id - plan->humanCount];
    if (!CacheRacerIdentity(id))
        return 0;
    for (u8 i = 0; i < s_carriedHumanCount; ++i)
        if (s_carriedHumanPID[i] == s_racerPID[id])
            return s_carriedHumanPlace[i];
    return 0;
}

u16 InitialKnockoutMask()
{
    const auto* plan = CurrentPlan();
    if (!plan || plan->knockoutMode != 2)
        return 0;
    u16 mask = 0;
    for (u8 id = 0; id < plan->humanCount + plan->cpuCount; ++id)
        if (InitialKnockoutPlacement(id))
            mask |= 1u << id;
    return mask;
}

bool KnockoutRoundCommitted()
{
    return RaceKnockoutEnabled() && s_knockoutFinalBase != 0;
}

u16 KnockoutCourseDNFMask()
{
    return KnockoutRoundCommitted() ? s_courseDNF : 0;
}

void RequestRaceKnockoutCommit()
{
    if (!RaceKnockoutEnabled() || KnockoutRoundCommitted() || s_ticks - s_lastCourseSend < 60)
        return;
    s_lastCourseSend = s_ticks;
    GPReport::ReportU32("wl:cpu_plan", s_active.epoch);
}

bool PublishRaceKnockoutResult(u32 base)
{
    if (!RaceKnockoutEnabled() || !IsOwner() || !OwnerAlive() || KnockoutRoundCommitted() ||
        !RaceKnockoutRosterReady() || !base || base > 63u * 60000u - 1 - MAX_PLAYERS * 10u)
        return false;
    if (!s_courseResultLength) {
        const auto* race = mkw::RaceManager::Instance();
        if (!race || !race->m_players)
            return false;
        const u8 total                    = s_active.humanCount + s_active.cpuCount;
        u8       bytes[COURSE_RESULT_MAX] = {};
        Wire::Write32(bytes, s_active.epoch);
        Wire::Write32(bytes + 4, base);
        bytes[8] = total;
        for (u8 id = 0; id < total; ++id) {
            const auto* player = race->m_players[id];
            if (!player || player->m_id != id)
                return false;
            u8* row = bytes + 12 + id * 16;
            row[0]  = id;
            if (id >= s_active.humanCount) {
                row[1] = 1;
                row[2] = id - s_active.humanCount;
            } else {
                row[2] = s_racerLocal[id];
                Wire::Write32(row + 4, s_racerPID[id]);
            }
            if (InitialKnockoutPlacement(id))
                continue;
            const auto* time = player->m_raceFinishTime;
            if (player->m_stateFlags & mkw::RaceManager::Player::FLAG_DISCONNECTED)
                row[3] = 2;
            else if (!(player->m_stateFlags & mkw::RaceManager::Player::FLAG_FINISHED))
                return false;
            else if (
                time && time->m_valid && time->m_minutes < 63 && time->m_seconds < 60 &&
                time->m_milliseconds < 1000
            ) {
                const u32 ms =
                    time->m_minutes * 60000u + time->m_seconds * 1000u + time->m_milliseconds;
                if (ms) {
                    row[3] = 1;
                    Wire::Write32(row + 8, ms);
                } else
                    row[3] = 4;
            } else
                row[3] = 4;
        }
        s_courseResultLength = 12 + total * 16;
        std::memcpy(s_courseResult, bytes, s_courseResultLength);
        // First delivery is immediate; retries retain this exact decision input.
        s_lastCourseSend = s_ticks - 60;
    }
    if (s_ticks - s_lastCourseSend >= 60) {
        s_lastCourseSend = s_ticks;
        GPReport::ReportB64Encode("wl:ko_course", s_courseResult, s_courseResultLength);
    }
    return true;
}

bool KnockoutDecisionPending()
{
    return KnockoutEnabled() && Wire::Read32(s_pendingKnockout + 4) != 0;
}

u32 KnockoutFinalBase()
{
    return KnockoutEnabled() ? s_knockoutFinalBase : 0;
}

u8 KnockoutWinnerId()
{
    return KnockoutEnabled() ? s_knockoutWinnerId : 0xff;
}

u16 KnockoutFinalDisconnectedMask()
{
    return KnockoutEnabled() ? s_knockoutFinalDisconnected : 0;
}

bool PublishKnockoutWinner(u32 base, u8 winner, u16 disconnected)
{
    const auto* plan = CurrentPlan();
    if (!KnockoutEnabled() || RaceKnockoutEnabled() || !IsOwner() || !OwnerAlive() ||
        KnockoutDecisionPending() || s_knockoutFinalBase || !plan ||
        winner >= plan->humanCount + plan->cpuCount || !base ||
        base > 63u * 60000u - 1 - MAX_PLAYERS * 10u)
        return false;
    const u16 mask = s_knockoutCount ? s_knockoutEvents[s_knockoutCount - 1].eliminatedMask
                                     : InitialKnockoutMask();
    const u16 grid = (1u << (plan->humanCount + plan->cpuCount)) - 1;
    if ((disconnected & ~grid) || (grid & ~(mask | disconnected)) != (1u << winner))
        return false;
    std::memset(s_pendingKnockout, 0, sizeof(s_pendingKnockout));
    Wire::Write32(s_pendingKnockout, plan->epoch);
    Wire::Write32(s_pendingKnockout + 4, s_knockoutCount + 1);
    s_pendingKnockout[10] = winner;
    s_pendingKnockout[11] = 1;
    Wire::Write16(s_pendingKnockout + 12, mask);
    s_pendingKnockout[14] = 2;
    Wire::Write32(s_pendingKnockout + 16, base);
    Wire::Write32(s_pendingKnockout + 20, disconnected);
    s_lastKnockoutSend = s_ticks;
    GPReport::ReportB64Encode("wl:ko_event", s_pendingKnockout, sizeof(s_pendingKnockout));
    return true;
}

bool ReadKnockoutEvent(u32 after, KnockoutEvent& event)
{
    if (!KnockoutEnabled() || after >= s_knockoutCount)
        return false;
    event = s_knockoutEvents[after];
    return true;
}

bool PublishKnockout(u16 lap, u8 id, u8 placement)
{
    if (!KnockoutEnabled() || !IsOwner() || !OwnerAlive() || Wire::Read32(s_pendingKnockout + 4) ||
        s_knockoutFinalBase || s_knockoutCount >= 11 || lap < 2 || lap > 255 ||
        id >= s_active.humanCount + s_active.cpuCount || placement < 2 || placement > 12)
        return false;
    const u16 mask = s_knockoutCount ? s_knockoutEvents[s_knockoutCount - 1].eliminatedMask : 0;
    if (mask & (1u << id))
        return false;
    Wire::Write32(s_pendingKnockout, s_active.epoch);
    Wire::Write32(s_pendingKnockout + 4, s_knockoutCount + 1);
    Wire::Write16(s_pendingKnockout + 8, lap);
    s_pendingKnockout[10] = id;
    s_pendingKnockout[11] = placement;
    Wire::Write16(s_pendingKnockout + 12, mask | (1u << id));
    s_pendingKnockout[14] = 0;
    s_pendingKnockout[15] = 0;
    Wire::Write32(s_pendingKnockout + 16, 0);
    Wire::Write32(s_pendingKnockout + 20, 0);
    s_lastKnockoutSend = s_ticks;
    GPReport::ReportB64Encode("wl:ko_event", s_pendingKnockout, sizeof(s_pendingKnockout));
    return true;
}

bool PublishKnockoutFinal(u32 base)
{
    if (!KnockoutEnabled() || !IsOwner() || !OwnerAlive() || KnockoutDecisionPending() ||
        s_knockoutFinalBase || !s_knockoutCount || !base ||
        base > 63u * 60000u - 1 - MAX_PLAYERS * 10u)
        return false;
    std::memset(s_pendingKnockout, 0, sizeof(s_pendingKnockout));
    Wire::Write32(s_pendingKnockout, s_active.epoch);
    Wire::Write32(s_pendingKnockout + 4, s_knockoutCount + 1);
    s_pendingKnockout[10] = 0xff;
    Wire::Write16(s_pendingKnockout + 12, s_knockoutEvents[s_knockoutCount - 1].eliminatedMask);
    s_pendingKnockout[14] = 1;
    Wire::Write32(s_pendingKnockout + 16, base);
    s_lastKnockoutSend = s_ticks;
    GPReport::ReportB64Encode("wl:ko_event", s_pendingKnockout, sizeof(s_pendingKnockout));
    return true;
}

bool IsOwner()
{
    const Plan* p = CurrentPlan();
    return p && p->ownerProfileId == Self();
}

bool IsCPUSlot(u8 id)
{
    return s_grid && InPlan(s_active, id);
}

bool OwnerAlive()
{
    // The server also watches the authenticated owner's heartbeat. Keep the
    // local allocation stable when the owner disappears; native DC handles DNF.
    return s_alive &&
           (s_valid == 0 ? s_ticks - s_lastOwnerTick < 5400 : s_ticks - s_lastOwnerTick < 300);
}

bool HasRaceData(u8 id)
{
    return id < 12 && IsCPUSlot(id) && OwnerAlive() && (s_valid & (1u << id));
}

const void* RaceData(u8 id)
{
    return HasRaceData(id) ? s_frames[id] + Wire::DATA_OFFSET : nullptr;
}

u32 RaceDataTimer(u8 id)
{
    // Receiver::Calc compares this with its last decoded timestamp. Native
    // RH1 has no AID for a virtual CPU and would otherwise always return zero.
    return HasRaceData(id) ? Wire::Read32(s_frames[id] + 40) + 1 : 0;
}

bool HasItemState(u8 id)
{
    if (!IsCPUSlot(id) || !OwnerAlive() || !(s_itemValid & (1u << id)) ||
        Knockout::IsEliminated(id) || !RaceScene())
        return false;
    // Owner heartbeats and movement cannot keep an old guest inventory alive.
    // The adapter reconciles an empty ITEM row after 300 transport ticks without
    // a fresh snapshot. Keep the valid bit/sequence so stale packets cannot
    // resurrect it; owner captures remain authoritative and do not expire.
    if (!IsOwner() && s_ticks - s_lastItemReceive[id] >= ITEM_STATE_TIMEOUT_TICKS)
        return false;
    const auto* race = mkw::RaceManager::Instance();
    if (!race || race->m_state != mkw::RaceManager::EState::RACE || !race->m_players ||
        !race->m_players[id])
        return false;
    return !(
        race->m_players[id]->m_stateFlags &
        (mkw::RaceManager::Player::FLAG_FINISHED | mkw::RaceManager::Player::FLAG_DISCONNECTED |
         mkw::RaceManager::Player::FLAG_RETIRED)
    );
}

const void* ItemState(u8 id)
{
    return HasItemState(id) ? s_items[id] + Wire::ITEM_DATA_OFFSET : nullptr;
}

void CaptureItemState(u8 id, const void* bytes, u32 size)
{
    if (!IsOwner() || !IsCPUSlot(id) || !OwnerAlive() || !bytes || size != Wire::ITEM_DATA_SIZE ||
        Knockout::IsEliminated(id) || !RaceScene())
        return;
    const auto* race = mkw::RaceManager::Instance();
    if (!race || race->m_state != mkw::RaceManager::EState::RACE || !race->m_players ||
        !race->m_players[id] ||
        (race->m_players[id]->m_stateFlags &
         (mkw::RaceManager::Player::FLAG_FINISHED | mkw::RaceManager::Player::FLAG_DISCONNECTED |
          mkw::RaceManager::Player::FLAG_RETIRED)))
        return;
    u8         out[Wire::ITEM_STATE_SIZE] = {};
    const bool changed = !(s_itemValid & (1u << id)) ||
                         std::memcmp(s_items[id] + Wire::ITEM_DATA_OFFSET, bytes, size) != 0;
    Wire::Write32(out, s_active.epoch);
    Wire::Write32(out + 4, Wire::Read32(s_items[id] + 4));
    out[8] = id;
    out[9] = 1;
    std::memcpy(out + Wire::ITEM_DATA_OFFSET, bytes, size);
    if (!Wire::ValidItemState(out, s_active.epoch, s_active.humanCount, s_active.cpuCount))
        return;
    std::memcpy(s_items[id], out, sizeof(out));
    s_itemValid |= 1u << id;
    if (changed)
        s_itemDirty |= 1u << id;
}

bool GridCanceled()
{
    return s_gridCanceled;
}

void BeginSelection()
{
    if (!s_protocol || s_waiting || RaceScene())
        return;
    s_waiting      = true;
    s_grid         = false;
    s_allocated    = false;
    s_gridCanceled = false;
    s_pending      = {};
    s_committed    = false;
    s_valid        = 0;
    s_finishSent   = 0;
    ResetKnockoutEvents();
    ResetItemStates();
    ResetTournament();
    if (++s_nonce == 0)
        ++s_nonce;
    s_lastPrepareTick = s_ticks;
    GPReport::ReportU32("wl:cpu_prepare", s_nonce);
}

bool CanSelectAdvance()
{
    if (RaceKnockoutSeriesComplete())
        return false;
    if (Knockdown::Selected() && (!Knockdown::ServerAvailable() || !s_protocol))
        return false;
    return !s_protocol || (s_waiting && s_committed);
}

void EndSelection()
{
    s_waiting = false;
}

void OnGridInitialized()
{
    if (s_committed) {
        s_active        = s_pending;
        s_allocated     = true;
        s_grid          = s_active.cpuCount != 0;
        s_alive         = true;
        s_sawRaceScene  = false;
        s_lastOwnerTick = s_ticks;
    }
    EndSelection();
}

void GridRejected()
{
    GPReport::ReportU32("wl:cpu_grid_error", s_pending.epoch);
    s_alive = false;
    s_grid  = false;
    ResetItemStates();
    EndSelection();
}

void CaptureRaceData(u8 id, const void* bytes, u32 size)
{
    if (!IsOwner() || !IsCPUSlot(id) || !bytes || size != 64)
        return;
    auto* race = mkw::RaceManager::Instance();
    if (!race || !race->m_players || !race->m_players[id])
        return;
    auto* player = race->m_players[id];
    u8*   out    = s_frames[id];
    Wire::Write32(out, s_active.epoch);
    Wire::Write32(out + 4, ++s_sequence);
    out[8] = id;
    out[9] = 1;
    Wire::Write16(out + 10, player->m_currentLap);
    for (u32 offset = 12; offset <= 28; offset += 4)
        Wire::WriteFloat(out + offset, Load<float>(player, offset));
    Wire::Write16(out + 32, Load<u16>(player, 0xA));
    out[34] = Load<u8>(player, 0x26);
    out[35] = Load<u8>(player, 0x27);
    out[36] = Load<u8>(player, 0x28);
    out[37] = Load<u8>(player, 0x21);
    out[38] = player->m_position;
    out[39] = 0;
    Wire::Write32(out + 40, Load<u32>(player, 0x2C));
    u32   finish = 0;
    auto* time   = player->m_raceFinishTime;
    if ((player->m_stateFlags & mkw::RaceManager::Player::FLAG_FINISHED) && time && time->m_valid &&
        time->m_minutes < 63 && time->m_seconds < 60 && time->m_milliseconds < 1000) {
        finish = time->m_minutes * 60000u + time->m_seconds * 1000u + time->m_milliseconds;
        if (finish)
            out[9] |= 2;
    }
    Wire::Write32(out + 44, finish);
    std::memcpy(out + 48, bytes, 64);
    s_valid |= 1u << id;
    s_lastOwnerTick = s_ticks;
    if (s_ticks % 3 == 0 && SnapshotBudgetAvailable() &&
        Wire::ValidFrame(out, s_active.epoch, s_active.humanCount, s_active.cpuCount) &&
        GPReport::ReportB64Encode("wl:cpu_frame", out, Wire::FRAME_SIZE) && (out[9] & 2))
        s_finishSent |= 1u << id;
}

void RetryFinishedCPUFrames()
{
    auto* race = mkw::RaceManager::Instance();
    if (!s_grid || !IsOwner() || !OwnerAlive() || !RaceScene() || !race || !race->m_players)
        return;
    for (u8 id = s_active.humanCount; id < s_active.humanCount + s_active.cpuCount; ++id) {
        const u16   bit    = 1u << id;
        auto*       player = race->m_players[id];
        const auto* time   = player ? player->m_raceFinishTime : nullptr;
        if (!(s_valid & bit) || (s_finishSent & bit) || Knockout::IsEliminated(id) || !player ||
            !(player->m_stateFlags & mkw::RaceManager::Player::FLAG_FINISHED) || !time ||
            !time->m_valid || time->m_minutes >= 63 || time->m_seconds >= 60 ||
            time->m_milliseconds >= 1000)
            continue;
        const u32 finish =
            time->m_minutes * 60000u + time->m_seconds * 1000u + time->m_milliseconds;
        if (!finish)
            continue;
        if (!SnapshotBudgetAvailable())
            break;
        // Retain the last genuine RACEDATA bytes; only refresh observed finish
        // metadata. Native send callbacks may stop before a final frame queues.
        u8* out = s_frames[id];
        Wire::Write32(out + 4, ++s_sequence);
        Wire::Write16(out + 10, player->m_currentLap);
        out[9] |= 2;
        Wire::Write32(out + 44, finish);
        if (Wire::ValidFrame(out, s_active.epoch, s_active.humanCount, s_active.cpuCount) &&
            GPReport::ReportB64Encode("wl:cpu_frame", out, Wire::FRAME_SIZE))
            s_finishSent |= bit;
    }
}

void ImportMetadata(void* gameModeData, u8 id)
{
    auto* race = mkw::RaceManager::Instance();
    if (!IsCPUSlot(id) || !race || !race->m_players || !race->m_players[id])
        return;
    auto* player = race->m_players[id];
    if (!IsOwner() && HasRaceData(id)) {
        const u8* frame = s_frames[id];
        Store(player, 0xA, Wire::Read16(frame + 32));
        for (u32 offset = 12; offset <= 28; offset += 4)
            Store(player, offset, Wire::ReadFloat(frame + offset));
        player->m_currentLap = Wire::Read16(frame + 10);
        Store(player, 0x26, frame[34]);
        Store(player, 0x27, frame[35]);
        Store(player, 0x28, frame[36]);
        Store(player, 0x21, frame[37]);
        Store(player, 0x2C, Wire::Read32(frame + 40));
        if ((frame[9] & 2) && !Knockout::IsEliminated(id) &&
            !(player->m_stateFlags & (mkw::RaceManager::Player::FLAG_FINISHED |
                                      mkw::RaceManager::Player::FLAG_DISCONNECTED)) &&
            player->m_raceFinishTime) {
            auto time           = *player->m_raceFinishTime;
            u32  ms             = Wire::Read32(frame + 44);
            time.m_minutes      = ms / 60000;
            time.m_seconds      = (ms / 1000) % 60;
            time.m_milliseconds = ms % 1000;
            time.m_valid        = true;
            player->endRace(time);
        }
    }
    if (!gameModeData || !CurrentPlan())
        return;
    // A native GMDataOnlineVS row is 0x14 bytes, starting at object+8.
    // Its received mask must acknowledge observed HUMAN finishes even before
    // this CPU finishes, or the human's RH2 handshake waits indefinitely.
    u16      observedFinishes = 0;
    const u8 total            = s_active.humanCount + s_active.cpuCount;
    for (u8 other = 0; other < total; ++other) {
        const auto* observed = race->m_players[other];
        const auto* time     = observed ? observed->m_raceFinishTime : nullptr;
        if (observed && (observed->m_stateFlags & mkw::RaceManager::Player::FLAG_FINISHED) &&
            time && time->m_valid && time->m_minutes < 63 && time->m_seconds < 60 &&
            time->m_milliseconds < 1000 &&
            (time->m_minutes || time->m_seconds || time->m_milliseconds))
            observedFinishes |= 1u << other;
    }
    u8*         row      = static_cast<u8*>(gameModeData) + id * 0x14;
    const auto* finish   = player->m_raceFinishTime;
    const bool  finished = (player->m_stateFlags & mkw::RaceManager::Player::FLAG_FINISHED) &&
                           finish && finish->m_valid && finish->m_minutes < 63 &&
                           finish->m_seconds < 60 && finish->m_milliseconds < 1000 &&
                           (finish->m_minutes || finish->m_seconds || finish->m_milliseconds);
    // Preserve the native Timer vtable at row+8. CPU rows never confirm or
    // invent another player's finish, nor mark a human disconnected.
    Store<u16>(row, 0xC, finished ? finish->m_minutes : 0);
    Store<u8>(row, 0xE, finished ? finish->m_seconds : 0);
    Store<u16>(row, 0x10, finished ? finish->m_milliseconds : 0);
    Store<bool>(row, 0x12, finished);
    Store<u16>(row, 0x14, observedFinishes);
    Store<u16>(row, 0x16, 0);
    Store<u16>(row, 0x18, 0);
    Store<u16>(row, 0x1A, 0);
}

void Tick()
{
    ++s_ticks;
    if (DWC::stpLoginCnt == nullptr || DWC::stpLoginCnt->state != 5) {
        s_protocol     = false;
        s_waiting      = false;
        s_grid         = false;
        s_allocated    = false;
        s_gridCanceled = false;
        s_committed    = false;
        ResetKnockoutEvents();
        ResetItemStates();
        return;
    }
    if (s_committed && IsOwner() && s_ticks % 60 == 0)
        GPReport::ReportU32("wl:cpu_ping", CurrentPlan()->epoch);
    if (RaceKnockoutEnabled()) {
        RaceKnockoutRosterReady();
        if (IsOwner() && s_courseResultLength && !KnockoutRoundCommitted() &&
            s_ticks - s_lastCourseSend >= 60) {
            s_lastCourseSend = s_ticks;
            GPReport::ReportB64Encode("wl:ko_course", s_courseResult, s_courseResultLength);
        }
    }
    if (s_waiting && !s_committed && s_pending.epoch && !RaceKnockoutSeriesComplete() &&
        s_ticks - s_lastAckTick >= 60) {
        s_lastAckTick = s_ticks;
        GPReport::ReportU32(
            s_pending.knockoutMode == 2 && !s_tournamentReady ? "wl:cpu_plan" : "wl:cpu_ack",
            s_pending.epoch
        );
    }
    if (s_ticks % 3 == 0)
        RetryFinishedCPUFrames();
    if (s_allocated && s_grid && IsOwner() && OwnerAlive() && s_ticks % 3 == 0) {
        for (u8 id = s_active.humanCount; id < s_active.humanCount + s_active.cpuCount; ++id) {
            const u16 bit = 1u << id;
            if (!HasItemState(id) || (!(s_itemDirty & bit) && s_ticks - s_lastItemSend[id] < 30))
                continue;
            if (!SnapshotBudgetAvailable())
                break;
            Wire::Write32(s_items[id] + 4, ++s_itemSequence);
            if (GPReport::ReportB64Encode("wl:cpu_item", s_items[id], Wire::ITEM_STATE_SIZE)) {
                s_itemDirty &= ~bit;
                s_lastItemSend[id] = s_ticks;
            }
        }
    }
    // SELECT can precede the server's QR2 membership snapshot. Retry while
    // there is no proposal, including membership changes before an epoch exists.
    if (s_protocol && s_waiting && !s_pending.epoch && s_ticks - s_lastPrepareTick >= 180) {
        if (++s_nonce == 0)
            ++s_nonce;
        s_lastPrepareTick = s_ticks;
        GPReport::ReportU32("wl:cpu_prepare", s_nonce);
    }
    if (KnockoutEnabled() && IsOwner() && OwnerAlive() && Wire::Read32(s_pendingKnockout + 4) &&
        s_ticks - s_lastKnockoutSend >= 60) {
        s_lastKnockoutSend = s_ticks;
        GPReport::ReportB64Encode("wl:ko_event", s_pendingKnockout, sizeof(s_pendingKnockout));
    }
    if (s_allocated && RaceScene())
        s_sawRaceScene = true;
    if (s_allocated && s_sawRaceScene && !RaceScene()) {
        s_grid      = false;
        s_allocated = false;
        s_committed = false;
        ResetItemStates();
    }
}

WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x800D3648, Transport::Buddy);
} // namespace wwfc::OnlineCPU

#  else
namespace wwfc::OnlineCPU
{
bool RaceKnockoutEnabled()
{
    return false;
}

bool RaceKnockoutSeriesComplete()
{
    return false;
}

bool RaceKnockoutRosterReady()
{
    return false;
}

bool IsLocalProfileSeriesEliminated()
{
    return false;
}

u32 EliminatedSeriesVotingAids(u32, bool)
{
    return 0;
}

u8 InitialKnockoutPlacement(u8)
{
    return 0;
}

u16 InitialKnockoutMask()
{
    return 0;
}

bool KnockoutRoundCommitted()
{
    return false;
}

u16 KnockoutCourseDNFMask()
{
    return 0;
}

bool PublishRaceKnockoutResult(u32)
{
    return false;
}

void RequestRaceKnockoutCommit()
{
}

u8 KnockoutWinnerId()
{
    return 0xff;
}

u16 KnockoutFinalDisconnectedMask()
{
    return 0;
}

bool PublishKnockoutWinner(u32, u8, u16)
{
    return false;
}

bool KnockoutEnabled()
{
    return false;
}

bool KnockoutDecisionPending()
{
    return false;
}

u32 KnockoutFinalBase()
{
    return 0;
}

bool PublishKnockoutFinal(u32)
{
    return false;
}

bool ReadKnockoutEvent(u32, KnockoutEvent&)
{
    return false;
}

bool PublishKnockout(u16, u8, u8)
{
    return false;
}

const Plan* CurrentPlan()
{
    return nullptr;
}

bool IsOwner()
{
    return false;
}

bool IsCPUSlot(u8)
{
    return false;
}

bool OwnerAlive()
{
    return false;
}

bool HasRaceData(u8)
{
    return false;
}

const void* RaceData(u8)
{
    return nullptr;
}

u32 RaceDataTimer(u8)
{
    return 0;
}

bool HasItemState(u8)
{
    return false;
}

const void* ItemState(u8)
{
    return nullptr;
}

void CaptureItemState(u8, const void*, u32)
{
}

bool EventActorOwnedByAid(u8 playerId, u8 senderAid)
{
    return playerId == senderAid;
}

bool GridCanceled()
{
    return false;
}

void CaptureRaceData(u8, const void*, u32)
{
}

void ImportMetadata(void*, u8)
{
}

void OnGridInitialized()
{
}

void GridRejected()
{
}

bool CanSelectAdvance()
{
    return true;
}

void BeginSelection()
{
}

void EndSelection()
{
}

void Tick()
{
}
} // namespace wwfc::OnlineCPU
#  endif
#endif
