#include "wwfcGPReport.hpp"
#include "import/dwc.h"
#include "import/gamespy.h"
#include "wwfcLibC.hpp"
#include "wwfcLog.hpp"

namespace wwfc::GPReport
{

// TODO: Games other than Mario Kart Wii
#if RMC

void Report(const char* key, const char* string)
{
    // stpMatchCnt is null once the DWC match context is torn down (race over,
    // disconnect, back to menu). Report() runs every frame via Process::Tick ->
    // Time::Tick while a finish-time report is pending, so a pending report that
    // outlives the match context would dereference a null stpMatchCnt here --
    // connection sits at offset 0, so the load faults reading address 0.
    auto matchCnt = DWC::stpMatchCnt;
    if (matchCnt == nullptr) {
        return;
    }

    auto connection = matchCnt->connection;
    if (connection == nullptr) {
        return;
    }

    auto iconnection = reinterpret_cast<GameSpy::GPIConnection*>(*connection);
    if (iconnection == nullptr) {
        // During teardown the GPConnection handle outlives its inner
        // GPIConnection: *connection goes null while connection itself does
        // not, so the guard above is not enough. Appending to
        // &iconnection->outputBuffer would hand the game a near-null pointer
        // (outputBuffer is at offset 0x210), faulting inside
        // gpiAppendStringToBuffer at 0x210/0x214/0x218.
        return;
    }

    GameSpy::gpiAppendStringToBuffer(connection, &iconnection->outputBuffer, "\\wl:report\\\\");
    GameSpy::gpiAppendStringToBuffer(connection, &iconnection->outputBuffer, key);
    GameSpy::gpiAppendStringToBuffer(connection, &iconnection->outputBuffer, "\\");
    GameSpy::gpiAppendStringToBuffer(connection, &iconnection->outputBuffer, string);
    GameSpy::gpiAppendStringToBuffer(connection, &iconnection->outputBuffer, "\\final\\");

    WWFC_LOG_INFO_FMT("REPORT '%s': %s", key, string);
}

void ReportU32(const char* key, u32 value)
{
    char buffer[sizeof("4294967295")];

    buffer[sizeof(buffer) - 1] = '\0';
    int n;
    for (n = sizeof(buffer) - 2; n >= 0 && value != 0; n--) {
        buffer[n] = '0' + (value % 10);
        value /= 10;
    }

    Report(key, buffer + n + 1);
}

void ReportB64Encode(const char* key, const void* data, std::size_t dataSize)
{
    char b64Data[0x400];

    s32 b64Size = DWC::DWC_Base64Encode(data, dataSize, b64Data, sizeof(b64Data));
    if (b64Size == -1 || b64Size == sizeof(b64Data)) {
        WWFC_LOG_ERROR("Could not fit the base64-encoded data into the provided buffer!");
        return;
    }
    b64Data[b64Size] = '\0';

    Report(key, b64Data);
}

#endif

} // namespace wwfc::GPReport