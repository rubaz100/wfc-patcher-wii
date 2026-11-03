#include "mkwEliminatedVoting.hpp"

#if RMC
#  include "import/mkw/net/NetSelectHandler.hpp"
#  include "mkwOnlineCPU.hpp"
#  include "wwfcLibC.hpp"

#  if WWFC_ONLINE_CPU_APPLY
#    include "import/mkw/system/RaceConfig.hpp"
#    include "wwfcGPReport.hpp"
#    include "wwfcPatch.hpp"
#  endif

namespace wwfc::EliminatedVoting
{
namespace
{
constinit bool s_enabled = false;
} // namespace

#  if WWFC_ONLINE_CPU_APPLY
// PAL retail Random: the same constructor/limited draw/destructor used by
// NetSelectHandler::decideCourse (0x80661CE8). No virtual dispatch is needed.
class alignas(4) CourseRandom
{
public:
    CourseRandom()
    {
        [[gnu::longcall]] void construct(CourseRandom*) AT(0x80555464);
        construct(this);
    }

    ~CourseRandom()
    {
        [[gnu::longcall]] void destroy(CourseRandom*, s32) AT(0x80555538);
        destroy(this, -1); // Destroy the stack object without freeing it.
    }

    u32 next(u32 limit)
    {
        [[gnu::longcall]] u32 next(CourseRandom*, u32) AT(0x805555CC);
        return next(this, limit);
    }

private:
    u8 m_data[0x18];
};

static_assert(sizeof(CourseRandom) == 0x18);

static mkw::ECourse SingleConsoleRandomCourse(CourseRandom& random)
{
    // Native decideCourse returns early with only one connected console. A
    // lone eliminated human can still host surviving CPUs, so seed the copy
    // with a real random VS course before invoking that native path.
    u32 course = random.next(32);
    const auto* configs = mkw::RaceConfigManager::Instance();
    if (configs && configs->getConfig().isWifiVSRace()) {
        const u32 previous = static_cast<u32>(configs->getConfig().course());
        if (previous < 32 && course == previous) {
            course = random.next(31);
            if (course >= previous)
                ++course;
        }
    }
    return static_cast<mkw::ECourse>(course);
}

static bool ShouldNormalizeLocalVote(mkw::NetSelectHandler& selection)
{
    if (!s_enabled)
        return false;
    const auto& packet = selection.getSendPacket();
    if ((packet.phase != mkw::NetSelectHandler::Packet::EPhase::PREPARE &&
         packet.phase != mkw::NetSelectHandler::Packet::EPhase::VOTING) ||
        packet.selectedCourse != mkw::ECourse::CONTROL_VOTING_NOT_DECIDED)
        return false;
    const auto* network = mkw::NetManager::Instance();
    if (!network || network->myAid() >= 12)
        return false;
    const u32 self = 1u << network->myAid();
    return (OnlineCPU::EliminatedSeriesVotingAids(self, false) & self) != 0;
}

// Native leaf setter at 0x80660750. Replay its first four instructions, then
// continue at +0x10 to store course/star rank and return. It has no saved LR,
// so the SAVE_LR trampoline is required.
WWFC_DEFINE_CTR_STUB_SAVE_LR(
    0x80660760,
    void NativeSetPlayerDetails(mkw::NetSelectHandler*, u32, u32, u32, u32, u32),
    slwi r0, r7, 3; add r3, r3, r0; stb r4, 0x1C(r3); stb r5, 0x1D(r3);
)

void SetPlayerDetails(
    mkw::NetSelectHandler* selection, u32 character, u32 vehicle, u32 course, u32 local, u32 stars
)
{
    // Normalize at publication, before the vote card caches its first display.
    // Character, kart, star rank and the second local player's vote stay native.
    if (selection && local == 0 && course < 32 && ShouldNormalizeLocalVote(*selection))
        course = 0xFF;
    NativeSetPlayerDetails(selection, character, vehicle, course, local, stars);
}

WWFC_DEFINE_PATCH =
    Patch::BranchWithCTR(WWFC_PATCH_LEVEL_FEATURE, 0x80660750, SetPlayerDetails);
#  endif

void ResetSession()
{
    s_enabled = false;
}

bool HandleServerMessage(const char* message)
{
    if (!message || std::strncmp(message, "LEV1:", 5))
        return false;
    if (std::strcmp(message, "LEV1:S:0") && std::strcmp(message, "LEV1:S:1"))
        return true;
    s_enabled = message[7] == '1';
    return true;
}

bool Enabled()
{
    return s_enabled;
}

void NormalizeLocalVote(mkw::NetSelectHandler& selection)
{
#  if WWFC_ONLINE_CPU_APPLY
    auto& vote = selection.getSendPacket().player[0].courseVote;
    // A late-arriving roster can follow publication. Correct a concrete vote
    // before the next network send; never turn an unsubmitted 0x43 into a vote.
    if (static_cast<u32>(static_cast<mkw::ECourse>(vote)) < 32 &&
        ShouldNormalizeLocalVote(selection))
        vote = mkw::ECourse::CONTROL_VOTING_RANDOM;
#  endif
}

void DecideCourse(mkw::NetSelectHandler& selection)
{
#  if WWFC_ONLINE_CPU_APPLY
    auto* network = mkw::NetManager::Instance();
    const u32 connected = network ? network->availableAids() : 0;
    const u32 excluded = s_enabled ? OnlineCPU::EliminatedSeriesVotingAids(connected) : 0;
    // Do not alter unsupported/malformed rosters or a console's native path
    // outside an agreed upcoming Race Knockout selection.
    if (excluded && !(connected & ~0xFFFu) && network->myAid() < 12 &&
        (connected & (1u << network->myAid()))) {
        const u32 eligible = connected & ~excluded;
        const u32 candidates = eligible ? eligible : connected;
        const auto voteFor = [&](u8 aid) -> mkw::ECourse {
            return aid == network->myAid() ? selection.getSendPacket().player[0].courseVote
                                          : selection.getReceivedPacket(aid).player[0].courseVote;
        };
        // NOT_SELECTED is not a random vote. Never publish it as a course if
        // an incomplete or unexpected native vote reaches this decision point.
        for (u8 aid = 0; aid < 12; ++aid) {
            if (eligible & (1u << aid)) {
                const u32 vote = static_cast<u32>(voteFor(aid));
                if (vote >= 32 && vote != 0xFF) {
                    selection.decideCourse();
                    return;
                }
            }
        }

        CourseRandom random;
        u32 count = 0;
        for (u8 aid = 0; aid < 12; ++aid)
            if (candidates & (1u << aid))
                ++count;
        u32 index = random.next(count);
        u8 winner = 0;
        for (; winner < 12; ++winner) {
            if ((candidates & (1u << winner)) && index-- == 0)
                break;
        }
        const auto vote = eligible ? voteFor(winner) : mkw::ECourse::CONTROL_VOTING_RANDOM;

        // Retail decideCourse reads the connection mask twice and returns
        // early for one AID. Work on a bounded stack copy instead of changing
        // live connections or vote-received flags. Giving every copied voter
        // the chosen vote retains the game's own random-course resolution and
        // repeat avoidance, regardless of which copied AID its lottery picks.
        auto lottery = selection;
        lottery.getSendPacket().player[0].courseVote = vote;
        for (u8 aid = 0; aid < 12; ++aid)
            lottery.getReceivedPacket(aid).player[0].courseVote = vote;
        lottery.getSendPacket().winningVoterAid = winner;
        lottery.getSendPacket().selectedCourse =
            vote == mkw::ECourse::CONTROL_VOTING_RANDOM ? SingleConsoleRandomCourse(random) : vote;
        lottery.decideCourse();
        selection.getSendPacket().selectedCourse = lottery.getSendPacket().selectedCourse;
        selection.getSendPacket().winningVoterAid =
            eligible ? winner : lottery.getSendPacket().winningVoterAid;
        GPReport::ReportU32("wl:vote_excluded", excluded);
        GPReport::ReportU32("wl:vote_eligible", eligible);
        return;
    }
#  endif
    selection.decideCourse();
}

} // namespace wwfc::EliminatedVoting
#endif
