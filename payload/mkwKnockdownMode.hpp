#pragma once

#include "wwfcBase.hpp"

#if RMC
namespace wwfc::Knockdown
{
constexpr u32 MATCH_REGION     = 19279; // 0x4B4F; server RK key vs_19279.
constexpr u32 PROTOCOL_VERSION = 2;

bool ServerAvailable();
bool Selected();
void SetSelected(bool selected);
void ResetSession();
void HandleServerMessage(const char* message);
void HandleLoginResponse(const char* message);
void Tick();
} // namespace wwfc::Knockdown
#endif
