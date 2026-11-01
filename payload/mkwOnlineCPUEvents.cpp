// The retail EVENT queue is shared by a console, and its item records identify
// racers by the full race-grid ID. Unlike the outgoing ITEM/RACE arrays, it has
// no two-local-player index to adapt. Keep its native object lifetime, collision
// reports, retries and duplicate suppression intact.
#if RMC && WWFC_ONLINE_CPU_APPLY && RMCPD00

#  include "import/dwc.h"
#  include "import/mkw/net/NetManager.hpp"
#  include "mkwOnlineCPU.hpp"

namespace wwfc::OnlineCPU
{

bool EventActorOwnedByAid(u8 playerId, u8 senderAid)
{
    if (playerId >= MAX_PLAYERS || senderAid >= MAX_PLAYERS)
        return false;

    const auto* manager = mkw::NetManager::Instance();
    if (!manager)
        return false;

    if (!IsCPUSlot(playerId)) {
        // This is the native global-racer-to-console map, also used by
        // EVENTHandler::ProcessReceivedEntries. Both local humans may belong
        // to the same AID; comparing the racer ID itself with the AID is wrong.
        const auto* playerAids = reinterpret_cast<const u8*>(manager) + 0x2920;
        return playerAids[playerId] == senderAid;
    }

    // A virtual CPU has no AID of its own. Only the authenticated console that
    // owns the committed grid may originate its USE events (including TC).
    const Plan* plan = CurrentPlan();
    if (!plan || !OwnerAlive())
        return false;

    if (senderAid == manager->myAid()) {
        const auto* user = DWC::DWCi_GetUserData();
        return IsOwner() && user && static_cast<u32>(user->profileId) == plan->ownerProfileId;
    }

    if (!(manager->availableAids() & (1u << senderAid)))
        return false;

    const auto* node = DWC::DWCi_NodeInfoList_GetNodeInfoForAid(senderAid);
    return node && node->profileId == plan->ownerProfileId;
}

} // namespace wwfc::OnlineCPU

#endif
