#include "wwfcGPReport.hpp"
#include "import/dwc.h"
#include "import/gamespy.h"
#include "import/revolution.h"
#include "wwfcLibC.hpp"
#include "wwfcLog.hpp"

namespace wwfc::GPReport
{

#if RMC
namespace
{
// Callbacks may run outside DWC_ProcessFriendsMatch. They only copy complete
// records into this queue; the GP buffer itself has one producer in Flush.
constexpr u32  QUEUE_SIZE      = 8192;
constexpr u32  CONTROL_RESERVE = 1024;
constexpr u32  MAX_FRAME       = 1152;
constexpr u32  OUTPUT_BUDGET   = 4096;
constexpr char PREFIX[]        = "\\wl:report\\\\";
constexpr char SUFFIX[]        = "\\final\\";
static_assert((QUEUE_SIZE & (QUEUE_SIZE - 1)) == 0);
static_assert(sizeof(PREFIX) - 1 + 63 + 1 + 1023 + sizeof(SUFFIX) <= MAX_FRAME);
static_assert(MAX_FRAME + 2 <= QUEUE_SIZE - CONTROL_RESERVE);
constinit u8                      s_queue[QUEUE_SIZE] = {};
constinit u32                     s_read = 0, s_write = 0, s_bytes = 0, s_generation = 0;
constinit GameSpy::GPIConnection* s_context = nullptr;
constinit GameSpy::GPConnection*  s_handle  = nullptr;
constinit bool s_flushing = false, s_overflowWarned = false, s_appendWarned = false;

struct Context {
    GameSpy::GPConnection*  handle;
    GameSpy::GPIConnection* inner;
};

Context CurrentContext()
{
    auto* match = DWC::stpMatchCnt;
    if (!match || !match->connection || !*match->connection)
        return {};
    return { match->connection, reinterpret_cast<GameSpy::GPIConnection*>(*match->connection) };
}

void BindContext(const Context& context)
{
    if (context.inner == s_context && context.handle == s_handle)
        return;
    s_context = context.inner;
    s_handle  = context.handle;
    s_read = s_write = s_bytes = 0;
    ++s_generation;
    s_overflowWarned = s_appendWarned = false;
}

void CopyIn(u32 offset, const void* source, u32 size)
{
    const u32 first = size < QUEUE_SIZE - offset ? size : QUEUE_SIZE - offset;
    std::memcpy(s_queue + offset, source, first);
    if (size > first)
        std::memcpy(s_queue, static_cast<const u8*>(source) + first, size - first);
}

void CopyOut(u32 offset, void* destination, u32 size)
{
    const u32 first = size < QUEUE_SIZE - offset ? size : QUEUE_SIZE - offset;
    std::memcpy(destination, s_queue + offset, first);
    if (size > first)
        std::memcpy(static_cast<u8*>(destination) + first, s_queue, size - first);
}

bool BoundedField(const char* value, u32 limit, u32& size)
{
    if (!value)
        return false;
    for (size = 0; size <= limit; ++size) {
        if (!value[size])
            return true;
        if (value[size] == '\\')
            return false;
    }
    return false;
}

bool Replaceable(const char* key)
{
    return !std::strcmp(key, "wl:cpu_frame") || !std::strcmp(key, "wl:cpu_item") ||
           !std::strcmp(key, "wl:race_live") || !std::strcmp(key, "wl:race_map");
}

} // namespace

bool Report(const char* key, const char* string)
{
    u32 keySize, valueSize;
    if (!BoundedField(key, 63, keySize) || !keySize || !BoundedField(string, 1023, valueSize))
        return false;
    char frame[MAX_FRAME];
    u32  size = 0;
    std::memcpy(frame + size, PREFIX, sizeof(PREFIX) - 1);
    size += sizeof(PREFIX) - 1;
    std::memcpy(frame + size, key, keySize);
    size += keySize;
    frame[size++] = '\\';
    std::memcpy(frame + size, string, valueSize);
    size += valueSize;
    std::memcpy(frame + size, SUFFIX, sizeof(SUFFIX));
    size += sizeof(SUFFIX); // includes the one local terminator, never sent
    if (size > sizeof(frame))
        return false;
    const bool    replaceable = Replaceable(key);
    const bool    interrupts  = RVL::OSDisableInterrupts();
    const Context context     = CurrentContext();
    BindContext(context);
    const u32 capacity = replaceable ? QUEUE_SIZE - CONTROL_RESERVE : QUEUE_SIZE;
    const u32 occupied = size + 2;
    if (!context.inner || occupied > capacity - (s_bytes < capacity ? s_bytes : capacity)) {
        const bool warn = context.inner && !s_overflowWarned;
        if (warn)
            s_overflowWarned = true;
        RVL::OSRestoreInterrupts(interrupts);
        if (warn)
            WWFC_LOG_WARN("Report queue full; record was not queued");
        return false;
    }
    const u8 header[2] = { static_cast<u8>(size >> 8), static_cast<u8>(size) };
    CopyIn(s_write, header, sizeof(header));
    CopyIn((s_write + 2) & (QUEUE_SIZE - 1), frame, size);
    s_write = (s_write + occupied) & (QUEUE_SIZE - 1);
    s_bytes += occupied;
    RVL::OSRestoreInterrupts(interrupts);
    if (!replaceable)
        WWFC_LOG_INFO_FMT("REPORT '%s': %s", key, string);
    return true;
}

u32 PendingBytes()
{
    const bool interrupts = RVL::OSDisableInterrupts();
    BindContext(CurrentContext());
    const u32 bytes = s_bytes;
    RVL::OSRestoreInterrupts(interrupts);
    return bytes;
}

void Flush()
{
    bool interrupts = RVL::OSDisableInterrupts();
    if (!interrupts || s_flushing) {
        RVL::OSRestoreInterrupts(interrupts);
        return;
    }
    s_flushing = true;
    RVL::OSRestoreInterrupts(interrupts);
    char frame[MAX_FRAME];
    for (u32 count = 0; count < 32; ++count) {
        interrupts            = RVL::OSDisableInterrupts();
        const Context context = CurrentContext();
        BindContext(context);
        if (!context.inner || s_bytes < 2) {
            RVL::OSRestoreInterrupts(interrupts);
            break;
        }
        const u32 read = s_read, generation = s_generation;
        const u32 size = (s_queue[read] << 8) | s_queue[(read + 1) & (QUEUE_SIZE - 1)];
        if (!size || size > sizeof(frame) || size + 2 > s_bytes) {
            // Internal corruption must never become a malformed GP command.
            s_read = s_write = s_bytes = 0;
            ++s_generation;
            RVL::OSRestoreInterrupts(interrupts);
            break;
        }
        const s32 output = context.inner->outputBuffer.length;
        if (output < 0 || static_cast<u32>(output) > OUTPUT_BUDGET ||
            size - 1 > OUTPUT_BUDGET - static_cast<u32>(output)) {
            RVL::OSRestoreInterrupts(interrupts);
            break;
        }
        CopyOut((read + 2) & (QUEUE_SIZE - 1), frame, size);
        RVL::OSRestoreInterrupts(interrupts);
        // No heap work, SDK callbacks or socket processing occur with IRQs off.
        // One append owns the whole record, including its final delimiter.
        const auto result =
            GameSpy::gpiAppendStringToBuffer(context.handle, &context.inner->outputBuffer, frame);
        interrupts = RVL::OSDisableInterrupts();
        if (result != GameSpy::GPNoError) {
            const bool warn = !s_appendWarned;
            s_appendWarned  = true;
            RVL::OSRestoreInterrupts(interrupts);
            if (warn)
                WWFC_LOG_WARN("GP append failed; queued reports remain pending");
            break;
        }
        if (s_generation == generation && s_context == context.inner &&
            s_handle == context.handle && s_read == read) {
            s_read = (s_read + size + 2) & (QUEUE_SIZE - 1);
            s_bytes -= size + 2;
        }
        RVL::OSRestoreInterrupts(interrupts);
    }
    interrupts = RVL::OSDisableInterrupts();
    s_flushing = false;
    RVL::OSRestoreInterrupts(interrupts);
}

bool ReportU32(const char* key, u32 value)
{
    char buffer[sizeof("4294967295")];
    buffer[sizeof(buffer) - 1] = '\0';
    int n                      = sizeof(buffer) - 2;
    do {
        buffer[n--] = '0' + value % 10;
        value /= 10;
    } while (n >= 0 && value);
    return Report(key, buffer + n + 1);
}

bool ReportB64Encode(const char* key, const void* data, std::size_t dataSize)
{
    char      b64Data[0x400];
    const s32 b64Size = DWC::DWC_Base64Encode(data, dataSize, b64Data, sizeof(b64Data));
    if (b64Size < 0 || b64Size >= static_cast<s32>(sizeof(b64Data))) {
        WWFC_LOG_ERROR("Could not fit the base64-encoded report into its buffer!");
        return false;
    }
    b64Data[b64Size] = '\0';
    return Report(key, b64Data);
}

#endif

} // namespace wwfc::GPReport
