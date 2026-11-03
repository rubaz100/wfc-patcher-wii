// On-screen speedometer (client-side, no homebrew). The game has no native
// speedometer, so this reads the local kart's current speed and provides it as
// HUD display text. The text is drawn by the finish-message control manager
// (mkwKnockoutUI.cpp), which reuses the one verified SetMessage text path and
// moves the pane into a bottom corner; this module owns the speed read and the
// formatting. It still reports wl:speed for calibration.
//
// Active on PAL retail (RMCPD00) only, where the kart-manager address is known;
// other regions get an empty tick / no display until their addresses are
// verified.

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

// Walk the verified Kart::Manager -> local player -> Physics chain and read the
// local player's current speedNorm. Returns false unless the race is actively
// running and every link is readable.
bool ReadLocalSpeed(float& outSpeed)
{
    RaceManager* race = RaceManager::Instance();
    if (!race || race->m_state != RaceManager::EState::RACE) {
        return false;
    }
    RaceConfigManager* configs = RaceConfigManager::Instance();
    if (!configs) {
        return false;
    }
    RaceConfig& scenario = configs->getConfig();
    const u8    count    = scenario.playerCount();
    const u8    id       = scenario.localPlayerId();
    if (!count || id >= count) {
        return false;
    }

    // Kart::Manager::sInstance (PAL 0x809C18F8) -> players[id] -> Link pointers
    // -> body(+8) -> PhysicsHolder(+0x90) -> Physics(+4); speedNorm @ 0xE0.
    extern void* speedometerKartManager AT(0x809C18F8);
    const void*  manager = speedometerKartManager;
    if (!Readable(manager, 0x28) || Load<u8>(manager, 0x24) < count) {
        return false;
    }
    const void* const* players = Load<const void* const*>(manager, 0x20);
    if (!Readable(players, count * 4) || !Readable(players[id], 4)) {
        return false;
    }
    const void* pointers = Load<const void*>(players[id], 0);
    if (!Readable(pointers, 0x0C)) {
        return false;
    }
    const void* body = Load<const void*>(pointers, 8);
    if (!Readable(body, 0x94)) {
        return false;
    }
    const void* holder = Load<const void*>(body, 0x90);
    if (!Readable(holder, 8)) {
        return false;
    }
    const void* physics = Load<const void*>(holder, 4);
    if (!Readable(physics, 0xE4)) {
        return false;
    }
    outSpeed = Load<float>(physics, 0xE0);
    return true;
}

// Append an ASCII string to the two-byte display buffer, NUL-terminating and
// bounds-checking. Returns false on overflow.
bool Append(wchar_t (&output)[128], u32& used, const char* text)
{
    while (*text) {
        if (used + 1 >= 128) {
            output[used] = 0;
            return false;
        }
        output[used++] = static_cast<wchar_t>(*text++);
    }
    output[used] = 0;
    return true;
}

constinit u32 s_frame = 0;

} // namespace

void Tick()
{
    float speed;
    if (!ReadLocalSpeed(speed)) {
        return;
    }
    // report-first diagnostic: ~2 Hz, speedNorm x100 so decimals are visible.
    if ((s_frame++ % 30) == 0) {
        GPReport::ReportU32("wl:speed", static_cast<u32>(speed * 100.0f));
    }
}

bool CurrentDisplay(wchar_t (&output)[128], u32& generation)
{
    output[0] = 0;
    float speed;
    if (!ReadLocalSpeed(speed)) {
        return false;
    }
    if (speed < 0.0f) {
        speed = 0.0f;
    }
    // speedNorm reads close to km/h on retail karts but is not yet calibrated;
    // show the rounded whole value (units/scale to be tuned from a live view).
    u32 whole = static_cast<u32>(speed + 0.5f);
    if (whole > 999) {
        whole = 999;
    }
    // The number changes only a few times a second; key the glyph cache off it
    // so the HUD rebuilds text only when the displayed value changes.
    generation = 0x20000000u | whole;

    char digits[4];
    u8   len = 0;
    if (whole >= 100) {
        digits[len++] = static_cast<char>('0' + (whole / 100) % 10);
    }
    if (whole >= 10) {
        digits[len++] = static_cast<char>('0' + (whole / 10) % 10);
    }
    digits[len++] = static_cast<char>('0' + whole % 10);
    digits[len]   = 0;

    u32 used = 0;
    if (!Append(output, used, digits) || !Append(output, used, " km/h")) {
        return false;
    }
    return true;
}

} // namespace wwfc::Speedometer

#  else // !RMCPD00

namespace wwfc::Speedometer
{
void Tick()
{
}
bool CurrentDisplay(wchar_t (&)[128], u32&)
{
    return false;
}
} // namespace wwfc::Speedometer

#  endif // RMCPD00

#endif // RMC
