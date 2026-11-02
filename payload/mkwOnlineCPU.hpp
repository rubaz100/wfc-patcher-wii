#pragma once

#include "wwfcBase.hpp"

#ifndef WWFC_ONLINE_CPU_APPLY
#  define WWFC_ONLINE_CPU_APPLY 0
#endif

#if RMC
namespace wwfc::mkw
{
class RaceConfig;
}

namespace wwfc::OnlineCPU
{

constexpr u8  MAX_PLAYERS      = 12;
constexpr u32 PROTOCOL_VERSION = 3;

struct Plan {
    u32  epoch;
    u32  ownerProfileId;
    u32  seed;
    u8   humanCount;
    u8   cpuCount;
    bool knockoutEnabled;
};

// Owner decisions are relayed by the authenticated server in a bounded ordered
// stream. Guests never select eliminations from their delayed position table.
struct KnockoutEvent {
    u32 sequence;
    u16 leaderLap;
    u8  playerId;
    u8  placement;
    u16 eliminatedMask;
};

bool KnockoutEnabled();
bool KnockoutDecisionPending();
u32  KnockoutFinalBase();
u8   KnockoutWinnerId();
u16  KnockoutFinalDisconnectedMask();
bool PublishKnockoutWinner(u32 baseMilliseconds, u8 winnerId, u16 disconnectedMask);
bool PublishKnockoutFinal(u32 baseMilliseconds);
bool ReadKnockoutEvent(u32 afterSequence, KnockoutEvent& event);
bool PublishKnockout(u16 leaderLap, u8 playerId, u8 placement);

// The transport owns plans, storage and liveness. Native adapters never invent
// participants or read outside the committed CPU tail of the 12-slot grid.
const Plan* CurrentPlan();
bool        IsOwner();
bool        IsCPUSlot(u8 playerId);
bool        OwnerAlive();
bool        HasRaceData(u8 playerId);
const void* RaceData(u8 playerId);
u32         RaceDataTimer(u8 playerId);
// Native 8-byte ITEM snapshots are owned by the CPU authority and never use
// PacketMgr's two local-human outgoing slots. Callers apply them only in-race.
bool        HasItemState(u8 playerId);
const void* ItemState(u8 playerId);
void        CaptureItemState(u8 playerId, const void* bytes, u32 size);
// Native EVENT source checks use per-racer ownership, including a console's
// second local human and the negotiated owner's virtual CPU racers.
bool EventActorOwnedByAid(u8 playerId, u8 senderAid);
bool GridCanceled();
void CaptureRaceData(u8 playerId, const void* bytes, u32 size);
void ImportMetadata(void* gameModeData, u8 playerId);
void OnGridInitialized();
void GridRejected();

// SELECT is held while this server and its clients agree the next race grid.
bool CanSelectAdvance();
void BeginSelection();
void EndSelection();
void Tick();

} // namespace wwfc::OnlineCPU
#endif
