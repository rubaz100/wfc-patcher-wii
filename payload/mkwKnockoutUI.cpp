// PAL race notice through the actual finish-message control's virtual Update,
// not a guessed control pointer reached through menu/HUD offsets.
#if RMC && WWFC_KNOCKOUT_APPLY && RMCPD00

#  include "import/mkw/system/RaceConfig.hpp"
#  include "import/mkw/system/RaceManager.hpp"
#  include "import/mkw/ui/FormatParam.hpp"
#  include "mkwKnockout.hpp"
#  include "mkwKnockoutNotice.hpp"
#  include "mkwOnlineCPU.hpp"
#  include "wwfcLibC.hpp"
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
    FILL(0xC0, 0x144);
    BmgView current;
    BmgView common;
    FILL(0x16C, 0x190);
    u8 hudSlot;
    FILL(0x191, 0x198);
    TextPane* text;
    u32       nativeMessageId;
};

static_assert(sizeof(BmgView) == 0x14);
static_assert(__builtin_offsetof(FinishMessageControl, opacity) == 0x60);
static_assert(__builtin_offsetof(FinishMessageControl, hidden) == 0x80);
static_assert(__builtin_offsetof(FinishMessageControl, root) == 0xBC);
static_assert(__builtin_offsetof(FinishMessageControl, current) == 0x144);
static_assert(__builtin_offsetof(FinishMessageControl, common) == 0x158);
static_assert(__builtin_offsetof(FinishMessageControl, hudSlot) == 0x190);
static_assert(__builtin_offsetof(FinishMessageControl, text) == 0x198);
static_assert(__builtin_offsetof(FinishMessageControl, nativeMessageId) == 0x19C);
static_assert(sizeof(FinishMessageControl) == 0x1A0);

constinit FinishMessageControl* s_lastNoticeControl    = nullptr;
constinit TextPane*             s_lastNoticePane       = nullptr;
constinit mkw::RaceManager*     s_lastNoticeRace       = nullptr;
constinit u32                   s_lastNoticeGeneration = 0;
constinit u32                   s_lastNativeMessageId  = 0;

bool CopyRaceNotice(u8 slot, wchar_t (&output)[128], u32& generation)
{
    if (CopyEliminationNotice(slot, output, generation)) {
        if (OnlineCPU::RaceKnockoutEnabled()) {
            wchar_t spectator[128];
            std::memcpy(spectator, output, sizeof(spectator));
            generation ^= 0x40000000u;
            return FormatSeriesSpectatorNotice(output, spectator);
        }
        return true;
    }
    u8    remaining, total, winner;
    auto* race    = mkw::RaceManager::Instance();
    auto* configs = mkw::RaceConfigManager::Instance();
    if (slot || !race || !configs || !race->m_players ||
        race->m_state != mkw::RaceManager::EState::RACE ||
        !ContinuousRaceInfo(remaining, total, winner))
        return false;
    for (u8 id = 0; id < total; ++id) {
        if (configs->getConfig().getPlayer(id)->m_type !=
            mkw::RaceConfigPlayer::EPlayerType::MASTER)
            continue;
        const auto* player = race->m_players[id];
        if (!player || player->m_id != id ||
            (player->m_stateFlags & (mkw::RaceManager::Player::FLAG_FINISHED |
                                     mkw::RaceManager::Player::FLAG_DISCONNECTED)))
            return false;
        if (OnlineCPU::RaceKnockoutEnabled()) {
            const auto* plan = OnlineCPU::CurrentPlan();
            if (!plan)
                return false;
            generation = 0xC0000000u | (remaining << 16) | (plan->seriesRound << 8) |
                         (plan->knockoutEliminationCount << 4) | total;
            return FormatRaceKnockoutNotice(
                output, remaining, total, plan->seriesRound, plan->knockoutEliminationCount
            );
        }
        if (player->m_currentLap > total)
            return false;
        const u8 lap = static_cast<u8>(player->m_currentLap);
        generation   = 0x80000000u | (remaining << 16) | (lap << 8) | total;
        return FormatKnockdownRaceNotice(output, remaining, total, lap);
    }
    return false;
}

struct LapControl {
    void* vtable;
    FILL(0x04, 0x80);
    bool hidden;
};

static_assert(__builtin_offsetof(LapControl, hidden) == 0x80);
[[gnu::longcall]] void NativeLapInit(LapControl*) AT(0x807EF6E8);
[[gnu::longcall]] void NativeLapUpdate(LapControl*) AT(0x8063DB00);

void InitLapControl(LapControl* control)
{
    if (!control)
        return;
    if (control && control->vtable == reinterpret_cast<void*>(0x808D3D18) &&
        ContinuousLapCount() > 9) {
        // The retail counter uses a single 0..9 texture frame. The text notice
        // carries the full lap number instead; never feed 10..12 to that asset.
        control->hidden = true;
        return;
    }
    NativeLapInit(control);
}

