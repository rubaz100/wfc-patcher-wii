#include "mkwKnockdownMode.hpp"

#if RMC
#  include "wwfcLibC.hpp"

#  if WWFC_ONLINE_CPU_APPLY && WWFC_KNOCKOUT_APPLY && RMCPD00
#    include "import/dwc.h"
#    include "import/gamespy.h"
#  endif

namespace wwfc::Knockdown
{
#  if WWFC_ONLINE_CPU_APPLY && WWFC_KNOCKOUT_APPLY && RMCPD00
namespace
{
constinit bool s_serverAvailable = false;
constinit bool s_selected        = false;
} // namespace

bool ServerAvailable()
{
    return s_serverAvailable;
}

bool Selected()
{
    return s_selected;
}

void SetSelected(bool selected)
{
    s_selected = selected;
}

void ResetSession()
{
    s_serverAvailable = false;
    s_selected        = false;
}

void HandleServerMessage(const char* message)
{
    // Only the reserved authenticated self-buddy channel dispatches here.
    if (!std::strcmp(message, "LKD3:S:1"))
        s_serverAvailable = true;
    else if (!std::strcmp(message, "LKD3:S:0"))
        s_serverAvailable = false;
}

void HandleLoginResponse(const char* message)
{
    ResetSession();
    char value[8] = {};
    if (GameSpy::gpiValueForKey(message, "\\wl:ko_pool_ver\\", value, sizeof(value)) &&
        !std::strcmp(value, "3"))
        s_serverAvailable = true;
}

void Tick()
{
    if (!DWC::stpLoginCnt || DWC::stpLoginCnt->state != 5)
        ResetSession();
}
#  else
bool ServerAvailable()
{
    return false;
}

bool Selected()
{
    return false;
}

void SetSelected(bool)
{
}

void ResetSession()
{
}

void HandleServerMessage(const char*)
{
}

void HandleLoginResponse(const char*)
{
}

void Tick()
{
}
#  endif
} // namespace wwfc::Knockdown
#endif
