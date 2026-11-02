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
    // The finish-message control's text pane is "TextBox_00" (capitalised); the
    // lookup is case-sensitive, so the wrong case silently returned no handler
    // and the in-race notice never displayed.
    TextHandler* handler = findText(reinterpret_cast<u8*>(control) + 0xA8, "TextBox_00");
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

// --- Elimination warning: red, blinking position number --------------------
// PAL retail stores the transparent position_l_00 overlay at control+0x1A0.
// The visible rank is the separate Picture named "position". Select that exact
// pane through the same nonvirtual layout lookup used by native rank Init;
// never recolor the shadow/decorations or call a guessed pane virtual method.
namespace Placement
{

struct RankNumControl {
    void* vtable;
    FILL(0x004, 0x190);
    u8 hudSlot;
    FILL(0x191, 0x19C);
    u32   playerId;
    void* positionPane;
};

static_assert(__builtin_offsetof(RankNumControl, hudSlot) == 0x190);
static_assert(__builtin_offsetof(RankNumControl, playerId) == 0x19C);
static_assert(__builtin_offsetof(RankNumControl, positionPane) == 0x1A0);
static_assert(sizeof(RankNumControl) == 0x1A4);

constexpr u32 RANK_VTABLE        = 0x808D3E98;
constexpr u32 PICTURE_VTABLE     = 0x80273500;
constexpr u32 PICTURE_COLOURS    = 0xD8;
constexpr u32 PICTURE_COLOUR_END = 0xE8;
constexpr u32 PANE_NAME          = 0xBC;
constexpr u32 PANE_PARENT        = 0x0C;
constexpr u32 CONTROL_HIDDEN     = 0x80;
// At 60 HUD updates per second: half a second visible, half a second hidden.
constexpr u32 BLINK_HALF_PERIOD = 30;

[[gnu::longcall]] void  NativeRankInit(RankNumControl*) AT(0x807F48FC);
[[gnu::longcall]] void  NativeRankUpdate(RankNumControl*) AT(0x8063DB00);
[[gnu::longcall]] void* NativeFindPane(void*, const char*) AT(0x805E8368);

struct WarningState {
    RankNumControl* control;
    void*           anchor;
    void*           picture;
    u32             original[4];
    u32             applied[4];
    u32             frame;
    u8              nativeHidden;
    u8              appliedHidden;
    bool            owned;
};

constinit WarningState s_warning[4] = {};

bool InGameSpan(const void* pointer, u32 size)
{
    const u32 address = reinterpret_cast<u32>(pointer);
    return !(address & 3) && size &&
           ((address >= 0x80000000u && address < 0x81800000u && size <= 0x81800000u - address) ||
            (address >= 0x90000000u && address < 0x94000000u && size <= 0x94000000u - address));
}

bool IsRankControl(const RankNumControl* control)
{
    return InGameSpan(control, sizeof(*control)) &&
           control->vtable == reinterpret_cast<void*>(RANK_VTABLE);
}

bool IsPicture(const void* pointer, const char* name, u32 nameLength)
{
    return InGameSpan(pointer, PICTURE_COLOUR_END) &&
           *static_cast<const u32*>(pointer) == PICTURE_VTABLE &&
           !std::memcmp(static_cast<const u8*>(pointer) + PANE_NAME, name, nameLength);
}

bool IsRankPicture(const RankNumControl* control, const void* picture)
{
    constexpr char name[]       = "position";
    constexpr char anchorName[] = "position_l_00";
    if (!IsPicture(picture, name, sizeof(name)) ||
        !IsPicture(control->positionPane, anchorName, sizeof(anchorName)))
        return false;
    const void* parent =
        *reinterpret_cast<void* const*>(static_cast<const u8*>(picture) + PANE_PARENT);
    return InGameSpan(parent, 0xD8) &&
           parent == *reinterpret_cast<void* const*>(
                         static_cast<const u8*>(control->positionPane) + PANE_PARENT
                     );
}

void RestoreWarning(WarningState& state, RankNumControl* control)
{
    if (!state.owned || state.control != control)
        return;
    if (state.anchor == control->positionPane && IsRankPicture(control, state.picture)) {
        auto* colours = reinterpret_cast<u32*>(static_cast<u8*>(state.picture) + PICTURE_COLOURS);
        for (u8 i = 0; i < 4; ++i) {
            if (colours[i] == state.applied[i])
                colours[i] = state.original[i];
        }
    }
    u8& hidden = reinterpret_cast<u8*>(control)[CONTROL_HIDDEN];
    if (hidden == state.appliedHidden)
        hidden = state.nativeHidden;
    state.owned = false;
}

void InitRankNum(RankNumControl* control)
{
    if (!IsRankControl(control))
        return;
    // A new layout may reuse the prior object's addresses. Forget ownership
    // without dereferencing old panes; native Init establishes the new colors.
    if (control->hudSlot < 4)
        s_warning[control->hudSlot] = {};
    NativeRankInit(control);
}

void UpdateRankNum(RankNumControl* control)
{
    if (!IsRankControl(control))
        return;
    WarningState* state = control->hudSlot < 4 ? &s_warning[control->hudSlot] : nullptr;
    if (state && state->control == control)
        RestoreWarning(*state, control);
    // Restore our previous frame BEFORE native animation updates. Capture the
    // fresh native colors/visibility afterwards; never force white on exit.
    NativeRankUpdate(control);
    if (!state || !IsRankControl(control))
        return;

    // Spectator: once the local player is eliminated, their frozen placement is
    // meaningless while they watch other racers, so hide the position number
    // entirely (the race HUD has no text pane here to print "SPECTATOR"). Reuses
    // the same owned/restore bookkeeping as the warning blink.
    if (control->playerId < 12 && IsEliminated(static_cast<u8>(control->playerId))) {
        if (state->control != control) {
            *state         = {};
            state->control = control;
        }
        state->picture       = nullptr; // no colour change, only hide
        u8& hidden           = reinterpret_cast<u8*>(control)[CONTROL_HIDDEN];
        state->nativeHidden  = hidden;
        state->appliedHidden = 1;
        hidden               = 1;
        state->owned         = true;
        return;
    }

    if (control->playerId >= 12 || !IsEliminationCandidate(static_cast<u8>(control->playerId))) {
        *state = {};
        return;
    }
    if (state->control != control) {
        *state         = {};
        state->control = control;
    }
    state->anchor  = control->positionPane;
    state->picture = NativeFindPane(reinterpret_cast<u8*>(control) + 0xA8, "position");
    if (IsRankPicture(control, state->picture)) {
        auto* colours = reinterpret_cast<u32*>(static_cast<u8*>(state->picture) + PICTURE_COLOURS);
        for (u8 i = 0; i < 4; ++i) {
            state->original[i] = colours[i];
            // Big-endian RGBA: preserve each native vertex's alpha.
            state->applied[i] = 0xFF000000u | (colours[i] & 0xFFu);
            colours[i]        = state->applied[i];
        }
    } else {
        state->picture = nullptr;
    }
    u8& hidden           = reinterpret_cast<u8*>(control)[CONTROL_HIDDEN];
    state->nativeHidden  = hidden;
    const u8 blinkHidden = ((state->frame++ / BLINK_HALF_PERIOD) & 1) ? 1 : 0;
    state->appliedHidden = state->nativeHidden ? state->nativeHidden : blinkHidden;
    hidden               = state->appliedHidden;
    state->owned         = true;
}

} // namespace Placement

// CtrlRaceRankNum::vtable (0x808D3E98) + update (0x10) = 0x808D3EA8, PAL only.
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D3EA8, Placement::UpdateRankNum);
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D3EA4, Placement::InitRankNum);

} // namespace wwfc::Knockout
#endif
