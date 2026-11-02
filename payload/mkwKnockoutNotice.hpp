#pragma once

#include "wwfcBase.hpp"

namespace wwfc::Knockout
{

template <typename Char, decltype(sizeof(0)) N>
bool FormatKnockdownRaceNotice(Char (&output)[N], u8 remaining, u8 total, u8 lap)
{
    static_assert(N > 0);
    output[0] = 0;
    if (total < 2 || total > 12 || remaining > total || lap > total)
        return false;
    decltype(sizeof(0)) used   = 0;
    auto                append = [&](const char* text) {
        while (*text) {
            if (used + 1 >= N)
                return false;
            output[used++] = static_cast<Char>(*text++);
        }
        output[used] = 0;
        return true;
    };
    auto number = [&](u8 value) {
        char digits[3] = {};
        if (value >= 10) {
            digits[0] = '1';
            digits[1] = '0' + value % 10;
        } else
            digits[0] = '0' + value;
        return append(digits);
    };
    return append("Knockdown - ") && number(remaining) && append(" / ") && number(total) &&
           append(" remain\nLap ") && number(lap ? lap : 1) && append("\nLast racer out each lap");
}

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

// The retail Wi-Fi finish pane supports three lines. The caller supplies a
// one-based display number (a CPU ordinal for CPUs), or zero when every
// eligible racer has finished and the canonical results are still pending.
// The placement-only reconnect notice deliberately uses the formatter above.
template <typename Char, decltype(sizeof(0)) N>
bool FormatSpectatorNotice(
    Char (&output)[N], u8 placement, u8 targetNumber, bool targetCPU, bool automatic
)
{
    if (!FormatEliminationNotice(output, placement, false))
        return false;
    if (targetNumber > 12) {
        output[0] = 0;
        return false;
    }
    decltype(sizeof(0)) used = 0;
    while (output[used])
        ++used;
    auto append = [&](const char* text) {
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
    if (!targetNumber)
        return append("\nWaiting for results");
    char number[3] = {};
    if (targetNumber >= 10) {
        number[0] = '1';
        number[1] = '0' + targetNumber % 10;
    } else {
        number[0] = '0' + targetNumber;
    }
    return append(targetCPU ? "\nWatching CPU " : "\nWatching racer ") && append(number) &&
           append(automatic ? " (Auto)" : " (Manual)") &&
           append("\nD-pad: Left/Right view, Down auto");
}

} // namespace wwfc::Knockout
