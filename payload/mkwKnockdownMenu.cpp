// Separate Knockdown entry in the retail online menu, delivered at connection.
// Use the reserved regional VS pool only while this entry is selected.
#if RMC && RMCPD00

#  include "import/mkw/ui/FormatParam.hpp"
#  include "import/mkw/ui/page/MessagePopupPage.hpp"
#  include "import/mkw/ui/section/SectionManager.hpp"
#  include "mkwKnockdownMode.hpp"
#  include "mkwPenalty.hpp"
#  include "wwfcPatch.hpp"

namespace wwfc::KnockdownMenu
{

struct BmgView {
    void* header;
    void* info;
    void* data;
    void* strings;
    void* ids;

    bool loaded() const
    {
        return header && info && data && ids;
    }
};

struct LayoutView {
    void* vtable;
    FILL(0x04, 0xBC);
    /* 0xBC */ void* root;
    FILL(0xC0, 0x144);
    /* 0x144 */ BmgView current;
    /* 0x158 */ BmgView common;
    FILL(0x16C, 0x174);
};

struct ButtonView {
    LayoutView layout;
    FILL(0x174, 0x240);
    /* 0x240 */ s32 buttonId;
    FILL(0x244, 0x254);
};

struct MenuView;

struct ButtonCallback {
    using Function = void (*)(MenuView*, ButtonView*, u32);
    s32      thisAdjustment;
    s32      vtableOffset;
    Function function;
};

struct ButtonHandler {
    void*          vtable;
    MenuView*      object;
    ButtonCallback callback;
};

struct MenuView {
    void*          vtable;
    /* 0x04 */ u32 pageId;
    FILL(0x08, 0x58);
    /* 0x58 */ ButtonHandler regionalClick;
    FILL(0x6C, 0x484);
    /* 0x484 */ ButtonView regionalButton;
    FILL(0x6D8, 0xB98);
    /* 0xB98 */ LayoutView help;
    FILL(0xD0C, 0xF30);
    /* 0xF30 */ u32 nextPage;
};

struct GlobeView {
    void*          vtable;
    /* 0x04 */ u32 pageId;
    FILL(0x08, 0x1CF4);
    /* 0x1CF4 */ u32 searchType;
    /* 0x1CF8 */ u32 mode;
};

static_assert(sizeof(BmgView) == 0x14);
static_assert(__builtin_offsetof(LayoutView, root) == 0xBC);
static_assert(__builtin_offsetof(LayoutView, current) == 0x144);
static_assert(__builtin_offsetof(LayoutView, common) == 0x158);
static_assert(sizeof(LayoutView) == 0x174);
static_assert(__builtin_offsetof(ButtonView, buttonId) == 0x240);
static_assert(sizeof(ButtonView) == 0x254);
static_assert(__builtin_offsetof(ButtonCallback, function) == 0x08);
static_assert(sizeof(ButtonCallback) == 0x0C);
static_assert(__builtin_offsetof(ButtonHandler, callback) == 0x08);
static_assert(sizeof(ButtonHandler) == 0x14);
static_assert(__builtin_offsetof(MenuView, pageId) == 0x04);
static_assert(__builtin_offsetof(MenuView, regionalClick) == 0x58);
static_assert(__builtin_offsetof(MenuView, regionalButton) == 0x484);
static_assert(__builtin_offsetof(MenuView, help) == 0xB98);
static_assert(__builtin_offsetof(MenuView, nextPage) == 0xF30);
static_assert(sizeof(MenuView) == 0xF34);
static_assert(__builtin_offsetof(GlobeView, searchType) == 0x1CF4);
static_assert(__builtin_offsetof(GlobeView, mode) == 0x1CF8);

constexpr wchar_t LABEL[]       = L"Knockdown Mode";
constexpr wchar_t ONE_PLAYER[]  = L"Knockdown Mode requires Nintendo WFC 1 Player.";
constexpr wchar_t UNAVAILABLE[] = L"Knockdown Mode is unavailable on this server.";

// The existing payload uses this custom-match convention for packet policy.
// Keep SystemManager's native region untouched: RKSYS also uses that field
// when writing and validating the save's four-bit region.
extern volatile u32 CustomMatchingArea AT(0x80005EFC);
constinit u32                          s_savedArea = 0;
constinit bool                         s_areaSaved = false;

[[gnu::longcall]] void NativeMenuInit(MenuView*) AT(0x8064B910);
[[gnu::longcall]] void NativeMenuActivate(MenuView*) AT(0x8064BC90);
[[gnu::longcall]] void NativeMenuUpdate(MenuView*) AT(0x805BB22C);
[[gnu::longcall]] void NativeRegionalClick(MenuView*, ButtonView*, u32) AT(0x8064BDC4);
[[gnu::longcall]] void
NativeSetMessage(LayoutView*, u32, const mkw::UI::FormatParam*) AT(0x8063DDB4);
[[gnu::longcall]] void NativeSetHelp(LayoutView*, u32, const mkw::UI::FormatParam*) AT(0x807E9A38);
[[gnu::longcall]] void NativeSetGlobeMode(GlobeView*, u32) AT(0x80609CB8);
[[gnu::longcall]] u32  NativeGlobeNextPage(const GlobeView*) AT(0x8060A670);
[[gnu::longcall]] s32  NativeSnprintf(char*, u32, const char*, ...) AT(0x80011938);

mkw::UI::Section* CurrentSection()
{
    auto* manager = mkw::UI::SectionManager::Instance();
    return manager ? manager->currentSection() : nullptr;
}

bool OneLocalPlayer()
{
    const auto* section = CurrentSection();
    if (!section)
        return false;
    // The native WFC menu uses these same section ranges to distinguish
    // Nintendo WFC 1 Player (0x55..0x57) from 2 Players (0x5B..0x5D).
    const u32 id = *reinterpret_cast<const u32*>(section);
    return id >= 0x55 && id <= 0x57;
}

bool IsMenu(MenuView* page)
{
    auto* section = CurrentSection();
    return page && section && page->vtable == reinterpret_cast<void*>(0x808BFF44) &&
           page->pageId == 0x8B &&
           section->getPage<mkw::UI::Page>(mkw::UI::EPageID::WIFI_MENU_TOP) ==
               reinterpret_cast<mkw::UI::Page*>(page);
}

void RestoreArea()
{
    if (s_areaSaved && CustomMatchingArea == Knockdown::MATCH_REGION)
        CustomMatchingArea = s_savedArea;
    s_areaSaved = false;
    s_savedArea = 0;
    Knockdown::SetSelected(false);
}

s32 FormatRegionalVS(char* output, u32 length, const char* format, const char* mode, u32 region)
{
    // Selection fixes the pool for this search. A later capability revocation
    // must never move an already selected Knockdown search into ordinary VS.
    const u32 selectedRegion = Knockdown::Selected() ? Knockdown::MATCH_REGION : region;
    return NativeSnprintf(output, length, format, mode, static_cast<s32>(selectedRegion));
}

void ShowUnavailable(MenuView* page, const wchar_t* message)
{
    auto* section = CurrentSection();
    if (!IsMenu(page) || !section)
        return;
    mkw::UI::FormatParam format{ .strings = { message } };
    if (page->help.root && page->help.current.loaded() && page->help.common.loaded())
        NativeSetHelp(&page->help, 0x19CA, &format);
    auto* popup = section->getPage<mkw::UI::MessagePopupPage>(mkw::UI::EPageID::POPUP_MESSAGE);
    if (!popup)
        return;
    popup->reset();
    popup->setWindowMessage(0x19CA, &format);
    reinterpret_cast<mkw::UI::Page*>(page)->push(
        mkw::UI::EPageID::POPUP_MESSAGE, mkw::UI::Page::ENextType::FORWARD
    );
}

void SelectKnockdown(MenuView* page, ButtonView* button, u32 hudSlot)
{
    if (!IsMenu(page) || button != &page->regionalButton || button->buttonId != 2)
        return;
    if (!OneLocalPlayer()) {
        RestoreArea();
        ShowUnavailable(page, ONE_PLAYER);
        return;
    }
    if (!Knockdown::ServerAvailable()) {
        RestoreArea();
        ShowUnavailable(page, UNAVAILABLE);
        return;
    }
    auto* section = CurrentSection();
    auto* globe   = reinterpret_cast<GlobeView*>(
        section->getPage<mkw::UI::Page>(static_cast<mkw::UI::EPageID>(0x8F))
    );
    if (!globe || globe->vtable != reinterpret_cast<void*>(0x808BAAD0) || globe->pageId != 0x8F) {
        RestoreArea();
        ShowUnavailable(page, UNAVAILABLE);
        return;
    }
    RestoreArea();
    s_savedArea        = CustomMatchingArea;
    s_areaSaved        = true;
    CustomMatchingArea = Knockdown::MATCH_REGION;
    Knockdown::SetSelected(true);
    // Native RegionalClick sets regional search and the normal exit animation.
    // Native Globe mode 0 selects public VS. Bypass the VS/Battle choice page
    // so this reserved pool can never be entered as a regional battle search.
    NativeRegionalClick(page, button, hudSlot);
    NativeSetGlobeMode(globe, 0);
    page->nextPage = 0x8F;
}

void ConfigureMenu(MenuView* page)
{
    auto& button  = page->regionalButton;
    auto& handler = page->regionalClick;
    if (!IsMenu(page) || button.layout.vtable != reinterpret_cast<void*>(0x808B776C) ||
        !button.layout.root || !button.layout.current.loaded() || !button.layout.common.loaded() ||
        handler.object != page || handler.callback.thisAdjustment != 0 ||
        handler.callback.vtableOffset != -1 ||
        (handler.callback.function != &NativeRegionalClick &&
         handler.callback.function != &SelectKnockdown))
        return;
    // Retail OnInit points the button at its existing member-function holder.
    // Replace only that instance's direct function word at page+0x68; preserve
    // its object, member-call adjustment, and all other button callbacks.
    handler.callback.function = SelectKnockdown;
    // Both 1P and 2P use WFCMainMenu and its WifiMenuSingleTop/ButtonRegion
    // variant, originally Menu BMG 4001. Retail PAL Menu E/F/G/I/S all contain
    // 6602 (0x19CA): string argument 0 only, with the same regular font.
    // Native SetMessage synchronously builds the existing button's glyph cache.
    mkw::UI::FormatParam format{ .strings = { LABEL } };
    NativeSetMessage(&button.layout, 0x19CA, &format);
}

void Init(MenuView* page)
{
    if (!page)
        return;
    RestoreArea();
    NativeMenuInit(page);
    ConfigureMenu(page);
}

void Activate(MenuView* page)
{
    if (!page)
        return;
    // Returning from search/racing or entering Worldwide/Friends restores the
    // prior area. The native Activate body still runs its existing MOTD hook.
    RestoreArea();
    NativeMenuActivate(page);
    // WFCMainMenu may already have been initialized before the connection
    // payload arrived. Its shared vtable still reaches this activation hook.
    ConfigureMenu(page);
}

void Update(MenuView* page)
{
    NativeMenuUpdate(page);
    if (IsMenu(page))
        Penalty::TickMenu();
}

u32 GlobeNextPage(const GlobeView* page)
{
    const u32 next = NativeGlobeNextPage(page);
    // Regional search normally returns to the VS/Battle chooser. Knockdown
    // entered VS directly, so cancel to the main menu, whose Activate restores
    // the previous matching area and clears this mode's selection.
    return Knockdown::Selected() && next == 0x8C ? 0x8B : next;
}

// WFCMainMenu vtable + OnInit (0x28). This data-pointer replacement leaves the
// complete native function and the independent friend-menu/MOTD hooks intact.
WWFC_DEFINE_PATCH = Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808BFF6C, Init);
WWFC_DEFINE_PATCH = Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808BFF74, Activate);
// Only this page's OnUpdate slot; the shared native no-op stays untouched.
WWFC_DEFINE_PATCH = Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808BFF94, Update);
// GlobeSearch vtable + GetNextPage (0x10); preserve all other native destinations.
WWFC_DEFINE_PATCH = Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808BAAE0, GlobeNextPage);

// Replace addi/addi/crclr/bl as complete 16-byte blocks, preserving the
// original buffer/limit and native region in r3/r4/r7. The following native
// branches at 0x80659220 and 0x80659738 remain intact. No native branch,
// symbol, relocation, or data pointer enters either displaced block.
// Only the regional VS format sites are replaced; Battle stays native.
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x80659210,
    ASM_LAMBDA(
        ( : ASM_IMPORT(i, FormatRegionalVS)),
        // clang-format off
        addi r5, r6, 0x06;
        addi r6, r6, 0x0C;
        crclr 6;
        b %[FormatRegionalVS];
        // clang-format on
    )
);
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x80659728,
    ASM_LAMBDA(
        ( : ASM_IMPORT(i, FormatRegionalVS)),
        // clang-format off
        addi r5, r6, 0x89;
        addi r6, r6, 0x8F;
        crclr 6;
        b %[FormatRegionalVS];
        // clang-format on
    )
);

} // namespace wwfc::KnockdownMenu

#endif
