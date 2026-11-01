#include "mkwRaceTelemetry.hpp"

#if RMC && WWFC_ONLINE_CPU_APPLY && RMCPD00

#  include "import/dwc.h"
#  include "import/gamespy.h"
#  include "import/mkw/net/NetManager.hpp"
#  include "import/mkw/system/RaceConfig.hpp"
#  include "import/mkw/system/RaceManager.hpp"
#  include "import/mkw/system/System.hpp"
#  include "mkwKnockout.hpp"
#  include "mkwOnlineCPU.hpp"
#  include "mkwOnlineCPUProtocol.hpp"
#  include "wwfcGPReport.hpp"
#  include "wwfcLibC.hpp"

namespace wwfc::RaceTelemetry
{
namespace
{
using OnlineCPU::Wire::Write16;
using OnlineCPU::Wire::Write32;
using OnlineCPU::Wire::WriteFloat;

constexpr u32 LIVE_HEADER        = 24;
constexpr u32 RACER_SIZE         = 28;
constexpr u32 MAX_PLAYERS        = 12;
constexpr u16 MAX_POINTS         = 256;
constexpr u16 MAX_SEGMENTS       = 512;
constexpr u16 POINTS_PER_CHUNK   = 24;
constexpr u16 SEGMENTS_PER_CHUNK = 48;

struct Point {
    float x, z;
};

struct Segment {
    u16 from, to;
};

constinit mkw::RaceManager* s_race                      = nullptr;
constinit u32               s_raceId                    = 0;
constinit u32               s_sequence                  = 0;
constinit u32               s_lastSend                  = 0;
constinit u32               s_lastFrame                 = 0;
constinit u32               s_seed                      = 0;
constinit u8                s_course                    = 0xFF;
constinit u8                s_phase                     = 0;
constinit bool              s_hasSent                   = false;
constinit u32               s_profileIds[MAX_PLAYERS]   = {};
constinit u8                s_localIndices[MAX_PLAYERS] = {};
constinit Point             s_points[MAX_POINTS]        = {};
constinit Segment           s_segments[MAX_SEGMENTS]    = {};
constinit u16               s_pointCount                = 0;
constinit u16               s_segmentCount              = 0;
constinit u16               s_mapOffset                 = 0;
constinit u8                s_mapKind                   = 1;
constinit bool              s_mapReady                  = false;
constinit bool              s_mapCycleDone              = false;
constinit u32               s_mapCycleTime              = 0;

template <class T>
T Load(const void* object, u32 offset)
{
    T value;
    std::memcpy(&value, static_cast<const u8*>(object) + offset, sizeof(value));
    return value;
}

bool Readable(const void* pointer, u32 bytes)
{
    const u32 address = reinterpret_cast<u32>(pointer);
    if ((address & 3) || bytes > 0x04000000u)
        return false;
    return (address >= 0x80000000u && address <= 0x81800000u - bytes) ||
           (address >= 0x90000000u && address <= 0x94000000u - bytes);
}

bool Coordinate(float value)
{
    return value >= -1000000.0f && value <= 1000000.0f;
}

u32 Ticks()
{
    u32 value;
    asm volatile("mftb %0" : "=r"(value));
    return value;
}

u32 HalfSecond()
{
    // Wii low-memory bus clock is Hz. OS timebase runs at bus clock / 4.
    return *reinterpret_cast<volatile const u32*>(0x800000F8) / 8;
}

bool Budget(u32 bytes)
{
    const auto* match = DWC::stpMatchCnt;
    if (!match || !match->connection || !*match->connection)
        return false;
    const auto* connection = reinterpret_cast<const GameSpy::GPIConnection*>(*match->connection);
    const s32   length     = connection->outputBuffer.length;
    const u32   added      = ((bytes + 2) / 3) * 4 + 64;
    const u32   pending    = GPReport::PendingBytes();
    return length >= 0 && added < 4096 && pending <= 4096 - added &&
           static_cast<u32>(length) <= 4096 - added - pending;
}

u32 Milliseconds(const mkw::Time* time)
{
    if (!Readable(time, sizeof(*time)) || !time->m_valid || time->m_minutes >= 63 ||
        time->m_seconds >= 60 || time->m_milliseconds >= 1000)
        return 0;
    return time->m_minutes * 60000u + time->m_seconds * 1000u + time->m_milliseconds;
}

void NewRace(mkw::RaceManager* race, u8 course, u32 seed, u32 now)
{
    // Seed once per payload lifetime, then advance monotonically even if the
    // native RaceManager allocation or a track is reused for the next race.
    if (!s_raceId)
        s_raceId = now ? now : 1;
    ++s_raceId;
    if (!s_raceId)
        ++s_raceId;
    s_race         = race;
    s_course       = course;
    s_seed         = seed;
    s_sequence     = 0;
    s_lastSend     = now;
    s_hasSent      = false;
    s_mapReady     = false;
    s_mapCycleDone = false;
    s_mapKind      = 1;
    s_mapOffset    = 0;
    s_pointCount = s_segmentCount = 0;
    std::memset(s_profileIds, 0, sizeof(s_profileIds));
    std::memset(s_localIndices, 0, sizeof(s_localIndices));
}

bool Reporter(const mkw::NetManager& network)
{
    const auto* plan = OnlineCPU::CurrentPlan();
    if (plan && plan->cpuCount && OnlineCPU::IsCPUSlot(plan->humanCount))
        return OnlineCPU::IsOwner() && OnlineCPU::OwnerAlive();
    return network.amITheServer();
}

bool Identity(u8 id, const mkw::NetManager& network, bool cpu, u32& profile, u8& local)
{
    profile = 0;
    local   = 0;
    if (cpu)
        return true;
    // Native global-racer-to-AID map; two local humans share the same AID.
    const auto* aids = reinterpret_cast<const u8*>(&network) + 0x2920;
    const u8    aid  = aids[id];
    if (s_profileIds[id]) {
        profile = s_profileIds[id];
        local   = s_localIndices[id];
        return true;
    }
    if (aid >= MAX_PLAYERS)
        return false;
    if (aid == network.myAid()) {
        const auto* user = DWC::DWCi_GetUserData();
        if (user)
            profile = user->profileId;
    } else if (network.availableAids() & (1u << aid)) {
        const auto* node = DWC::DWCi_NodeInfoList_GetNodeInfoForAid(aid);
        if (node)
            profile = node->profileId;
    }
    if (!profile)
        return false;
    for (u8 previous = 0; previous < id; ++previous)
        if (aids[previous] == aid)
            ++local;
    if (local > 1)
        return false;
    s_profileIds[id]   = profile;
    s_localIndices[id] = local;
    return true;
}

bool Position(u8 id, u8 count, Point& point)
{
    extern void* telemetryKartManager AT(0x809C18F8);
    const void*                       manager = telemetryKartManager;
    if (!Readable(manager, 0x28) || Load<u8>(manager, 0x24) < count)
        return false;
    const auto* players = Load<const void* const*>(manager, 0x20);
    if (!Readable(players, count * 4) || !Readable(players[id], 4))
        return false;
    // Verified Kart::Link::GetPosition (8059020C) chain, through Physics.
    const void* pointers = Load<const void*>(players[id], 0);
    if (!Readable(pointers, 0x0C))
        return false;
    const void* body = Load<const void*>(pointers, 8);
    if (!Readable(body, 0x94))
        return false;
    const void* holder = Load<const void*>(body, 0x90);
    if (!Readable(holder, 8))
        return false;
    const void* physics = Load<const void*>(holder, 4);
    if (!Readable(physics, 0x74))
        return false;
    point = { Load<float>(physics, 0x68), Load<float>(physics, 0x70) };
    return Coordinate(point.x) && Coordinate(point.z);
}

bool CaptureMap()
{
    // KMP::Manager singleton verified in CreateInstance 80512694. ENPT uses
    // its native graph links, not array order; branches must remain separate.
    extern void* telemetryCourseMap AT(0x809BD6E8);
    const void*                     manager = telemetryCourseMap;
    if (!Readable(manager, 0x14))
        return false;
    const void* section = Load<const void*>(manager, 0x10);
    if (!Readable(section, 0x0C))
        return false;
    const u16 count = Load<u16>(section, 4);
    if (count < 2 || count > MAX_POINTS)
        return false;
    const auto* holders = Load<const void* const*>(section, 0);
    if (!Readable(holders, count * 4))
        return false;
    u16 segments = 0;
    for (u16 id = 0; id < count; ++id) {
        const void* holder = holders[id];
        if (!Readable(holder, 0x18))
            return false;
        const void* raw = Load<const void*>(holder, 4);
        if (!Readable(raw, 0x14))
            return false;
        const Point point = { Load<float>(raw, 0), Load<float>(raw, 8) };
        if (!Coordinate(point.x) || !Coordinate(point.z))
            return false;
        s_points[id]          = point;
        const u8    nextCount = Load<u8>(holder, 0x11);
        const auto* next      = Load<const u8*>(holder, 0x0C);
        // Stock path groups have at most six next links. An absent terminal
        // edge is fine; malformed or oversized geometry uses marker-only UI.
        if (nextCount > 6 || (nextCount && !Readable(next, nextCount)))
            return false;
        for (u8 link = 0; link < nextCount; ++link) {
            const u8 target = next[link];
            if (target == 0xFF || target == id)
                continue;
            if (target >= count || segments == MAX_SEGMENTS)
                return false;
            s_segments[segments++] = { id, target };
        }
    }
    s_pointCount   = count;
    s_segmentCount = segments;
    return true;
}

void SendMap(u32 now, u32 halfSecond)
{
    if (!s_mapReady)
        return;
    if (s_mapCycleDone) {
        if (now - s_mapCycleTime < halfSecond * 60u)
            return;
        s_mapCycleDone = false;
        s_mapKind      = 1;
        s_mapOffset    = 0;
    }
    u8        bytes[16 + POINTS_PER_CHUNK * sizeof(Point)] = {};
    const u16 total     = s_mapKind == 1 ? s_pointCount : s_segmentCount;
    const u16 capacity  = s_mapKind == 1 ? POINTS_PER_CHUNK : SEGMENTS_PER_CHUNK;
    const u16 remaining = total - s_mapOffset;
    const u16 count     = remaining < capacity ? remaining : capacity;
    const u32 length    = 16 + count * (s_mapKind == 1 ? 8 : 4);
    if (!Budget(length))
        return;
    bytes[0] = 1;
    bytes[1] = s_mapKind;
    bytes[2] = s_course;
    Write32(bytes + 4, s_raceId);
    Write16(bytes + 8, total);
    Write16(bytes + 10, s_mapOffset);
    Write16(bytes + 12, count);
    for (u16 i = 0; i < count; ++i) {
        if (s_mapKind == 1) {
            const auto& point = s_points[s_mapOffset + i];
            WriteFloat(bytes + 16 + i * 8, point.x);
            WriteFloat(bytes + 20 + i * 8, point.z);
        } else {
            const auto& segment = s_segments[s_mapOffset + i];
            Write16(bytes + 16 + i * 4, segment.from);
            Write16(bytes + 18 + i * 4, segment.to);
        }
    }
    if (!GPReport::ReportB64Encode("wl:race_map", bytes, length))
        return;
    s_mapOffset += count;
    if (s_mapOffset == total) {
        s_mapOffset = 0;
        if (s_mapKind == 1) {
            s_mapKind = 2;
        } else {
            s_mapCycleDone = true;
            s_mapCycleTime = now;
        }
    }
}

} // namespace

void Tick()
{
    auto& system        = mkw::System::Instance();
    auto* configManager = mkw::RaceConfigManager::Instance();
    auto* network       = mkw::NetManager::Instance();
    auto* race          = mkw::RaceManager::Instance();
    if (!system.getSceneManager() || !system.isCurrentSceneID(mkw::Scene::ESceneID::RACE) ||
        !configManager || !network || !Readable(race, 0x2C) ||
        !configManager->getConfig().isWifiVSRace()) {
        s_race = nullptr;
        return;
    }
    if (!Reporter(*network))
        return;
    const auto& config = configManager->getConfig();
    const u8    count  = config.playerCount();
    const u32   course = Load<u32>(&config, 0xB48);
    const u8    laps   = Load<u8>(&config, 0xB6D);
    const u32   seed   = Load<u32>(&config, 0xB74);
    const u32   stage  = static_cast<u32>(race->m_state);
    if (!count || count > MAX_PLAYERS || course >= 32 || !laps ||
        !Readable(race->m_players, count * 4) || stage > 4)
        return;
    const u8  phase      = stage < 3 ? stage : 3;
    const u32 frame      = Load<u32>(race, 0x20);
    const u32 now        = Ticks();
    const u32 halfSecond = HalfSecond();
    if (!halfSecond)
        return;
    if (s_race != race || s_course != course || s_seed != seed ||
        (phase < 2 && (s_phase >= 2 || frame < s_lastFrame)))
        NewRace(race, static_cast<u8>(course), seed, now);
    s_lastFrame = frame;
    s_phase     = phase;
    if (s_hasSent && now - s_lastSend < halfSecond)
        return;
    const u32 length = LIVE_HEADER + count * RACER_SIZE;
    if (!Budget(length))
        return;
    if (!s_mapReady)
        s_mapReady = CaptureMap();
    u8 bytes[LIVE_HEADER + MAX_PLAYERS * RACER_SIZE] = {};
    bytes[0]                                         = 1;
    bytes[1]                                         = phase;
    bytes[2]                                         = count;
    bytes[3]                                         = static_cast<u8>(course);
    Write32(bytes + 4, s_raceId);
    Write32(bytes + 8, s_sequence + 1);
    if (Readable(race->m_timer, sizeof(mkw::Timer)))
        Write32(bytes + 12, Milliseconds(&race->m_timer->m_time[0]));
    bytes[16] = laps;
    bytes[17] = (Load<u32>(&config, 0xB70) & mkw::RaceConfig::MODE_FLAGS_MIRROR ? 1 : 0) |
                (OnlineCPU::KnockoutEnabled() ? 2 : 0) | (s_mapReady ? 4 : 0);
    Write32(bytes + 20, seed);
    for (u8 id = 0; id < count; ++id) {
        const auto* player = race->m_players[id];
        if (!Readable(player, 0x44))
            return;
        const auto* scenario = const_cast<mkw::RaceConfig&>(config).getPlayer(id);
        if (!scenario)
            return;
        const bool cpu =
            scenario->m_type == mkw::RaceConfigPlayer::EPlayerType::CPU || OnlineCPU::IsCPUSlot(id);
        u32 profile = 0;
        u8  local   = 0;
        if (!Identity(id, *network, cpu, profile, local))
            return;
        u8* out         = bytes + LIVE_HEADER + id * RACER_SIZE;
        out[0]          = id;
        out[1]          = cpu ? 1 : 0;
        const u32 flags = player->m_stateFlags;
        if (flags & mkw::RaceManager::Player::FLAG_FINISHED)
            out[1] |= 2;
        if (flags & mkw::RaceManager::Player::FLAG_DISCONNECTED)
            out[1] |= 4;
        if (Knockout::IsEliminated(id))
            out[1] |= 8;
        Write16(out + 2, player->m_currentLap <= 255 ? player->m_currentLap : 0);
        Write32(out + 4, profile);
        out[8]                   = local;
        const u8 eliminatedPlace = Knockout::EliminationPlacement(id);
        // Stopped karts remain native-unfinished while spectating. Their live
        // distance rank can move; keep the agreed KO placement on the panel.
        out[9] = eliminatedPlace ? eliminatedPlace
                                 : (player->m_position <= count ? player->m_position : 0);
        const float progress = Load<float>(player, 0x0C);
        WriteFloat(out + 12, progress >= 0.0f && progress <= 255.0f ? progress : 0.0f);
        Point point = {};
        if (Position(id, count, point)) {
            out[1] |= 16;
            WriteFloat(out + 16, point.x);
            WriteFloat(out + 20, point.z);
        }
        if (flags & mkw::RaceManager::Player::FLAG_FINISHED)
            Write32(out + 24, Milliseconds(player->m_raceFinishTime));
    }
    if (!GPReport::ReportB64Encode("wl:race_live", bytes, length))
        return;
    ++s_sequence;
    s_lastSend = now;
    s_hasSent  = true;
    SendMap(now, halfSecond);
}

} // namespace wwfc::RaceTelemetry

#elif RMC
namespace wwfc::RaceTelemetry
{
void Tick()
{
}
} // namespace wwfc::RaceTelemetry
#endif
