#include "mkwOnlineCPU.hpp"

#if RMC
#  include "mkwOnlineCPUProtocol.hpp"

#  if WWFC_ONLINE_CPU_APPLY && !RMCPD00
#    error "Online CPU native adapters have only been verified for PAL retail"
#  endif

#  if WWFC_ONLINE_CPU_APPLY
#    include "import/dwc.h"
#    include "import/gamespy.h"
#    include "import/mkw/system/RaceConfig.hpp"
#    include "import/mkw/system/RaceManager.hpp"
#    include "import/mkw/system/System.hpp"
#    include "mkwKnockout.hpp"
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
constinit u32           s_itemSequence        = 0;
constinit u32           s_lastItemSend[12]    = {};
constinit u32           s_lastItemReceive[12] = {};
constexpr u32 ITEM_STATE_TIMEOUT_TICKS = 300;
constinit u32           s_ticks = 0, s_lastOwnerTick = 0, s_nonce = 0, s_sequence = 0;
constinit u32           s_lastPrepareTick = 0;
constinit bool          s_protocol = false, s_waiting = false, s_committed = false;
constinit bool          s_grid = false, s_alive = false, s_sawRaceScene = false;
constinit bool          s_allocated = false, s_gridCanceled = false;
constexpr u32           KNOCKOUT_EVENT_SIZE                    = 24;
constinit KnockoutEvent s_knockoutEvents[12]                   = {};
constinit u8            s_knockoutCount                        = 0;
constinit u32           s_knockoutFinalBase                    = 0;
constinit u8            s_pendingKnockout[KNOCKOUT_EVENT_SIZE] = {};
constinit u32           s_lastKnockoutSend                     = 0;

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
    s_knockoutCount     = 0;
    s_knockoutFinalBase = 0;
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
    const auto* connection =
        reinterpret_cast<const GameSpy::GPIConnection*>(*match->connection);
    const s32 output = connection->outputBuffer.length;
    const u32 pending = GPReport::PendingBytes();
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
            !Wire::Number(p, f, '\0') || f > 1 || !a || !b || c == 0 || c > 12 || d > 11 ||
            c + d > 12)
            break;
        if (s_pending.epoch && a != s_pending.epoch && !Wire::Newer(a, s_pending.epoch))
            break;
        if (a != s_pending.epoch) {
            s_valid     = 0;
            s_committed = false;
            ResetKnockoutEvents();
            ResetItemStates();
        }
        s_pending = { a, b, e, static_cast<u8>(c), static_cast<u8>(d), f != 0 };
        GPReport::ReportU32("wl:cpu_ack", a);
        break;
    case 'C':
        if (text[7] == ':' && s_waiting && Wire::Number(p, a, '\0') && s_pending.epoch == a) {
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
        s_lastOwnerTick = s_ticks;
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
            Wire::Read32(bytes + 4) != s_knockoutCount + 1u || bytes[15] ||
            Wire::Read32(bytes + 20))
            break;
        const u16 previousMask =
            s_knockoutCount ? s_knockoutEvents[s_knockoutCount - 1].eliminatedMask : 0;
        const u16 lap = Wire::Read16(bytes + 8);
        const u8  id = bytes[10], place = bytes[11], kind = bytes[14];
        const u8  total = plan.humanCount + plan.cpuCount;
        const u32 base  = Wire::Read32(bytes + 16);
        if (kind == 1) {
            if (!previousMask || lap || id != 0xff || place ||
                Wire::Read16(bytes + 12) != previousMask || !base ||
                base > 63u * 60000u - 1 - MAX_PLAYERS * 10u)
                break;
            s_knockoutFinalBase = base;
        } else if (kind == 0) {
            if (s_knockoutCount >= 11 || base || lap < 2 || lap > 255 || id >= total || place < 2 ||
                place > total || (previousMask & (1u << id)) ||
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
        if (length < 256 && length >= 8 && !std::strncmp(text, "LCPU3:", 6)) {
            Message(text);
            return;
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

bool KnockoutDecisionPending()
{
    return KnockoutEnabled() && Wire::Read32(s_pendingKnockout + 4) != 0;
}

u32 KnockoutFinalBase()
{
    return KnockoutEnabled() ? s_knockoutFinalBase : 0;
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
    ResetKnockoutEvents();
    ResetItemStates();
    if (++s_nonce == 0)
        ++s_nonce;
    s_lastPrepareTick = s_ticks;
    GPReport::ReportU32("wl:cpu_prepare", s_nonce);
}

bool CanSelectAdvance()
{
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
        Wire::ValidFrame(out, s_active.epoch, s_active.humanCount, s_active.cpuCount))
        GPReport::ReportB64Encode("wl:cpu_frame", out, Wire::FRAME_SIZE);
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
