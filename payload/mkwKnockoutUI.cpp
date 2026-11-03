// PAL race notice through the actual finish-message control's virtual Update,
// not a guessed control pointer reached through menu/HUD offsets.
#if RMC && WWFC_KNOCKOUT_APPLY && RMCPD00

#  include "import/mkw/system/RaceConfig.hpp"
#  include "import/mkw/system/RaceManager.hpp"
#  include "import/mkw/ui/FormatParam.hpp"
#  include "mkwKnockout.hpp"
#  include "mkwKnockoutNotice.hpp"
#  include "mkwOnlineCPU.hpp"
#  include "mkwSpeedometer.hpp"
#  include "wwfcGPReport.hpp"
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
    FILL(0x00, 0x2C);
    float trans[3]; // 0x2C nw4r::lyt::Pane::trans (x, y, z)
    FILL(0x38, 0x44);
    float scale[2]; // 0x44 nw4r::lyt::Pane::scale (x, y)
    FILL(0x4C, 0xB8);
    u8 alpha;       // 0xB8
};

static_assert(__builtin_offsetof(TextPane, trans) == 0x2C);
static_assert(__builtin_offsetof(TextPane, scale) == 0x44);
static_assert(__builtin_offsetof(TextPane, alpha) == 0xB8);

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
    auto*       race    = mkw::RaceManager::Instance();
    auto*       configs = mkw::RaceConfigManager::Instance();
    const auto* plan    = OnlineCPU::CurrentPlan();
    if (slot == 0 && race && configs && plan && OnlineCPU::RaceKnockoutEnabled() &&
        OnlineCPU::KnockoutRoundCommitted() && OnlineCPU::RaceKnockoutSeriesComplete() &&
        static_cast<u32>(race->m_state) >= 3 && static_cast<u32>(race->m_state) <= 4 &&
        race->m_players &&
        configs->getConfig().playerCount() == plan->humanCount + plan->cpuCount) {
        u8 winner = 0xFF;
        for (u8 id = 0; id < plan->humanCount + plan->cpuCount; ++id) {
            const auto* player = race->m_players[id];
            if (!player || player->m_id != id)
                return false;
            if (!IsEliminated(id) &&
                !(player->m_stateFlags & mkw::RaceManager::Player::FLAG_DISCONNECTED)) {
                if (winner != 0xFF)
                    return false;
                winner = id;
            }
        }
        const bool cpu    = winner != 0xFF && OnlineCPU::IsCPUSlot(winner);
        const u8   number = winner == 0xFF ? 0 : cpu ? winner - plan->humanCount + 1 : winner + 1;
        generation        = 0x80000000u | (cpu ? 0x100u : 0) | number;
        return FormatSeriesCompleteNotice(output, number, cpu);
    }
    // While the local player is still racing, show no notice here: the
    // "racers remaining" message sat in the bottom-left corner on top of the
    // placement number. The remaining count still appears on the native/web
    // HUDs; this notice pane is reserved for the spectator notice, shown only
    // once the player is eliminated (the elimination-notice branch above).
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

// --- Speedometer: show the local kart's speed in a bottom corner ------------
// Ordinary (non-Knockout) online VS has no native speed readout. When no race
// notice is active, reuse this finish-message control to display the local
// kart's speed: translate and scale its root pane into a bottom corner and
// publish the text through the verified SetMessage path. Knockout races keep
// the pane for their notice (speed is gated off), so that HUD is untouched.
// Placement constants are starting values, calibrated from a live view.
constexpr float SPEED_TRANS_X = 0.0f;
constexpr float SPEED_TRANS_Y = 170.0f;
constexpr float SPEED_SCALE   = 0.55f;

constinit FinishMessageControl* s_speedControl    = nullptr;
constinit float                 s_speedTrans[3]   = {};
constinit float                 s_speedScale[2]   = {};
constinit bool                  s_speedOwned      = false;
constinit u32                   s_speedGeneration = 0;

