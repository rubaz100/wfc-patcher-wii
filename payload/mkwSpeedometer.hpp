#pragma once

#if RMC

namespace wwfc::Speedometer
{

// Called every frame (from wwfc::Process::Tick). Reads the local player's kart
// speed and (for now, report-first validation) reports it so the value and a
// km/h scale can be confirmed from the server log before any HUD rendering.
void Tick();

} // namespace wwfc::Speedometer

#endif // RMC
