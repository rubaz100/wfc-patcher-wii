#include "mkwInactivity.hpp"

#if RMC
#  include "import/dwc.h"
#  include "import/revolution.h"
#  include "wwfcLibC.hpp"

namespace wwfc::Inactivity
{
namespace
{
constinit bool                  s_disabled = false;
constinit DWC::DWCLoginContext* s_login    = nullptr;
} // namespace

void ResetSession()
{
    const bool interrupts = RVL::OSDisableInterrupts();
    s_disabled            = false;
    s_login               = nullptr;
    RVL::OSRestoreInterrupts(interrupts);
}

bool HandleServerMessage(const char* message)
{
    if (!message || std::strncmp(message, "LAFK1:", 6))
        return false;
    if (std::strcmp(message, "LAFK1:S:0") && std::strcmp(message, "LAFK1:S:1"))
        return true;
    const bool interrupts = RVL::OSDisableInterrupts();
    s_disabled            = message[8] == '1';
    s_login               = DWC::stpLoginCnt;
    RVL::OSRestoreInterrupts(interrupts);
    return true;
}

bool Enabled()
{
#  if RMCPD00
    const bool interrupts = RVL::OSDisableInterrupts();
    const bool disabled   = s_disabled;
    auto*      login      = s_login;
    RVL::OSRestoreInterrupts(interrupts);
    const u32 address = reinterpret_cast<u32>(login);
    const bool readable = !(address & 3) &&
                          ((address >= 0x80000000u && address <= 0x817FFFF8u) ||
                           (address >= 0x90000000u && address <= 0x93FFFFF8u));
    // The SDK context belongs to this negotiated login only. New/old servers
    // keep native behavior until they send an explicit authenticated policy.
    return disabled && readable && login == DWC::stpLoginCnt && login->state == 5;
#  else
    return false;
#  endif
}

} // namespace wwfc::Inactivity
#endif
