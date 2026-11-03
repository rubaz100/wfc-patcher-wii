#pragma once

#if RMC

namespace wwfc::Speedometer
{

// Called every frame (from wwfc::Process::Tick). Reads the local player's kart
// speed and reports it (diagnostic wl:speed) for calibration.
void Tick();

// Fills `output` with the local kart's current speed as HUD display text and
// returns true when a speed is available (active race, readable local kart).
// `generation` changes only when the displayed value changes, so the caller can
// skip rebuilding the glyph cache on unchanged frames. Returns false (output
// left empty) when nothing should be shown. The real body is PAL-only.
bool CurrentDisplay(wchar_t (&output)[128], u32& generation);

} // namespace wwfc::Speedometer

#endif // RMC
