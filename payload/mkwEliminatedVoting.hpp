#pragma once

#include "wwfcBase.hpp"

#if RMC
namespace wwfc::mkw
{
class NetSelectHandler;
}

namespace wwfc::EliminatedVoting
{
// Parse the authenticated reserved self-buddy policy "LEV1:S:0|1". Returns true
// when the message was an LEV1 message (consumed), false otherwise.
bool HandleServerMessage(const char* message);
void ResetSession();
// True when the server policy says eliminated series spectators must not
// influence the next-track vote. Off until the server sends the policy.
bool Enabled();
// Keep a submitted spectator choice visibly Random, using a valid completed
// vote rather than the NOT_SELECTED sentinel that stalls the room.
void NormalizeLocalVote(mkw::NetSelectHandler& selection);
// Choose a surviving voter's course without changing submitted SELECT packets
// or the masks used by the room's loading/stall handshake.
void DecideCourse(mkw::NetSelectHandler& selection);
} // namespace wwfc::EliminatedVoting
#endif
