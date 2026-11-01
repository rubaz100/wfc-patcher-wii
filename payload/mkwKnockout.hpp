#pragma once

#if RMC

#  include "import/mkw/system/RaceManager.hpp"

namespace wwfc::Knockout
{

// Called as each player crosses the finish line (from mkwTime's proven
// per-player finish hook). Reports the finishing player's live rank so
// position@0x20 can be confirmed against the server log.
void OnPlayerFinished(mkw::RaceManager::Player& player);

// Synthetic knockout finishes must not be rewritten/reported as ordinary
// race times by mkwTime's shared retail finish hook.
bool           NormalizeRetiredFinish(mkw::RaceManager::Player& player);
bool           IsSpectatorTestRoom();
bool           IsEliminated(u8 playerId);
const wchar_t* EliminationNotice(u8 hudSlot);
bool           CopyEliminationNotice(u8 hudSlot, wchar_t (&output)[128], u32& generation);
u32            NoticeGeneration();

// Called every frame (from wwfc::Process::Tick). Reports lap-boundary
// decisions. Active PAL builds apply only server-negotiated, owner-authored
// decisions; ordinary races and incompatible rooms never apply eliminations.
void Tick();

} // namespace wwfc::Knockout

#endif // RMC
