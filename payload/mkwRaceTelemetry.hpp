#pragma once

#include "wwfcBase.hpp"

#if RMC
namespace wwfc::RaceTelemetry
{
// Read-only Host Manager telemetry. This has no CPU/KO policy or game hooks.
void Tick();
} // namespace wwfc::RaceTelemetry
#endif
