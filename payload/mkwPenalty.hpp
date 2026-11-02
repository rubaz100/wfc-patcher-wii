#pragma once

#include "wwfcBase.hpp"

#if RMC
namespace wwfc::Penalty
{
// Reserved authenticated self-buddy channel only. This copies the request;
// native save work is deferred to the WFC menu's main-thread update.
bool HandleServerMessage(const char* message);
void TickMenu();
} // namespace wwfc::Penalty
#endif
