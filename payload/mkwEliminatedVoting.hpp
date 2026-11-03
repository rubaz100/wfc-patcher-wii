#pragma once

#include "wwfcBase.hpp"

#if RMC
namespace wwfc::EliminatedVoting
{
// Parse the authenticated reserved self-buddy policy "LEV1:S:0|1". Returns true
// when the message was an LEV1 message (consumed), false otherwise.
bool HandleServerMessage(const char* message);
void ResetSession();
// True when the server policy says eliminated series spectators must not
// influence the next-track vote. Off until the server sends the policy.
bool Enabled();
} // namespace wwfc::EliminatedVoting
#endif
