#pragma once

#include "wwfcBase.hpp"

namespace wwfc::Knockout
{

// ASCII text expanded to the game's two-byte wchar_t without libc formatting
// or allocations. Capacity includes the terminator, including failure paths.
template <typename Char, decltype(sizeof(0)) N>
bool FormatEliminationNotice(Char (&output)[N], u8 placement, bool spectating = true)
{
    static_assert(N > 0);
    output[0] = 0;
    if (placement < 2 || placement > 12) {
        return false;
    }
    decltype(sizeof(0)) used   = 0;
    auto                append = [&](const char* text) {
        while (*text) {
            if (used + 1 >= N) {
                output[used] = 0;
                return false;
            }
            output[used++] = static_cast<Char>(*text++);
        }
        output[used] = 0;
        return true;
    };
    char number[3] = {};
    if (placement >= 10) {
        number[0] = '1';
        number[1] = '0' + placement % 10;
    } else {
        number[0] = '0' + placement;
    }
    const char* suffix = placement == 2 ? "nd" : placement == 3 ? "rd" : "th";
    return append("Eliminated - ") && append(number) && append(suffix) && append(" place") &&
           (!spectating || append("\nNow spectating"));
}

} // namespace wwfc::Knockout
