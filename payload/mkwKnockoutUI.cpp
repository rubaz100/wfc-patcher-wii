// PAL race notice through the actual finish-message control's virtual Update,
// not a guessed control pointer reached through menu/HUD offsets.
#if RMC && WWFC_KNOCKOUT_APPLY && RMCPD00

#  include "import/mkw/ui/FormatParam.hpp"
#  include "mkwKnockout.hpp"
#  include "wwfcPatch.hpp"

namespace wwfc::Knockout
{
namespace RaceNotice
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

struct TextPane {
    FILL(0x00, 0xB8);
    u8 alpha;
};

struct TextHandler {
    TextPane* pane;
};

struct FinishMessageControl {
    void* vtable;
    FILL(0x04, 0x60);
    u8 opacity;
    FILL(0x61, 0x80);
    bool hidden;
    FILL(0x81, 0xBC);
    TextPane* root;
    FILL(0xC0, 0x158);
    BmgView common;
    FILL(0x16C, 0x190);
    u8 hudSlot;
    FILL(0x191, 0x198);
    TextPane* text;
};

static_assert(sizeof(BmgView) == 0x14);
static_assert(__builtin_offsetof(FinishMessageControl, opacity) == 0x60);
static_assert(__builtin_offsetof(FinishMessageControl, hidden) == 0x80);
static_assert(__builtin_offsetof(FinishMessageControl, root) == 0xBC);
static_assert(__builtin_offsetof(FinishMessageControl, common) == 0x158);
static_assert(__builtin_offsetof(FinishMessageControl, hudSlot) == 0x190);
static_assert(__builtin_offsetof(FinishMessageControl, text) == 0x198);

constinit FinishMessageControl* s_lastNoticeControl    = nullptr;
constinit u32                   s_lastNoticeGeneration = 0;

void UpdateFinishNotice(FinishMessageControl* control)
{
    if (!control)
        return;
    [[gnu::longcall]] void nativeUpdate(FinishMessageControl*) AT(0x8063DB00);
    nativeUpdate(control);
    if (control->vtable != reinterpret_cast<void*>(0x808D40C0) || !control->root ||
        !control->text || !control->common.loaded())
        return;
    const wchar_t* notice = EliminationNotice(control->hudSlot);
    if (!notice)
        return;
    u32 generation = NoticeGeneration();
    if (control != s_lastNoticeControl || generation != s_lastNoticeGeneration) {
        [[gnu::longcall]] TextHandler* findText(void*, const char*) AT(0x805E9028);
        TextHandler* handler = findText(reinterpret_cast<u8*>(control) + 0xA8, "textBox_00");
        if (!handler || handler->pane != control->text)
            return;
        // Race's own BMG lacks template 0x19CA. Its language archive also
        // contains Menu.bmg, so use a temporary, non-owning holder explicitly.
        // Native SetMessage synchronously rebuilds the game's glyph cache.
        BmgView                menu = {};
        [[gnu::longcall]] void load(BmgView*, const char*) AT(0x805F8B90);
        load(&menu, "Menu");
        if (!menu.loaded())
            return;
        mkw::UI::FormatParam   format{ .strings = { notice } };
        [[gnu::longcall]] void setMessage(
            TextHandler*, const BmgView&, const BmgView&, u32, const mkw::UI::FormatParam*
        ) AT(0x805CDD00);
        setMessage(handler, menu, control->common, 0x19CA, &format);
        s_lastNoticeControl    = control;
        s_lastNoticeGeneration = generation;
    }
    // After the entire native Update, including its animation/root opacity
    // propagation, so the ordinary unfinished-state fade cannot hide us.
    control->hidden      = false;
    control->opacity     = 255;
    control->root->alpha = 255;
    control->text->alpha = 255;
}

} // namespace RaceNotice

// CtrlRaceWifiFinishMessage::vtable + Update (0x10), PAL only.
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D40D0, RaceNotice::UpdateFinishNotice);

} // namespace wwfc::Knockout
#endif
