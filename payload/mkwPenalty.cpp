#include "mkwPenalty.hpp"

#if RMC
#  include "import/dwc.h"
#  include "import/mkw/system/System.hpp"
#  include "import/revolution.h"
#  include "wwfcGPReport.hpp"
#  include "wwfcLibC.hpp"

#  if RMCPD00
#    include "import/mkw/net/NetManager.hpp"
#  endif

namespace wwfc::Penalty
{
#  if RMCPD00
// PAL RKSYS::Mgr, confirmed against retail WriteAndRequestSaveCore/SaveCoreTask.
struct SaveManager {
    FILL(0x00000, 0x00014);
    void* raw;
    FILL(0x00018, 0x0001C);
    void* taskThread;
    FILL(0x00020, 0x00036);
    u16 currentLicense;
    FILL(0x00038, 0x25000);
    volatile u8 busy;
    FILL(0x25001, 0x25002);
    u8 loaded;
    FILL(0x25003, 0x25004);
    volatile s32 result;
};

static_assert(__builtin_offsetof(SaveManager, currentLicense) == 0x36);
static_assert(__builtin_offsetof(SaveManager, busy) == 0x25000);
static_assert(__builtin_offsetof(SaveManager, loaded) == 0x25002);
static_assert(__builtin_offsetof(SaveManager, result) == 0x25004);
static_assert(sizeof(SaveManager) == 0x25008);

extern SaveManager* s_saveManager AT(0x809BD748);

[[gnu::longcall]] void NativeSetPenalty(void*, u32, u8) AT(0x80546734);
[[gnu::longcall]] void NativeWriteLicense(SaveManager*, u32, const void*) AT(0x805467D0);
[[gnu::longcall]] void NativeSaveCoreTask(void*) AT(0x80544A5C);
[[gnu::longcall]] bool NativeRequestTask(void*, void (*)(void*), void*, u32) AT(0x80242C18);

#  endif

namespace
{
constexpr char PREFIX[]      = "LPR1:C:";
constexpr u32  RESULT_COUNT  = 8;
constexpr u64  DEFER_TIMEOUT = 30ull * 60750000ull; // Wii timebase: 60.75 MHz.

enum Status : u8 {
    PERSISTED   = 0,
    INVALID     = 1,
    SAVE_FAILED = 2,
    TIMED_OUT   = 3
};

struct Command {
    u32 nonce;
    u32 pid;
    u64 menuStarted;
};

struct Request {
    Command command;
    bool    occupied;
};

struct Result {
    u32    nonce;
    u32    pid;
    u32    replyPid;
    Status status;
    bool   needsAck;
};

constinit Request s_request               = {};
constinit Result  s_results[RESULT_COUNT] = {};
constinit u32     s_nextResult            = 0;

u64 Timebase()
{
    u32 hi, lo, again;
    do {
        asm volatile("mftbu %0; mftbl %1; mftbu %2" : "=r"(hi), "=r"(lo), "=r"(again));
    } while (hi != again);
    return (static_cast<u64>(hi) << 32) | lo;
}

bool InGameSpan(const void* pointer, u32 size)
{
    const u32 address = reinterpret_cast<u32>(pointer);
    return !(address & 3) && size &&
           ((address >= 0x80000000u && address < 0x81800000u && size <= 0x81800000u - address) ||
            (address >= 0x90000000u && address < 0x94000000u && size <= 0x94000000u - address));
}

u32 Self()
{
    if (!InGameSpan(DWC::stpLoginCnt, sizeof(DWC::DWCLoginContext)) || DWC::stpLoginCnt->state != 5)
        return 0;
    const auto* user = DWC::DWCi_GetUserData();
    return InGameSpan(user, sizeof(*user)) && user->profileId > 0
               ? static_cast<u32>(user->profileId)
               : 0;
}

bool ReadNumber(const char*& text, u32& value, char terminator)
{
    value     = 0;
    u8 digits = 0;
    while (*text >= '0' && *text <= '9') {
        if (++digits > 10)
            return false;
        const u32 digit = static_cast<u32>(*text++ - '0');
        if (value > (0xFFFFFFFFu - digit) / 10u)
            return false;
        value = value * 10 + digit;
    }
    if (!digits || !value || *text != terminator)
        return false;
    if (terminator)
        ++text;
    return true;
}

char* Decimal(char* destination, u32 value)
{
    char reverse[10];
    u8   count = 0;
    do {
        reverse[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        *destination++ = reverse[--count];
    return destination;
}

void FlushAcks()
{
    const u32 self = Self();
    if (!self)
        return;
    for (u32 index = 0; index < RESULT_COUNT; ++index) {
        const bool interrupts = RVL::OSDisableInterrupts();
        const auto result     = s_results[index];
        RVL::OSRestoreInterrupts(interrupts);
        if (!result.needsAck || result.replyPid != self)
            continue;
        char  message[16];
        char* end = Decimal(message, result.nonce);
        *end++    = ':';
        *end++    = static_cast<char>('0' + result.status);
        *end      = 0;
        if (!GPReport::Report("wl:penalty_ack", message))
            return;
        const bool locked = RVL::OSDisableInterrupts();
        auto&      saved  = s_results[index];
        if (saved.nonce == result.nonce && saved.pid == result.pid &&
            saved.status == result.status && saved.replyPid == result.replyPid)
            saved.needsAck = false;
        RVL::OSRestoreInterrupts(locked);
    }
}

void Complete(const Command& command, Status status, u32 replyPid)
{
    const bool interrupts   = RVL::OSDisableInterrupts();
    s_results[s_nextResult] = { command.nonce, command.pid, replyPid ? replyPid : command.pid,
                                status, true };
    s_nextResult            = (s_nextResult + 1) % RESULT_COUNT;
    if (s_request.occupied && s_request.command.nonce == command.nonce &&
        s_request.command.pid == command.pid)
        s_request.occupied = false;
    RVL::OSRestoreInterrupts(interrupts);
}

#  if RMCPD00

constexpr u32 LICENSE_BASE        = 0x38;
constexpr u32 LICENSE_SIZE        = 0x93F0;
constexpr u32 LICENSE_USER        = 0x8FB8;
constexpr u32 LICENSE_PENALTY     = 0x93EA;
constexpr u32 LICENSE_INITIALIZED = 0x93ED;
constexpr u32 NET_PENALTY         = 0x2760;

struct SaveJob {
    Command      command;
    SaveManager* manager;
    bool         queued;
};

constinit SaveJob s_job = {};

bool OutsideRace()
{
    const auto* scenes = mkw::System::Instance().getSceneManager();
    return InGameSpan(scenes, sizeof(*scenes)) &&
           scenes->getCurrentSceneID() != static_cast<s32>(mkw::Scene::ESceneID::RACE);
}

void FinishJob(Status status, u32 replyPid)
{
    const Command command = s_job.command;
    s_job                 = {};
    Complete(command, status, replyPid);
}

void Work(const Command& command)
{
    // Once accepted by the native worker, only its completion flag/result can
    // settle the operation. Do not time out/rollback over an in-flight NAND
    // write, even if the menu was left while that write finished.
    if (s_job.queued) {
        if (s_saveManager != s_job.manager || !InGameSpan(s_job.manager, sizeof(*s_job.manager))) {
            FinishJob(SAVE_FAILED, command.pid);
            return;
        }
        if (s_job.manager->busy)
            return;
        FinishJob(s_job.manager->result == 0 ? PERSISTED : SAVE_FAILED, command.pid);
        return;
    }

    if (Timebase() - command.menuStarted >= DEFER_TIMEOUT) {
        Complete(command, TIMED_OUT, Self());
        return;
    }
    if (!OutsideRace())
        return;

    const u32 self = Self();
    auto*     mgr  = s_saveManager;
    if (!self || self != command.pid || !InGameSpan(mgr, sizeof(*mgr)) ||
        mgr->currentLicense >= 4 || !mgr->loaded || !InGameSpan(mgr->raw, 0x28000) ||
        !InGameSpan(mgr->taskThread, 0x58)) {
        Complete(command, INVALID, self);
        return;
    }
    if (mgr->busy)
        return;
    const u32 licenseIndex = mgr->currentLicense;
    auto*     license = reinterpret_cast<u8*>(mgr) + LICENSE_BASE + licenseIndex * LICENSE_SIZE;
    if (!InGameSpan(license, LICENSE_SIZE) || !license[LICENSE_INITIALIZED]) {
        Complete(command, INVALID, self);
        return;
    }
    const auto* user = reinterpret_cast<const DWC::DWCUserData*>(license + LICENSE_USER);
    auto*       net  = mkw::NetManager::Instance();
    if (static_cast<u32>(user->profileId) != command.pid || !InGameSpan(net, sizeof(*net))) {
        Complete(command, INVALID, self);
        return;
    }

    const u8  previous[3] = { license[LICENSE_PENALTY], license[LICENSE_PENALTY + 1],
                              license[LICENSE_PENALTY + 2] };
    auto*     cached      = reinterpret_cast<u32*>(reinterpret_cast<u8*>(net) + NET_PENALTY);
    const u32 oldCached   = *cached;
    const u8  oldBusy     = mgr->busy;
    const s32 oldResult   = mgr->result;
    for (u32 index = 0; index < 3; ++index)
        NativeSetPenalty(license, index, 0);
    // Native WriteLicense modifies only this selected license's core image;
    // SaveCoreTask computes its CRC before writing through the normal NAND API.
    NativeWriteLicense(mgr, licenseIndex, license);
    *cached   = 0;
    mgr->busy = 1;
    if (!NativeRequestTask(mgr->taskThread, NativeSaveCoreTask, nullptr, 0)) {
        // No task owns these changes. Restore only our three counters and the
        // cached flag, and reserialize that same license before releasing busy.
        for (u32 index = 0; index < 3; ++index)
            NativeSetPenalty(license, index, previous[index]);
        NativeWriteLicense(mgr, licenseIndex, license);
        *cached     = oldCached;
        mgr->result = oldResult;
        mgr->busy   = oldBusy;
        Complete(command, SAVE_FAILED, self);
        return;
    }
    s_job = { command, mgr, true };
}
#  endif // RMCPD00

} // namespace

bool HandleServerMessage(const char* message)
{
    if (!message || std::strncmp(message, PREFIX, sizeof(PREFIX) - 1))
        return false;
    // At most two ten-digit numbers plus the exact separator/terminator.
    const char* cursor = message + sizeof(PREFIX) - 1;
    Command     command{};
    if (!ReadNumber(cursor, command.nonce, ':') || !ReadNumber(cursor, command.pid, '\0'))
        return true;
    const bool interrupts = RVL::OSDisableInterrupts();
    for (auto& result : s_results) {
        if (result.nonce == command.nonce && result.pid == command.pid) {
            result.needsAck = true;
            RVL::OSRestoreInterrupts(interrupts);
            return true;
        }
    }
    // The server serializes commands per PID and retries the same nonce. Do
    // not overwrite an executing command if another request arrives early.
    if (!s_request.occupied)
        s_request = { command, true };
    RVL::OSRestoreInterrupts(interrupts);
    return true;
}

void TickMenu()
{
    FlushAcks();
    const u64  now        = Timebase();
    const bool interrupts = RVL::OSDisableInterrupts();
    // A command received during a race stays queued until the player returns
    // to WFC. Only the first menu tick starts the native-busy wait deadline.
    if (s_request.occupied && !s_request.command.menuStarted)
        s_request.command.menuStarted = now;
    const auto request = s_request;
    RVL::OSRestoreInterrupts(interrupts);
    if (!request.occupied)
        return;
#  if RMCPD00
    Work(request.command);
#  else
    Complete(request.command, INVALID, Self());
#  endif
    FlushAcks();
}

} // namespace wwfc::Penalty
#endif // RMC
