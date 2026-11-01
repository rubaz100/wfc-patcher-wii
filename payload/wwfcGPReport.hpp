#pragma once

#include <wwfcLibC.hpp>
#include <wwfcTypes.h>

namespace wwfc::GPReport
{

#if RMC

// Accepted records remain pending until the GP append succeeds or that
// connection ends. Callers can retry a full queue without losing pending state.
bool Report(const char* key, const char* string);
bool ReportU32(const char* key, u32 uint);
bool ReportB64Encode(const char* key, const void* data, std::size_t dataSize);
u32  PendingBytes();
// Drain only from the serialized DWC_ProcessFriendsMatch tick, never a kart hook.
void Flush();

#endif

} // namespace wwfc::GPReport
