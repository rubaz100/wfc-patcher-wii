#include "mkwEliminatedVoting.hpp"

#if RMC
#  include "wwfcLibC.hpp"

namespace wwfc::EliminatedVoting
{
namespace
{
constinit bool s_enabled = false;
} // namespace

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

} // namespace wwfc::EliminatedVoting
#endif