void UpdateLapControl(LapControl* control)
{
    if (!control)
        return;
    if (control && control->vtable == reinterpret_cast<void*>(0x808D3D18) &&
        ContinuousLapCount() > 9) {
        control->hidden = true;
        return;
    }
    NativeLapUpdate(control);
}

void ClearNoticeCache()
{
    s_lastNoticeControl    = nullptr;
    s_lastNoticePane       = nullptr;
    s_lastNoticeRace       = nullptr;
    s_lastNoticeGeneration = 0;
    s_lastNativeMessageId  = 0;
}

TextHandler* FindText(FinishMessageControl* control)
{
    [[gnu::longcall]] TextHandler* findText(void*, const char*) AT(0x805E9028);
    TextHandler* handler = findText(reinterpret_cast<u8*>(control) + 0xA8, "textBox_00");
    return handler && handler->pane == control->text ? handler : nullptr;
}

bool RestoreNativeMessage(FinishMessageControl* control)
{
    auto* handler = FindText(control);
    if (!handler)
        return false;
    // SetPanePosition keeps the native status message ID separately from the
    // glyph cache. Our direct SetMessage never changes that ID. Prefer its
    // CURRENT value: native Update may have selected a new result this frame.
    if (control->nativeMessageId) {
        [[gnu::longcall]] void setMessage(
            TextHandler*, const BmgView&, const BmgView&, u32, const mkw::UI::FormatParam*
        ) AT(0x805CDD00);
        setMessage(handler, control->current, control->common, control->nativeMessageId, nullptr);
    } else {
        // Status 1 (ordinary completion) has no native finish warning. The
        // retail reset clears its glyphs without allocating or changing flags.
        [[gnu::longcall]] void resetMessage(TextHandler*) AT(0x805CEDCC);
        resetMessage(handler);
    }
    return true;
}

bool HasFinishNoticeStarted(FinishMessageControl* control)
{
    if (!control)
        return false;
    const auto* race = mkw::RaceManager::Instance();
    if (control->hudSlot == 0 && race && static_cast<u32>(race->m_state) < 2)
        ClearNoticeCache();
    wchar_t notice[128];
    u32     generation;
    if (CopyRaceNotice(control->hudSlot, notice, generation))
        return true;
    // The native predicate waits for the local FINISHED flag. A spectator
    // deliberately stays unfinished until canonical completion, but its pane
    // must start displaying the notice now. This changes display state only.
    [[gnu::longcall]] bool nativeHasStarted(FinishMessageControl*) AT(0x807F8D08);
    return nativeHasStarted(control);
}

void UpdateFinishNotice(FinishMessageControl* control)
{
    if (!control)
        return;
    [[gnu::longcall]] void nativeUpdate(FinishMessageControl*) AT(0x8063DB00);
    nativeUpdate(control);
    if (control->vtable != reinterpret_cast<void*>(0x808D40C0) || !control->root ||
        !control->text || !control->common.loaded()) {
        if (control == s_lastNoticeControl)
            ClearNoticeCache();
        return;
    }
    // The race tick can publish a new camera target from another thread.
    // Copy its text and generation together before the synchronous glyph build;
    // no native text/SDK call runs inside the producer's interrupt guard.
    wchar_t notice[128];
    u32     generation = 0;
    auto*   race       = mkw::RaceManager::Instance();
    if (!CopyRaceNotice(control->hudSlot, notice, generation)) {
        if (control == s_lastNoticeControl && control->text == s_lastNoticePane &&
            race == s_lastNoticeRace && !RestoreNativeMessage(control))
            return;
        if (control->hudSlot == 0)
            ClearNoticeCache();
        return;
    }
    if (control != s_lastNoticeControl || control->text != s_lastNoticePane ||
        race != s_lastNoticeRace || generation != s_lastNoticeGeneration ||
        control->nativeMessageId != s_lastNativeMessageId) {
        TextHandler* handler = FindText(control);
        if (!handler)
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
        s_lastNoticePane       = control->text;
        s_lastNoticeRace       = race;
        s_lastNoticeGeneration = generation;
        s_lastNativeMessageId  = control->nativeMessageId;
    }
    // After the entire native Update, including its animation/root opacity
    // propagation, so the ordinary unfinished-state fade cannot hide us.
    // Leave the control's animation opacity intact; native Update reapplies it
    // when the spectator notice ends and the normal results display resumes.
    control->hidden      = false;
    control->root->alpha = 255;
    control->text->alpha = 255;
}

} // namespace RaceNotice

// CtrlRaceWifiFinishMessage::vtable + Update (0x10), PAL only.
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D40D0, RaceNotice::UpdateFinishNotice);
// CtrlRaceBase::UpdatePausePosition uses this predicate to activate the pane,
// independently of the opacity override above (vtable + HasStarted, 0x4C).
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D410C, RaceNotice::HasFinishNoticeStarted);

// Verified retail vtable + Init/+Update. Ordinary controls retain native code.
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D3D24, RaceNotice::InitLapControl);
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D3D28, RaceNotice::UpdateLapControl);

} // namespace wwfc::Knockout
#endif
