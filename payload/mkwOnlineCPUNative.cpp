// PAL retail native adapters for the negotiated online CPU grid. The transport
// owns CPU identity, packet validation, storage and liveness; these hooks only
// bridge verified stock call sites to that state.
#if RMC && WWFC_ONLINE_CPU_APPLY && RMCPD00

#  include "mkwOnlineCPU.hpp"
#  include "wwfcLibC.hpp"
#  include "wwfcPatch.hpp"
#  include "wwfcTypes.h"

namespace wwfc::OnlineCPU::Native
{

constexpr u32 RACE_DATA_SIZE = 0x40;

// The stock decoder normally reads a stable packet-holder buffer. Keep the
// validated CPU copy stable as well, and bind its use to this synchronous
// Receiver::Calc call. A plan transition cannot route a half-decoded CPU to an
// unrelated native AID buffer.
alignas(4) constinit u8 s_receiveCache[MAX_PLAYERS][RACE_DATA_SIZE] = {};
constinit u8 s_decodingPlayerId                                     = 0xFF;

void* RaceConfigInstance()
{
    extern void* onlineCPURaceConfigForNative AT(0x809BD728);
    return onlineCPURaceConfigForNative;
}

void* ResetBuildFlags(u32 playerId)
{
    [[gnu::longcall]] void reset() AT(0x80592FA8);
    reset();

    // Kart's factory sets this byte for MASTER, and the other byte for REMOTE.
    // A CPU owner also needs the local sender/status objects. Keep its scenario
    // type CPU so the native AI is constructed; do not add a local screen.
    if (playerId < MAX_PLAYERS && IsOwner() && IsCPUSlot(static_cast<u8>(playerId))) {
        extern u8 onlineCPUKartBuildIsLocal AT(0x809C1950);
        onlineCPUKartBuildIsLocal = 1;
    }
    // The CTR patch also replaces the next three instructions, which load the
    // race-config singleton and clear r4. Return the same r3 value to the shim.
    return RaceConfigInstance();
}

void ExportRaceData(void* manager, const void* bytes, u32 size, u32 playerId)
{
    if (playerId < MAX_PLAYERS && IsCPUSlot(static_cast<u8>(playerId))) {
        // Never index PacketMgr's two local-player buffers with a CPU ordinal.
        // The native KartNetSender has already packed this complete record.
        if (IsOwner() && bytes && size == RACE_DATA_SIZE) {
            CaptureRaceData(static_cast<u8>(playerId), bytes, size);
        }
        return;
    }

    [[gnu::longcall]] void original(void*, const void*, u32, u32) AT(0x80653960);
    original(manager, bytes, size, playerId);
}

const void* GetRaceData(void* manager, u32 playerId)
{
    // This call site is inside UnpackPackets, whose only native caller is
    // Receiver::Calc. The CPU Calc adapter below requires a complete frame and
    // establishes this context before allowing any stock decoder to run.
    if (playerId < MAX_PLAYERS && playerId == s_decodingPlayerId) {
        return s_receiveCache[playerId];
    }

    [[gnu::longcall]] const void* getNativeRaceData(void*, u32) AT(0x80653ABC);
    return getNativeRaceData(manager, playerId);
}

u32 ReceivedTimer(const void* receiver, void* manager)
{
    [[gnu::longcall]] u8 playerIdOf(const void*) AT(0x80590A5C);
    const u8             id = playerIdOf(receiver);
    if (IsCPUSlot(id))
        return RaceDataTimer(id);
    [[gnu::longcall]] u32 getNativeTimer(void*, u32) AT(0x806544A8);
    return getNativeTimer(manager, id);
}

void CalculateReceivedKart(void* receiver)
{
    if (!receiver)
        return;

    [[gnu::longcall]] u8   playerIdOf(const void*) AT(0x80590A5C);
    [[gnu::longcall]] void original(void*) AT(0x8058998C);
    const u8               playerId = playerIdOf(receiver);
    if (!IsCPUSlot(playerId)) {
        original(receiver);
        return;
    }

    // In particular, never supply a zero/default record while the first CPU
    // frame is still in flight: the decoder would apply it as real physics.
    if (IsOwner() || !OwnerAlive() || !HasRaceData(playerId))
        return;
    const void* packet = RaceData(playerId);
    if (!packet)
        return;

    std::memcpy(s_receiveCache[playerId], packet, RACE_DATA_SIZE);
    const u8 previousPlayerId = s_decodingPlayerId;
    s_decodingPlayerId        = playerId;
    original(receiver);
    s_decodingPlayerId = previousPlayerId;
}

void CalculateReceivedKartFromSub(void* sub, u32 flags)
{
    // Reproduce the native remote-only condition before its two calls. The
    // skipped/local path must still reach the original setup label at 64CC.
    if (!(flags & 0x8))
        return;
    [[gnu::longcall]] void* getReceiver(void*) AT(0x8059144C);
    CalculateReceivedKart(getReceiver(sub));
}

void ImportPlayerMetadata(void* gameModeData, u32 playerId)
{
    if (playerId < MAX_PLAYERS && IsCPUSlot(static_cast<u8>(playerId))) {
        // Stock RH2 imports every non-MASTER slot, including an owner-side CPU,
        // through the two-player-per-AID protocol. CPU metadata uses its owner.
        ImportMetadata(gameModeData, static_cast<u8>(playerId));
    } else {
        [[gnu::longcall]] void importNativeMetadata(void*, u32) AT(0x8053E47C);
        importNativeMetadata(gameModeData, playerId);
    }
}

} // namespace wwfc::OnlineCPU::Native

