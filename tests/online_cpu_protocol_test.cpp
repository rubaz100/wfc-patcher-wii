#include "mkwOnlineCPUProtocol.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace wwfc;
using namespace wwfc::OnlineCPU;

int main()
{
    u8 frame[Wire::FRAME_SIZE] = {};
    Wire::Write32(frame, 0x01020304);
    assert(frame[0] == 1 && frame[1] == 2 && frame[2] == 3 && frame[3] == 4);
    assert(Wire::Read32(frame) == 0x01020304);
    Wire::Write16(frame + 10, 3);
    assert(Wire::Read16(frame + 10) == 3);
    frame[8]  = 2;
    frame[9]  = 1;
    frame[38] = 3;
    for (u32 i = 12; i <= 28; i += 4)
        Wire::WriteFloat(frame + i, 2.5f);
    assert(Wire::ReadFloat(frame + 12) == 2.5f);
    assert(Wire::ValidFrame(frame, 0x01020304, 2, 10));
    assert(!Wire::ValidFrame(frame, 99, 2, 10));
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 0));
    Wire::Write16(frame + 10, 256);
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    Wire::Write16(frame + 10, 3);
    frame[8] = 11;
    assert(Wire::ValidFrame(frame, 0x01020304, 2, 10));
    frame[8] = 12;
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    frame[8] = 1;
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    frame[8]  = 2;
    frame[39] = 1;
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    frame[39] = 0;
    frame[38] = 0;
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    frame[38] = 3;
    frame[9]  = 0;
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    frame[9] = 7;
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    frame[9] = 3;
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    Wire::Write32(frame + 44, 63 * 60000 - 1);
    assert(Wire::ValidFrame(frame, 0x01020304, 2, 10));
    Wire::Write32(frame + 44, 63 * 60000);
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    frame[9] = 1;
    Wire::Write32(frame + 12, 0x7fc00000); // NaN
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    Wire::Write32(frame + 12, 0x7f800000); // infinity
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));
    Wire::WriteFloat(frame + 12, 1000001.0f);
    assert(!Wire::ValidFrame(frame, 0x01020304, 2, 10));

    const char* text   = "4294967295:7";
    u32         number = 0;
    assert(Wire::Number(text, number, ':') && number == 0xffffffffu);
    assert(Wire::Number(text, number, '\0') && number == 7);
    for (const char* invalid : { "4294967296", "12345678901", "-1", "1x", "", ":" }) {
        const char* p = invalid;
        assert(!Wire::Number(p, number, '\0'));
    }
    assert(Wire::Newer(2, 1));
    assert(!Wire::Newer(2, 2));
    assert(!Wire::Newer(1, 2));
    assert(Wire::Newer(1, 0xffffffffu));
    assert(!Wire::Newer(0xffffffffu, 1));
    puts("Online CPU wire regression cases passed");
}