// True when the speedometer may drive this control now: first local HUD slot,
// outside Knockout, with no native finish/results message pending.
bool SpeedEligible(FinishMessageControl* control)
{
    // Active racers in ordinary VS and in Knockdown/Knockout alike. In knockout
    // this control is only taken for the spectator notice once eliminated (via
    // CopyRaceNotice), so an active racer's pane is free for the speedometer.
    // First local HUD slot only, and never over a native finish/results message.
    return control && control->hudSlot == 0 && control->nativeMessageId == 0;
}

// Put the root pane back where native Update left it when we first took it.
void RestoreSpeedPane(FinishMessageControl* control)
{
    if (!s_speedOwned || s_speedControl != control)
        return;
    if (control->root) {
        control->root->trans[0] = s_speedTrans[0];
        control->root->trans[1] = s_speedTrans[1];
        control->root->trans[2] = s_speedTrans[2];
        control->root->scale[0] = s_speedScale[0];
        control->root->scale[1] = s_speedScale[1];
    }
    s_speedOwned   = false;
    s_speedControl = nullptr;
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
    wchar_t speed[128];
    u32     speedGeneration;
    if (SpeedEligible(control) && Speedometer::CurrentDisplay(speed, speedGeneration))
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
        if (control == s_speedControl) {
            s_speedOwned   = false;
            s_speedControl = nullptr;
        }
        return;
    }
    // The race tick can publish a new camera target from another thread.
    // Copy its text and generation together before the synchronous glyph build;
    // no native text/SDK call runs inside the producer's interrupt guard.
    wchar_t notice[128];
    u32     generation = 0;
    auto*   race       = mkw::RaceManager::Instance();
    // Diagnostic (wl:spd_dbg): ~1 Hz during a race, record how far the
    // speedometer path gets on this control, so a no-show localises without a
    // blind rebuild. 1 hook ran | 2 slot 0 | 4 no native msg | 8 speed ready |
    // 16 text handler found. A healthy active racer reports 31.
    {
        static constinit u32 s_speedDebugFrame = 0;
        if (race && race->m_state == mkw::RaceManager::EState::RACE &&
            (s_speedDebugFrame++ % 30) == 0) {
            wchar_t dbg[128];
            u32     dbgGen = 0;
            u32     bits   = 1;
            if (control->hudSlot == 0)
                bits |= 2;
            if (control->nativeMessageId == 0)
                bits |= 4;
            if (Speedometer::CurrentDisplay(dbg, dbgGen))
                bits |= 8;
            if (FindText(control))
                bits |= 16;
            GPReport::ReportU32("wl:spd_dbg", bits);
        }
    }
    if (!CopyRaceNotice(control->hudSlot, notice, generation)) {
        // No race notice. In ordinary (non-Knockout) online VS, show the local
        // kart's speed in a bottom corner using this same control's text path.
        wchar_t speed[128];
        u32     speedGeneration = 0;
        if (SpeedEligible(control) && Speedometer::CurrentDisplay(speed, speedGeneration)) {
            TextHandler* handler = FindText(control);
            if (!handler) {
                RestoreSpeedPane(control);
                return;
            }
            if (!s_speedOwned || s_speedControl != control) {
                // Take ownership: remember the native placement so it can be
                // restored when a real message or the next race needs the pane.
                s_speedControl  = control;
                s_speedTrans[0] = control->root->trans[0];
                s_speedTrans[1] = control->root->trans[1];
                s_speedTrans[2] = control->root->trans[2];
                s_speedScale[0] = control->root->scale[0];
                s_speedScale[1] = control->root->scale[1];
                s_speedOwned      = true;
                s_speedGeneration = ~speedGeneration; // force a publish now
            }
            if (speedGeneration != s_speedGeneration) {
                BmgView                menu = {};
                [[gnu::longcall]] void load(BmgView*, const char*) AT(0x805F8B90);
                load(&menu, "Menu");
                if (!menu.loaded())
                    return;
                mkw::UI::FormatParam   format{ .strings = { speed } };
                [[gnu::longcall]] void setMessage(
                    TextHandler*, const BmgView&, const BmgView&, u32,
                    const mkw::UI::FormatParam*
                ) AT(0x805CDD00);
                setMessage(handler, menu, control->common, 0x19CA, &format);
                s_speedGeneration = speedGeneration;
            }
            control->root->trans[0] = SPEED_TRANS_X;
            control->root->trans[1] = SPEED_TRANS_Y;
            control->root->scale[0] = SPEED_SCALE;
            control->root->scale[1] = SPEED_SCALE;
            control->hidden      = false;
            control->root->alpha = 255;
            control->text->alpha = 255;
            return;
        }
        RestoreSpeedPane(control);
        if (control == s_lastNoticeControl && control->text == s_lastNoticePane &&
            race == s_lastNoticeRace && !RestoreNativeMessage(control))
            return;
        if (control->hudSlot == 0)
            ClearNoticeCache();
        return;
    }
    RestoreSpeedPane(control);
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