namespace wwfc::OnlineCPU
{

// Each CallWithCTR replaces a complete sixteen-byte block. No native entry may
// land inside that block: preserve shared labels outside it or replay branches
// inside the shim. Every shim preserves its return address around the C++ call
// and reproduces the displaced effects before returning to +0x10.
// The factory singleton load is performed by its C++ adapter. The GM loop's
// condition-entry label remains in native code, outside its replacement span.

// Kart factory: player ID is retained in nonvolatile r28. Displaced tail:
// lis r3,<RaceConfig>; li r4,0; lwz r3,<RaceConfig>(r3).
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x8058F5D8,
    ASM_LAMBDA(
        ( : ASM_IMPORT_AS(i, Native::ResetBuildFlags, ResetBuildFlags)),
        // clang-format off
        stwu r1, -0x20(r1);
        mflr r0;
        stw r0, 0x24(r1);
        mr r3, r28;
        bl %[ResetBuildFlags];
        lwz r0, 0x24(r1);
        mtlr r0;
        addi r1, r1, 0x20;
        li r4, 0;
        blr;
        // clang-format on
    )
);

// Keep the no-send branch's 0x8058CB18 epilogue entry outside the patch.
// Replace argument setup and the ExportRACEDATA call as one block instead.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x8058CB08,
    ASM_LAMBDA(
        ( : ASM_IMPORT_AS(i, Native::ExportRaceData, ExportRaceData)),
        // clang-format off
        stwu r1, -0x20(r1);
        mflr r0;
        stw r0, 0x24(r1);
        mr r3, r31;
        addi r4, r30, 0x14;
        li r5, 0x40;
        bl %[ExportRaceData];
        lwz r0, 0x24(r1);
        mtlr r0;
        addi r1, r1, 0x20;
        blr;
        // clang-format on
    )
);

// KartNetReceiver::UnpackPackets -> PacketMgr::GetRACEDATA.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x8058A1D0,
    ASM_LAMBDA(
        ( : ASM_IMPORT_AS(i, Native::GetRaceData, GetRaceData)),
        // clang-format off
        stwu r1, -0x20(r1);
        mflr r0;
        stw r0, 0x24(r1);
        bl %[GetRaceData];
        lwz r0, 0x24(r1);
        mtlr r0;
        addi r1, r1, 0x20;
        lwz r12, 0xF8(r1);
        mr r15, r3;
        addi r3, r1, 0xF8;
        blr;
        // clang-format on
    )
);

// clang-format off
// Replace the PlayerIdOf/setup/GetPlayerRH1Timer sequence as one unit.
// This leaves the following compare/branch at 0x80589AB8 intact; replacing
// just its BL with a 16-byte CTR patch would also overwrite that branch.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x80589AA8,
    ASM_LAMBDA(( : ASM_IMPORT_AS(i, Native::ReceivedTimer, ReceivedTimer)), stwu r1, -0x20(r1);
               mflr r0; stw r0, 0x24(r1); mr r4, r27; bl %[ReceivedTimer]; lwz r0, 0x24(r1);
               mtlr r0; addi r1, r1, 0x20; blr;)
);
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x80589ACC,
    ASM_LAMBDA(( : ASM_IMPORT_AS(i, Native::ReceivedTimer, ReceivedTimer)), stwu r1, -0x20(r1);
               mflr r0; stw r0, 0x24(r1); mr r4, r27; bl %[ReceivedTimer]; lwz r0, 0x24(r1);
               mtlr r0; addi r1, r1, 0x20; blr;)
);

// clang-format on
// Replace the remote flag test and both calls together. The native skip path
// lands at 0x805964CC, so that shared entry must remain outside the CTR patch.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x805964BC,
    ASM_LAMBDA(
        ( : ASM_IMPORT_AS(i, Native::CalculateReceivedKartFromSub, CalculateReceivedKartFromSub)),
        // clang-format off
        stwu r1, -0x20(r1);
        mflr r12;
        stw r12, 0x24(r1);
        mr r3, r29;
        mr r4, r0;
        bl %[CalculateReceivedKartFromSub];
        lwz r0, 0x24(r1);
        mtlr r0;
        addi r1, r1, 0x20;
        blr;
        // clang-format on
    )
);

// Replace the complete per-player body, preserving its entry at F2CC and the
// loop-condition entry at F2DC. The initial forward branch at F2C8 targets
// F2DC directly; placing a 16-byte patch at the BL (F2D4) would overwrite that
// label with mtctr and recursively call Update through the stale CTR value.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x8053F2CC,
    ASM_LAMBDA(
        ( : ASM_IMPORT_AS(i, Native::ImportPlayerMetadata, ImportPlayerMetadata)),
        // clang-format off
        stwu r1, -0x20(r1);
        mflr r0;
        stw r0, 0x24(r1);
        mr r3, r31;
        clrlwi r4, r29, 24;
        bl %[ImportPlayerMetadata];
        lwz r0, 0x24(r1);
        mtlr r0;
        addi r1, r1, 0x20;
        addi r29, r29, 1;
        blr;
        // clang-format on
    )
);

} // namespace wwfc::OnlineCPU

#endif
