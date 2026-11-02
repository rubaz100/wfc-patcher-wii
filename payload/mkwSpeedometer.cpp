// On-screen speedometer (client-side, no homebrew). The game has no native
// speedometer, so this reads the local kart's current speed and will render it
// on the HUD. STATUS: report-first — read the local player's Kart::Physics
// speedNorm each frame via the verified Kart::Manager -> Link -> Physics chain
// (the same chain the live-telemetry map uses) and report it throttled, so the
// value and a display scale can be confirmed before touching the HUD.
//
// Active on PAL retail (RMCPD00) only, where the kart-manager address is known;
// other regions get an empty tick until their addresses are verified.

#if RMC

#  include "mkwSpeedometer.hpp"

#  if RMCPD00

#    include "import/mkw/system/RaceConfig.hpp"
#    include "import/mkw/system/RaceManager.hpp"
#    include "wwfcGPReport.hpp"
#    include "wwfcTypes.h"

namespace wwfc::Speedometer
{

namespace
{

using namespace mkw;

template <class T>
T Load(const void* object, u32 offset)
{
    T value;
    __builtin_memcpy(&value, static_cast<const u8*>(object) + offset, sizeof(T));
    return value;
}

// Non-null, word-aligned, and entirely within MEM1 or MEM2.
bool Readable(const void* pointer, u32 bytes)
{
    const u32 address = reinterpret_cast<u32>(pointer);
    if (address == 0 || (address & 3) != 0) {
        return false;
    }
    const u32 end = address + bytes;
    if (end < address) {
        return false;
    }
    return (address >= 0x80000000u && end <= 0x81800000u) ||
           (address >= 0x90000000u && end <= 0x94000000u);
}

constinit u32 s_frame = 0;

} // namespace

void Tick()
{
    RaceManager* race = RaceManager::Instance();
    if (!race || race->m_state != RaceManager::EState::RACE) {
        return;
    }
    RaceConfigManager* configs = RaceConfigManager::Instance();
    if (!configs) {
        return;
    }
    RaceConfig& scenario = configs->getConfig();
    const u8    count    = scenario.playerCount();
    const u8    id       = scenario.localPlayerId();
    if (!count || id >= count) {
        return;
    }

    // Kart::Manager::sInstance (PAL 0x809C18F8) -> players[id] -> Link pointers
    // -> body(+8) -> PhysicsHolder(+0x90) -> Physics(+4); speedNorm @ 0xE0.
    extern void* speedometerKartManager AT(0x809C18F8);
    const void*  manager = speedometerKartManager;
    if (!Readable(manager, 0x28) || Load<u8>(manager, 0x24) < count) {
        return;
    }
    const void* const* players = Load<const void* const*>(manager, 0x20);
    if (!Readable(players, count * 4) || !Readable(players[id], 4)) {
        return;
    }
    const void* pointers = Load<const void*>(players[id], 0);
    if (!Readable(pointers, 0x0C)) {
        return;
    }
    const void* body = Load<const void*>(pointers, 8);
    if (!Readable(body, 0x94)) {
        return;
    }
    const void* holder = Load<const void*>(body, 0x90);
    if (!Readable(holder, 8)) {
        return;
    }
    const void* physics = Load<const void*>(holder, 4);
    if (!Readable(physics, 0xE4)) {
        return;
    }
    const float speedNorm = Load<float>(physics, 0xE0);

    // report-first: ~2 Hz, speedNorm x100 so decimals are visible in the log.
    if ((s_frame++ % 30) == 0) {
        GPReport::ReportU32("wl:speed", static_cast<u32>(speedNorm * 100.0f));
    }
}

} // namespace wwfc::Speedometer

#  else // !RMCPD00

namespace wwfc::Speedometer
{
void Tick()
{
}
} // namespace wwfc::Speedometer

#  endif // RMCPD00

#endif // RMC
