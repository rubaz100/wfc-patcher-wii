// PAL native adapters for an agreed online CPU roster. CPU karts occupy only
// the committed unused tail of the grid; they never create network profiles.
#if RMC && WWFC_ONLINE_CPU_APPLY && RMCPD00

#  include "import/dwc.h"
#  include "import/mkw/net/NetManager.hpp"
#  include "import/mkw/system/RaceConfig.hpp"
#  include "import/mkw/system/RaceManager.hpp"
#  include "import/mkw/ui/page/MessagePopupPage.hpp"
#  include "import/mkw/ui/section/SectionManager.hpp"
#  include "mkwKnockdownMode.hpp"
#  include "mkwOnlineCPU.hpp"
#  include "wwfcPatch.hpp"

namespace wwfc::OnlineCPU
{
namespace Grid
{

using namespace mkw;

constinit void*             s_selection           = nullptr;
constinit void*             s_completionSelection = nullptr;
constinit mkw::UI::Section* s_completionSection   = nullptr;

[[gnu::longcall]] mkw::UI::Page* NativeTopLayer(mkw::UI::Section*) AT(0x80622EA0);

void ShowCompletedSeries(void* selection)
{
    if (!RaceKnockoutSeriesComplete()) {
        s_completionSelection = nullptr;
        s_completionSection   = nullptr;
        return;
    }
    auto* manager = mkw::UI::SectionManager::Instance();
    auto* section = manager ? manager->currentSection() : nullptr;
    if (!section || (selection == s_completionSelection && section == s_completionSection))
        return;
    auto* popup = section->getPage<mkw::UI::MessagePopupPage>(mkw::UI::EPageID::POPUP_MESSAGE);
    auto* page  = NativeTopLayer(section);
    if (!popup || !page || page == popup)
        return;
    static constexpr wchar_t message[] =
        L"Race Knockout is complete.\nThe host must close this room\nto finish the series.";
    mkw::UI::FormatParam format{ .strings = { message } };
    popup->reset();
    popup->setWindowMessage(0x19CA, &format);
    page->push(mkw::UI::EPageID::POPUP_MESSAGE, mkw::UI::Page::ENextType::FORWARD);
    s_completionSelection = selection;
    s_completionSection   = section;
}

bool IsVSSelection(const void* selection)
{
    const u32 mode = *static_cast<const u32*>(selection);
    return mode == 1 || mode == 3; // Native SELECT public/private VS modes.
}

void RejectGrid()
{
    GridRejected();
    auto* network = NetManager::Instance();
    if (!network)
        return;
    const u32 aids = network->availableAids();
    const u8  own  = network->myAid();
    for (u8 aid = 0; aid < 32; ++aid) {
        if (aid != own && (aids & (1u << aid))) {
            DWC::DWC_CloseConnectionHard(aid);
        }
    }
}

void InitScenario(RaceConfig* scenario, const RaceConfig* previous)
{
    scenario->initScenarioForOnlineCPU(previous);
    if (scenario->isWifiVSRace() && GridCanceled()) {
        RejectGrid();
        return;
    }
    const Plan* committed = CurrentPlan();
    // A dedicated-mode selection cannot silently allocate an ordinary grid
    // if its server capability or agreed KO plan disappeared while loading.
    if (scenario->isWifiVSRace() && Knockdown::Selected() &&
        (!Knockdown::ServerAvailable() || !committed || !committed->knockoutEnabled ||
         scenario->localPlayerCount() != 1)) {
        RejectGrid();
        return;
    }
    if (!committed || !scenario->isWifiVSRace())
        return;
    const Plan plan = *committed;
    if (plan.knockoutEnabled && !Knockdown::ServerAvailable()) {
        RejectGrid();
        return;
    }
    if (!plan.epoch || !plan.ownerProfileId || !plan.humanCount || plan.humanCount > MAX_PLAYERS ||
        plan.cpuCount > MAX_PLAYERS - plan.humanCount ||
        scenario->playerCount() != plan.humanCount) {
        RejectGrid();
        return;
    }
    u8 red = 0, blue = 0;
    for (u8 id = 0; id < plan.humanCount; ++id) {
        auto* player = scenario->getPlayer(id);
        if (player->m_type != RaceConfigPlayer::EPlayerType::MASTER &&
            player->m_type != RaceConfigPlayer::EPlayerType::REMOTE) {
            RejectGrid();
            return;
        }
        red += player->m_team == RaceConfigPlayer::ETeam::TEAM_RED;
        blue += player->m_team == RaceConfigPlayer::ETeam::TEAM_BLUE;
    }
    const u8 total = plan.humanCount + plan.cpuCount;
    for (u8 id = plan.humanCount; id < total; ++id) {
        if (scenario->getPlayer(id)->m_type != RaceConfigPlayer::EPlayerType::NONE) {
            RejectGrid();
            return;
        }
    }
    const bool owner = IsOwner();
    for (u8 id = plan.humanCount; id < total; ++id) {
        auto* player = scenario->getPlayer(id);
        player->m_type =
            owner ? RaceConfigPlayer::EPlayerType::CPU : RaceConfigPlayer::EPlayerType::REMOTE;
        player->m_character    = ECharacter::MARIO;
        player->m_vehicle      = EVehicle::M_STANDARD_KART;
        player->m_screenId     = 0xFF;
        player->m_inputIndex   = 0xFF;
        player->m_controllerId = -1;
        player->m_0xD4         = 8;
        player->m_prevScore    = 0;
        player->m_score        = 0;
        player->m_0xDC         = 0;
        player->m_gpRankScore  = 0;
        player->m_gpRank       = 0;
        player->m_startPos     = id + 1;
        player->m_finishPos    = id + 1;
        player->m_rating.set(5000);
        player->m_team = RaceConfigPlayer::ETeam::TEAM_NONE;
        if (scenario->teamsEnabled()) {
            if (red <= blue) {
                player->m_team = RaceConfigPlayer::ETeam::TEAM_RED;
                ++red;
            } else {
                player->m_team = RaceConfigPlayer::ETeam::TEAM_BLUE;
                ++blue;
            }
        }
    }
    if (!scenario->recomputeOnlineCPUPlayerCount(total)) {
        for (u8 id = plan.humanCount; id < total; ++id) {
            scenario->getPlayer(id)->m_type = RaceConfigPlayer::EPlayerType::NONE;
        }
        RejectGrid();
        return;
    }
    if (plan.cpuCount)
        scenario->configureOnlineCPUSettings(plan.seed);
    if (plan.knockoutEnabled)
        scenario->configureOnlineLapCount(plan.knockoutMode == 2 ? 3 : total);
    OnGridInitialized();
}

// Replay the exact displaced PAL instructions and continue the original leaf
// function. Its first comparison's CR0 and the caller's LR must both survive.
WWFC_DEFINE_CTR_STUB_SAVE_LR(
    0x80654830, bool NativeIsPlayerConnected(void*, u32), lis r3, 0x809C; lwz r3, -0x28D8(r3);
    lbz r0, 0x24(r3); cmplw r4, r0;
)

bool IsPlayerConnected(void* manager, u32 playerId)
{
    if (playerId < MAX_PLAYERS && IsCPUSlot(static_cast<u8>(playerId)))
        return OwnerAlive();
    return NativeIsPlayerConnected(manager, playerId);
}

WWFC_DEFINE_CTR_STUB(
    0x80533D94, void NativeSetPlayerDisconnected(RaceManager*, u32), stwu r1, -0x10(r1); mflr r0;
    stw r0, 0x14(r1); rlwinm r0, r4, 2, 22, 29;
)

void SetPlayerDisconnected(RaceManager* race, u32 playerId)
{
    // Native packet loops inline the missing-AID check, so changing only
    // IsPlayerConnected would still retire these karts on the next frame.
    if (playerId < MAX_PLAYERS && IsCPUSlot(static_cast<u8>(playerId)) && OwnerAlive())
        return;
    NativeSetPlayerDisconnected(race, playerId);
}

WWFC_DEFINE_CTR_STUB(
    0x8065FE9C, void* NativeCreateSelection(u32), stwu r1, -0x10(r1); mflr r0; lis r4, 0x809C;
    stw r0, 0x14(r1);
)

void* CreateSelection(u32 mode)
{
    void* selection = NativeCreateSelection(mode);
    if (selection && IsVSSelection(selection) && selection != s_selection) {
        s_selection = selection;
        BeginSelection();
    }
    return selection;
}

WWFC_DEFINE_CTR_STUB(
    0x8065FF70, void NativeDestroySelection(), stwu r1, -0x10(r1); mflr r0; stw r0, 0x14(r1);
    stw r31, 0x0C(r1);
)

void DestroySelection()
{
    NativeDestroySelection();
    const bool hadSelection = s_selection != nullptr;
    s_selection             = nullptr;
    // Transport retains an already committed grid until race initialization.
    if (hadSelection)
        EndSelection();
}

WWFC_DEFINE_CTR_STUB(
    0x8066030C, void NativeUpdateSelection(void*), stwu r1, -0x20(r1); mflr r0; stw r0, 0x24(r1);
    stw r31, 0x1C(r1);
)

void UpdateSelection(void* selection)
{
    if (!selection)
        return;
    if (!IsVSSelection(selection)) {
        NativeUpdateSelection(selection);
        return;
    }
    if (selection != s_selection) {
        s_selection = selection;
    }
    // The server capability message can arrive after SELECT is constructed.
    // BeginSelection is idempotent while waiting, so retry until advertised.
    BeginSelection();
    ShowCompletedSeries(selection);
    // GPCM/DWC processing and the payload Tick run independently of SELECT.
    // Hold its progression until every participant agrees the same roster.
    if (CanSelectAdvance())
        NativeUpdateSelection(selection);
}

} // namespace Grid

// clang-format off
WWFC_DEFINE_PATCH = Patch::CallWithCTR(
    WWFC_PATCH_LEVEL_FEATURE, 0x805302E0,
    ASM_LAMBDA(( : ASM_IMPORT_AS(i, Grid::InitScenario, InitScenario)),
               // The CTR call replaces the original BL and the next three words.
               // Preserve the continuation LR around the C++ adapter, then replay
               // the displaced count loads/setup before resuming at 0x805302F0.
               stwu r1, -0x10(r1);
               mflr r0; stw r0, 0x14(r1); bl %[InitScenario]; lwz r0, 0x14(r1); mtlr r0;
               addi r1, r1, 0x10; lbz r9, 0xC14(r31); addi r4, r31, 0x28; lbz r8, 0xC15(r31); blr;)
);
WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x80654820, Grid::IsPlayerConnected);
WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x80533D84, Grid::SetPlayerDisconnected);
WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8065FE8C, Grid::CreateSelection);
WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x8065FF60, Grid::DestroySelection);
WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x806602FC, Grid::UpdateSelection);

// clang-format on
} // namespace wwfc::OnlineCPU
#endif
