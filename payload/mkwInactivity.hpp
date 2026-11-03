#pragma once

#include "wwfcBase.hpp"

#if RMC
namespace wwfc::Inactivity
{
// Only dispatch messages from the authenticated reserved self-buddy channel.
bool HandleServerMessage(const char* message);
void ResetSession();
// No exemption until the current WFC login receives an explicit policy.
bool Enabled();
} // namespace wwfc::Inactivity
#endif
