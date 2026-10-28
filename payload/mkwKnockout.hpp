#pragma once

#if RMC

#  include "import/mkw/system/RaceManager.hpp"

namespace wwfc::Knockout
{

// Called as each player crosses the finish line (from mkwTime's proven
// per-player finish hook). M2 sanity-check: reports the finishing player's
// live rank so position@0x20 can be confirmed against the server log.
void OnPlayerFinished(mkw::RaceManager::Player& player);

} // namespace wwfc::Knockout

#endif // RMC
