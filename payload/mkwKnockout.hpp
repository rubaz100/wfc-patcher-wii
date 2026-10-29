#pragma once

#if RMC

#  include "import/mkw/system/RaceManager.hpp"

namespace wwfc::Knockout
{

// Called as each player crosses the finish line (from mkwTime's proven
// per-player finish hook). Reports the finishing player's live rank so
// position@0x20 can be confirmed against the server log.
void OnPlayerFinished(mkw::RaceManager::Player& player);

// Called every frame (from wwfc::Process::Tick). M2 elimination decision
// (leader-lap, last-out): when the race leader completes a lap, computes the
// current last-place player id and reports it (report-only, no elimination
// yet) so the decision can be validated against the server log.
void Tick();

} // namespace wwfc::Knockout

#endif // RMC
