#pragma once

#include "wwfcBase.hpp"

namespace wwfc::Knockout
{

// Keep the decision policy independent of game memory so it can be exercised
// on the host, including invalid snapshots and races with finished players.
constexpr u8 MAX_PLAYERS = 12;

constexpr bool IsPacketRaceTime(u16 minutes, u8 seconds, u16 milliseconds, bool valid)
{
    return valid && minutes < 63 && seconds < 60 && milliseconds < 1000;
}

constexpr u32 FinalPlacementTimeMs(u32 elapsed, u8 placement)
{
    // Used only AFTER all surviving racers finish. Keep the result inside the
    // stock 6-bit minute field instead of sending a clamped 100-minute DNF.
    constexpr u32 maxTime = 63u * 60000u - 1;
    u32           offset  = placement * 10u;
    if (elapsed == 0 || placement < 2 || placement > MAX_PLAYERS || elapsed > maxTime - offset) {
        return 0;
    }
    return elapsed + offset;
}

struct Snapshot {
    u8  playerCount;
    u8  order[MAX_PLAYERS];
    u16 laps[MAX_PLAYERS];
    u16 finishedMask;
    u16 disconnectedMask;
};

struct Decision {
    u16 leaderLap;
    u8  playerId;
    u16 survivors;
};

class RaceState
{
public:
    void reset()
    {
        m_lastLeaderLap  = 0;
        m_eliminatedMask = 0;
        m_playerCount    = 0;
        m_armed          = false;
        for (u8& placement : m_placements) {
            placement = 0;
        }
    }

    void bind(u8 count)
    {
        if (m_playerCount != count) {
            reset();
            m_playerCount = count;
        }
    }

    bool evaluate(const Snapshot& snapshot, Decision& decision)
    {
        if (snapshot.playerCount < 2 || snapshot.playerCount > MAX_PLAYERS) {
            return false;
        }

        // Reject partial/invalid ordering before using any player id as an
        // array index, or changing the lap baseline.
        u16 seen = 0;
        for (u8 position = 0; position < snapshot.playerCount; ++position) {
            u8 id = snapshot.order[position];
            if (id >= snapshot.playerCount || (seen & (1u << id)) != 0) {
                return false;
            }
            seen |= 1u << id;
        }

        if (m_playerCount != snapshot.playerCount) {
            reset();
            m_playerCount = snapshot.playerCount;
        }
        u16 active = seen & ~(snapshot.finishedMask | snapshot.disconnectedMask | m_eliminatedMask);
        u8  leader = MAX_PLAYERS;
        u8  last   = MAX_PLAYERS;
        for (u8 position = 0; position < snapshot.playerCount; ++position) {
            u8 id = snapshot.order[position];
            if ((active & (1u << id)) == 0) {
                continue;
            }
            if (leader == MAX_PLAYERS) {
                leader = id;
            }
            last = id;
        }
        if (leader == MAX_PLAYERS || leader == last) {
            return false;
        }

        u16 lap = snapshot.laps[leader];
        if (!m_armed) {
            m_lastLeaderLap = lap;
            m_armed         = true;
            return false;
        }
        if (lap <= m_lastLeaderLap) {
            return false;
        }
        m_lastLeaderLap = lap;
        if (lap < 2) {
            // Going from lap 0 to 1 starts the race; it completes no lap.
            return false;
        }
        decision = { lap, last, static_cast<u16>(active & ~(1u << last)) };
        return true;
    }

    void markEliminated(u8 id, u8 placement)
    {
        if (id < m_playerCount && placement >= 2 && placement <= m_playerCount) {
            m_eliminatedMask |= 1u << id;
            m_placements[id] = placement;
        }
    }

    u8 placement(u8 id) const
    {
        return id < MAX_PLAYERS ? m_placements[id] : 0;
    }

    bool survivorsFinished(const Snapshot& snapshot) const
    {
        if (snapshot.playerCount != m_playerCount || m_playerCount < 2 ||
            m_playerCount > MAX_PLAYERS || m_eliminatedMask == 0) {
            return false;
        }
        u16 survivors =
            ((1u << m_playerCount) - 1) & ~(m_eliminatedMask | snapshot.disconnectedMask);
        return survivors != 0 && (snapshot.finishedMask & survivors) == survivors;
    }

    u16 eliminatedMask() const
    {
        return m_eliminatedMask;
    }

private:
    u16  m_lastLeaderLap           = 0;
    u16  m_eliminatedMask          = 0;
    u8   m_playerCount             = 0;
    bool m_armed                   = false;
    u8   m_placements[MAX_PLAYERS] = {};
};

} // namespace wwfc::Knockout