// --- Elimination warning: red-to-amber position number ----------------------
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
// At 60 HUD updates per second: one second red -> amber, one second back.
constexpr u32 GRADIENT_HALF_PERIOD = 60;
constexpr u32 AMBER_GREEN          = 0xBF;

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
    bool            visibilityOwned;
};

constinit WarningState s_warning[4] = {};

u32 WarningTint(u32 frame)
{
    const u32 phase = frame % (2 * GRADIENT_HALF_PERIOD);
    const u32 step  = phase <= GRADIENT_HALF_PERIOD ? phase : 2 * GRADIENT_HALF_PERIOD - phase;
    // Integer smoothstep has zero slope at red/amber, so the pulse reverses
    // gently. Its largest numerator fits u32; alpha is preserved per vertex.
    const u32 green = AMBER_GREEN * step * step * (3 * GRADIENT_HALF_PERIOD - 2 * step) /
                      (GRADIENT_HALF_PERIOD * GRADIENT_HALF_PERIOD * GRADIENT_HALF_PERIOD);
    return 0xFF000000u | (green << 16);
}

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
    if (state.visibilityOwned && hidden == state.appliedHidden)
        hidden = state.nativeHidden;
    state.owned           = false;
    state.visibilityOwned = false;
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

    // Use the same local-spectator/notice gate as the finish-message HUD. An
    // elimination record alone can outlive spectating into results, and the
    // watched racer's position must not decide whether this local HUD hides.
    wchar_t notice[128];
    u32     noticeGeneration;
    if (RaceNotice::CopyRaceNotice(control->hudSlot, notice, noticeGeneration)) {
        if (state->control != control) {
            *state         = {};
            state->control = control;
        }
        state->anchor          = control->positionPane;
        state->picture         = nullptr; // visibility only; keep native colors
        u8& hidden             = reinterpret_cast<u8*>(control)[CONTROL_HIDDEN];
        state->nativeHidden    = hidden;
        state->appliedHidden   = 1;
        hidden                 = 1;
        state->owned           = true;
        state->visibilityOwned = true;
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
    const u32 tint = WarningTint(state->frame);
    state->frame   = (state->frame + 1) % (2 * GRADIENT_HALF_PERIOD);
    if (IsRankPicture(control, state->picture)) {
        auto* colours = reinterpret_cast<u32*>(static_cast<u8*>(state->picture) + PICTURE_COLOURS);
        for (u8 i = 0; i < 4; ++i) {
            state->original[i] = colours[i];
            // Big-endian RGBA: preserve each native vertex's alpha.
            state->applied[i] = tint | (colours[i] & 0xFFu);
            colours[i]        = state->applied[i];
        }
    } else {
        state->picture = nullptr;
    }
    // Warning animation changes RGB only. Native visibility/alpha remain
    // authoritative; only the spectator branch above hides this control.
    state->visibilityOwned = false;
    state->owned           = true;
}

} // namespace Placement

// CtrlRaceRankNum::vtable (0x808D3E98) + update (0x10) = 0x808D3EA8, PAL only.
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D3EA8, Placement::UpdateRankNum);
WWFC_DEFINE_PATCH =
    Patch::WritePointer(WWFC_PATCH_LEVEL_FEATURE, 0x808D3EA4, Placement::InitRankNum);

} // namespace wwfc::Knockout
#endif
