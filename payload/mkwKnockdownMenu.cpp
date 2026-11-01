// Rename the retail online menu's regional button through the connected
// payload. The native regional click handler and matchmaking route stay intact.
#if RMC && RMCPD00

#  include "import/mkw/ui/FormatParam.hpp"
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

struct ButtonView {
    void* vtable;
    FILL(0x04, 0xBC);
    /* 0xBC */ void* root;
    FILL(0xC0, 0x144);
    /* 0x144 */ BmgView current;
    /* 0x158 */ BmgView common;
    FILL(0x16C, 0x254);
};

struct MenuView {
    void*          vtable;
    /* 0x04 */ u32 pageId;
    FILL(0x08, 0x484);
    /* 0x484 */ ButtonView regionalButton;
    FILL(0x6D8, 0xF34);
};

static_assert(sizeof(BmgView) == 0x14);
static_assert(__builtin_offsetof(ButtonView, root) == 0xBC);
static_assert(__builtin_offsetof(ButtonView, current) == 0x144);
static_assert(__builtin_offsetof(ButtonView, common) == 0x158);
static_assert(sizeof(ButtonView) == 0x254);
static_assert(__builtin_offsetof(MenuView, pageId) == 0x04);
static_assert(__builtin_offsetof(MenuView, regionalButton) == 0x484);
static_assert(sizeof(MenuView) == 0xF34);

constexpr wchar_t LABEL[] = L"Knockdown Mode";

[[gnu::longcall]] void NativeMenuInit(MenuView*) AT(0x8064B910);
[[gnu::longcall]] void
NativeSetMessage(ButtonView*, u32, const mkw::UI::FormatParam*) AT(0x8063DDB4);

void Init(MenuView* page)
{
    if (!page)
        return;
    NativeMenuInit(page);
    auto& button = page->regionalButton;
    if (page->vtable != reinterpret_cast<void*>(0x808BFF44) || page->pageId != 0x8B ||
        button.vtable != reinterpret_cast<void*>(0x808B776C) || !button.root ||
        !button.current.loaded() || !button.common.loaded())
        return;
    // Both 1P and 2P use WFCMainMenu and its WifiMenuSingleTop/ButtonRegion
    // variant, originally Menu BMG 4001. Retail PAL Menu E/F/G/I/S all contain
    // 6602 (0x19CA): string argument 0 only, with the same regular font.
    // Native SetMessage synchronously builds the existing button's glyph cache.
    mkw::UI::FormatParam format{ .strings = { LABEL } };
    NativeSetMessage(&button, 0x19CA, &format);
}

// WFCMainMenu vtable + OnInit (0x28). This data-pointer replacement leaves the
// complete native function and the independent friend-menu/MOTD hooks intact.
WWFC_DEFINE_PATCH = Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808BFF6C, Init);

} // namespace wwfc::KnockdownMenu

#endif
