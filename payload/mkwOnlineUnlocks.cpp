// PAL online selection unlocks. Override availability queries only; saved
// completion records and offline availability retain their native behavior.
#if RMC && RMCPD00

#  include "import/dwc.h"
#  include "import/mkw/Registry.hpp"
#  include "import/mkw/ui/section/SectionManager.hpp"
#  include "wwfcPatch.hpp"

namespace wwfc::mkw::OnlineUnlocks
{

[[gnu::longcall]] bool NativeIsOnlineMenu(u32 sectionId) AT(0x80860484);

static bool Readable(const void* pointer, u32 size)
{
    const u32 address = reinterpret_cast<u32>(pointer);
    return !(address & 3) && size &&
           ((address >= 0x80000000u && address < 0x81800000u && size <= 0x81800000u - address) ||
            (address >= 0x90000000u && address < 0x94000000u && size <= 0x94000000u - address));
}

static bool Active()
{
    // These menu queries also run before login and in offline license/demo
    // paths. Unlock only a connected WFC session's online menu sections.
    const auto* login = DWC::stpLoginCnt;
    if (!Readable(login, 8) || login->state != 5)
        return false;
    auto* manager = UI::SectionManager::Instance();
    if (!Readable(manager, sizeof(*manager)))
        return false;
    const auto* section = manager->currentSection();
    return Readable(section, sizeof(u32)) && NativeIsOnlineMenu(section->id());
}

// Each global per-item query has a full native stack prologue. Resume after
// its four overwritten instructions. CTR hooks work from MEM1 or MEM2; the
// eight-byte menu character helper at 0x8081D020 must not receive a CTR hook.
WWFC_DEFINE_CTR_STUB_SAVE_LR(
    0x8054FEE8, bool NativeCharacterUnlocked(u32, bool),
    stwu r1, -0x50(r1); mflr r0; li r6, 1; stw r0, 0x54(r1);
)

WWFC_DEFINE_CTR_STUB_SAVE_LR(
    0x805500DC, bool NativeVehicleUnlocked(u32, bool),
    stwu r1, -0x30(r1); mflr r0; li r5, 1; stw r0, 0x34(r1);
)

WWFC_DEFINE_CTR_STUB_SAVE_LR(
    0x8054FC54, bool NativeCupUnlocked(u32, u32, bool, bool),
    stwu r1, -0x60(r1); mflr r0; cmpwi r5, 0; stw r0, 0x64(r1);
)

bool CharacterUnlocked(u32 character, bool combineAllLicenses)
{
    // Retail characters and Mii outfits A/B only. Unsupported outfit C,
    // generic Mii selectors and control IDs keep the original query.
    if (IsCharacterValid(static_cast<ECharacter>(character)) && Active())
        return true;
    return NativeCharacterUnlocked(character, combineAllLicenses);
}

bool VehicleUnlocked(u32 vehicle, bool combineAllLicenses)
{
    if (IsVehicleValidVS(static_cast<EVehicle>(vehicle)) && Active())
        return true;
    // The menu's separate weight-class and kart/bike filters stay native.
    // This query's second argument combines licenses; it is not vehicle type.
    return NativeVehicleUnlocked(vehicle, combineAllLicenses);
}

bool CupUnlocked(u32 cup, u32 engineClass, bool mirror, bool combineAllLicenses)
{
    if (cup < 8 && (engineClass <= 2 || mirror) && Active())
        return true;
    return NativeCupUnlocked(cup, engineClass, mirror, combineAllLicenses);
}

WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8054FED8, CharacterUnlocked);
WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x805500CC, VehicleUnlocked);
WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8054FC44, CupUnlocked);

} // namespace wwfc::mkw::OnlineUnlocks

#endif
