// PAL retail ITEM adapters. CPU inventories belong to the negotiated owner;
// native ITEM's two outgoing human buffers are never used for virtual racers.
#if RMC && WWFC_ONLINE_CPU_APPLY && RMCPD00

#  include "import/mkw/system/RaceManager.hpp"
#  include "mkwKnockout.hpp"
#  include "mkwOnlineCPU.hpp"
#  include "wwfcLibC.hpp"
#  include "wwfcPatch.hpp"

namespace wwfc::OnlineCPU::Items
{

constexpr u8 NONE = 0x14;

struct Packet {
    u8 timer, stored, dragged, mode, tailMode, acknowledge, timer1, timer2;
};

static_assert(sizeof(Packet) == 8);
constinit Packet s_packets[MAX_PLAYERS] = {};
constinit u32    s_epoch                = 0;

template <class T>
T Load(const void* object, u32 offset)
{
    T value;
    std::memcpy(&value, static_cast<const u8*>(object) + offset, sizeof(value));
    return value;
}

template <class T>
void Store(void* object, u32 offset, T value)
{
    std::memcpy(static_cast<u8*>(object) + offset, &value, sizeof(value));
}

Packet EmptyPacket()
{
    return { 0, NONE, NONE, 0, 0, 0, 0, 0 };
}

void CheckEpoch()
{
    const auto* plan  = CurrentPlan();
    const u32   epoch = plan ? plan->epoch : 0;
    if (epoch == s_epoch)
        return;
    s_epoch = epoch;
    for (u8 id = 0; id < MAX_PLAYERS; ++id)
        s_packets[id] = EmptyPacket();
}

void* Handler()
{
    extern void* onlineCPUItemHandler AT(0x809C20F8);
    return onlineCPUItemHandler;
}

bool CPUPlayer(const void* player)
{
    return player && IsCPUSlot(Load<u8>(player, 0x18));
}

bool Active(u8 id)
{
    auto* race = mkw::RaceManager::Instance();
    if (!IsCPUSlot(id) || !OwnerAlive() || Knockout::IsEliminated(id) || !race ||
        race->m_state != mkw::RaceManager::EState::RACE || !race->m_players || !race->m_players[id])
        return false;
    const u32 terminal = mkw::RaceManager::Player::FLAG_FINISHED |
                         mkw::RaceManager::Player::FLAG_DISCONNECTED |
                         mkw::RaceManager::Player::FLAG_RETIRED;
    return !(race->m_players[id]->m_stateFlags & terminal);
}

u32 FrameCounter()
{
    // SetItem retail 8065C6C0 reads RaceManager+20, not its Timer pointer.
    const auto* race = mkw::RaceManager::Instance();
    return race ? Load<u32>(race, 0x20) : 0;
}

void Inject(void* handler, u8 id, const Packet& packet)
{
    if (!handler)
        return;
    // Verified receivedPackets[12] begins at +10; CPU global IDs are bounded
    // by IsCPUSlot. All stock non-local getters read this row directly.
    auto*      bytes    = static_cast<u8*>(handler);
    const auto previous = Load<Packet>(handler, 0x10 + id * sizeof(Packet));
    std::memcpy(bytes + 0x10 + id * sizeof(Packet), &packet, sizeof(packet));
    if (previous.timer != packet.timer || previous.stored != packet.stored) {
        // Unwrap stock ImportNewPackets' /8 timer against the full local clock.
        // The receive path uses these times for simultaneous-item arbitration.
        const u32 frame   = FrameCounter();
        const u8  now     = static_cast<u8>(frame >> 3);
        const s8  age     = static_cast<s8>(now - packet.timer);
        const u32 elapsed = static_cast<u32>(static_cast<s32>(age) * 8);
        Store<u32>(handler, 0xA0 + id * 4, (frame & ~7u) - elapsed);
    }
}

void InjectAll(void* handler)
{
    CheckEpoch();
    for (u8 id = 0; id < MAX_PLAYERS; ++id) {
        if (!IsCPUSlot(id))
            continue;
        Packet packet = EmptyPacket();
        if (Active(id)) {
            if (IsOwner()) {
                packet = s_packets[id];
            } else if (const void* state = ItemState(id)) {
                std::memcpy(&packet, state, sizeof(packet));
            }
        }
        Inject(handler, id, packet);
    }
}

// These are precisely the first sixteen displaced bytes of each retail
// function. Their continuations preserve native human/offline behavior.
WWFC_DEFINE_CTR_STUB(
    0x80797938, void NativeUpdatePlayer(void*), stwu r1, -0x40(r1); mflr r0; stw r0, 0x44(r1);
    stfd f31, 0x30(r1);
)
WWFC_DEFINE_CTR_STUB(
    0x80798C48, void NativeDecide(void*, u32, u32, u32), stwu r1, -0x20(r1); mflr r0;
    stw r0, 0x24(r1); stw r31, 0x1C(r1);
)
WWFC_DEFINE_CTR_STUB_SAVE_LR(
    0x807BC950, void NativeSetInventory(void*, u32, bool), mulli r0, r4, 0x1C; lis r6, 0x809C;
    stw r4, 4(r3); li r4, 0;
)
WWFC_DEFINE_CTR_STUB(
    0x8065C6D0, void NativeSetItem(void*, u32, u32), stwu r1, -0x20(r1); mflr r0; lis r6, 0x809C;
    stw r0, 0x24(r1);
)
WWFC_DEFINE_CTR_STUB(
    0x8065C75C, void NativeRemoveItem(void*, u32), stwu r1, -0x20(r1); mflr r0; stw r0, 0x24(r1);
    stw r31, 0x1C(r1);
)
WWFC_DEFINE_CTR_STUB(
    0x8065C7F0, void NativeRouletteEnd(void*, u32), stwu r1, -0x20(r1); mflr r0; stw r0, 0x24(r1);
    stw r31, 0x1C(r1);
)
WWFC_DEFINE_CTR_STUB(
    0x8065C5F8, void NativeUpdateHandler(void*), stwu r1, -0x20(r1); mflr r0; lis r4, 0x809C;
    stw r0, 0x24(r1);
)

void Snapshot(void* player, u8 id)
{
    auto&     packet    = s_packets[id];
    const u32 held      = Load<u32>(player, 0x8C);
    const u32 heldCount = Load<u32>(player, 0x90);
    if (held < 0x13 && heldCount && heldCount <= 3) {
        packet.stored = static_cast<u8>(held);
        // Retail UpdateStoredItem: 4/5/6 mean three/two/one remaining.
        packet.mode = static_cast<u8>(7 - heldCount);
    } else if (!Load<u32>(player, 0x58)) {
        packet.stored = NONE;
        packet.mode   = 0;
    }

    // Reproduce UpdateDraggedItem's count and odd/even generation phases.
    // Flattening these to a single 'has item' bit respawns fired objects and
    // loses the remaining triple-shell/banana count on remote machines.
    const bool odd      = packet.tailMode >= 1 && packet.tailMode <= 3;
    const bool even     = packet.tailMode >= 5 && packet.tailMode <= 7;
    const u32  objId    = Load<u32>(player, 0xC8);
    const u32  dragged  = Load<u32>(player, 0xCC);
    const u32  count    = Load<u32>(player, 0x104);
    const bool trailing = objId != 0x10 && !Load<u8>(player, 0x208);
    if (trailing && dragged < 0x13 && count && count <= 3) {
        const bool changed = (odd || even) && packet.dragged != dragged;
        const bool noOrOdd = packet.tailMode == 0 || odd;
        packet.tailMode    = static_cast<u8>(4 - count);
        if (changed ? noOrOdd : !noOrOdd)
            packet.tailMode += 4;
        packet.dragged = static_cast<u8>(dragged);
    } else if (!trailing) {
        if (odd)
            packet.tailMode = 4;
        else if (even)
            packet.tailMode = 0;
        packet.dragged = NONE;
    }
    packet.acknowledge = packet.timer1 = packet.timer2 = 0;
    Inject(Handler(), id, packet);
    CaptureItemState(id, &packet, sizeof(packet));
}

void ClearPlayer(void* player, u8 id)
{
    s_packets[id] = EmptyPacket();
    Inject(Handler(), id, s_packets[id]);
    // Inventory::ClearAll is a verified leaf reset, with no network access.
    [[gnu::longcall]] void clearInventory(void*) AT(0x807BC9C0);
    clearInventory(static_cast<u8*>(player) + 0x88);
    // Stop a pending roulette without calling its cancellation network writer.
    Store<u32>(player, 0x58, 0);
    Store<u8>(player, 0x81, 0);
    // The normal remote reconciler destroys held/trailing objects when its
    // received row becomes empty. Do not run owner use/AI after retirement.
    [[gnu::longcall]] void updateRemote(void*) AT(0x80795668);
    updateRemote(static_cast<u8*>(player) + 0xB4);
}

void UpdatePlayer(void* player)
{
    if (!CPUPlayer(player)) {
        NativeUpdatePlayer(player);
        return;
    }
    CheckEpoch();
    const u8 id = Load<u8>(player, 0x18);
    if (!Active(id)) {
        ClearPlayer(player, id);
        return;
    }
    if (IsOwner()) {
        Inject(Handler(), id, s_packets[id]);
        NativeUpdatePlayer(player);
        Snapshot(player, id);
    } else {
        Packet packet = EmptyPacket();
        if (const void* state = ItemState(id))
            std::memcpy(&packet, state, sizeof(packet));
        Inject(Handler(), id, packet);
        // REMOTE inventory remains empty, exactly as for stock human ghosts.
        // UpdateRemote consumes the ITEM row; EVENT creates remote use effects.
        // Populating Inventory would execute its local-use path a second time.
        NativeUpdatePlayer(player);
    }
}

void Decide(void* player, u32 playerBox, u32 cpuBox, u32 lotteryType)
{
    if (!CPUPlayer(player) || (IsOwner() && Active(Load<u8>(player, 0x18))))
        NativeDecide(player, playerBox, cpuBox, lotteryType);
}

void SetInventory(void* inventory, u32 item, bool forced)
{
    const void* player = inventory ? Load<void*>(inventory, 0) : nullptr;
    if (!CPUPlayer(player) || (IsOwner() && Active(Load<u8>(player, 0x18))))
        NativeSetInventory(inventory, item, forced);
}

void SetItem(void* handler, u32 playerId, u32 item)
{
    if (playerId >= MAX_PLAYERS || !IsCPUSlot(static_cast<u8>(playerId))) {
        NativeSetItem(handler, playerId, item);
        return;
    }
    CheckEpoch();
    const u8 id = static_cast<u8>(playerId);
    if (!IsOwner() || !Active(id) || item >= 0x13)
        return;
    auto& packet  = s_packets[id];
    packet.timer  = static_cast<u8>(FrameCounter() >> 3);
    packet.stored = static_cast<u8>(item);
    // CPU lottery decisions have one authority. Human ITEM acknowledgement
    // loops exclude CPU IDs, so leaving DECIDED(1) would stall the roulette.
    packet.mode = 2;
    Inject(handler, id, packet);
    Store<u32>(handler, 0xA0 + id * 4, FrameCounter() & ~7u);
}

void RemoveItem(void* handler, u32 playerId)
{
    if (playerId >= MAX_PLAYERS || !IsCPUSlot(static_cast<u8>(playerId))) {
        NativeRemoveItem(handler, playerId);
        return;
    }
    CheckEpoch();
    if (IsOwner()) {
        auto& packet  = s_packets[playerId];
        packet.stored = NONE;
        packet.mode   = 0;
        Inject(handler, static_cast<u8>(playerId), packet);
    }
}

void RouletteEnd(void* handler, u32 playerId)
{
    if (playerId >= MAX_PLAYERS || !IsCPUSlot(static_cast<u8>(playerId))) {
        NativeRouletteEnd(handler, playerId);
        return;
    }
    // The next Player update publishes the actual inventory count. RESET(3)
    // is internal to native outgoing packets and is never relayed as a state.
    CheckEpoch();
    if (IsOwner() && Active(static_cast<u8>(playerId))) {
        s_packets[playerId].mode = 2;
        Inject(handler, static_cast<u8>(playerId), s_packets[playerId]);
    }
}

void UpdateHandler(void* handler)
{
    InjectAll(handler);
    NativeUpdateHandler(handler);
    InjectAll(handler);
}

} // namespace wwfc::OnlineCPU::Items

namespace wwfc::OnlineCPU
{
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x80797928, Items::UpdatePlayer);
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x80798C38, Items::Decide);
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x807BC940, Items::SetInventory);
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8065C6C0, Items::SetItem);
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8065C74C, Items::RemoveItem);
WWFC_DEFINE_PATCH = Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8065C7E0, Items::RouletteEnd);
WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8065C5E8, Items::UpdateHandler);
} // namespace wwfc::OnlineCPU

#endif
