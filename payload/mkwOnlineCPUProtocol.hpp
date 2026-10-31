#pragma once

#include "wwfcBase.hpp"

namespace wwfc::OnlineCPU::Wire
{
constexpr u32 FRAME_SIZE  = 112;
constexpr u32 DATA_OFFSET = 48;

constexpr u16 Read16(const u8* p)
{
    return (static_cast<u16>(p[0]) << 8) | p[1];
}

constexpr u32 Read32(const u8* p)
{
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}

constexpr void Write16(u8* p, u16 v)
{
    p[0] = v >> 8;
    p[1] = v;
}

constexpr void Write32(u8* p, u32 v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

inline float ReadFloat(const u8* p)
{
    return __builtin_bit_cast(float, Read32(p));
}

inline void WriteFloat(u8* p, float value)
{
    Write32(p, __builtin_bit_cast(u32, value));
}

constexpr bool Newer(u32 value, u32 previous)
{
    return static_cast<s32>(value - previous) > 0;
}

// Bounded unsigned decimal parsing, including overflow and trailing bytes.
inline bool Number(const char*& input, u32& value, char separator)
{
    const char* p = input;
    if (*p < '0' || *p > '9')
        return false;
    u32 result = 0;
    for (u32 digits = 0; *p >= '0' && *p <= '9'; ++p, ++digits) {
        u32 digit = *p - '0';
        if (digits == 10 || result > (0xffffffffu - digit) / 10)
            return false;
        result = result * 10 + digit;
    }
    if (*p != separator)
        return false;
    input = separator ? p + 1 : p;
    value = result;
    return true;
}

inline bool ValidFrame(const u8* bytes, u32 epoch, u8 humans, u8 cpus)
{
    if (Read32(bytes) != epoch || bytes[8] < humans || bytes[8] >= humans + cpus || humans == 0 ||
        humans + cpus > 12 || Read16(bytes + 10) > 255 || (bytes[9] & ~3u) || !(bytes[9] & 1) ||
        bytes[39] || bytes[38] == 0 || bytes[38] > 12)
        return false;
    for (u32 offset = 12; offset <= 28; offset += 4) {
        float value = ReadFloat(bytes + offset);
        if (!(value >= -1000000.0f && value <= 1000000.0f))
            return false;
    }
    if ((bytes[9] & 2) && (!Read32(bytes + 44) || Read32(bytes + 44) >= 63u * 60000u))
        return false;
    return true;
}
} // namespace wwfc::OnlineCPU::Wire
