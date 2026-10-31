#pragma once

#if RMC

#  include "Timer.hpp"

namespace wwfc::mkw
{

class RaceManager
{
public:
    static RaceManager* Instance()
    {
        extern RaceManager* s_instance AT(
            RMCXD_PORT(0x809BD730, 0x809B8F70, 0x809BC790, 0x809ABD70, 0x809BDFB0)
        );
        return s_instance;
    }

    class Player
    {
    public:
        FILL(0x00, 0x08);
        /* 0x08 */ u8 m_id;
        FILL(0x09, 0x20);
        // Live race rank, 1..playerCount. Confirmed consistent with the proven
        // m_id/m_raceFinishTime offsets (SeekyCt/mkw-structures RaceinfoPlayer);
        // validated live via wl:ko_pos.
        /* 0x20 */ u8 m_position;
        FILL(0x21, 0x24);
        // Lap the player is currently on (1-based); increments as they cross the
        // line. mkw-structures RaceinfoPlayer.currentLap.
        /* 0x24 */ u16 m_currentLap;
        FILL(0x26, 0x38);
        /* 0x38 */ u32 m_stateFlags;
        FILL(0x3C, 0x40);
        /* 0x40 */ Time* m_raceFinishTime;

        static constexpr u32 FLAG_FINISHED     = 0x02;
        static constexpr u32 FLAG_DISCONNECTED = 0x10;
        static constexpr u32 FLAG_RETIRED      = 0x20;

#  if RMCPD00
        void retire()
        {
            [[gnu::longcall]] void vanish(Player * self) AT(0x80534C78);
            vanish(this);
        }

        // Verified in retail PAL StaticR.rel. The function sets the finish
        // flag, copies the time, and calls RaceManager::EndPlayerRace itself.
        // Do not call EndPlayerRace again or substitute the misleadingly named
        // CheckEndRaceOnline symbol (that path marks a disconnect).
        void endRace(const Time& finishTime)
        {
            [[gnu::longcall]] void endRace(
                Player * self, const Time& time, bool noCamera, u32 status
            ) AT(0x805347F4);
            endRace(this, finishTime, false, 1);
        }
#  endif
    };

    static_assert(__builtin_offsetof(Player, m_id) == 0x08);
    static_assert(__builtin_offsetof(Player, m_position) == 0x20);
    static_assert(__builtin_offsetof(Player, m_currentLap) == 0x24);
    static_assert(__builtin_offsetof(Player, m_stateFlags) == 0x38);
    static_assert(__builtin_offsetof(Player, m_raceFinishTime) == 0x40);

    enum class EState {
        INTRO                = 0,
        COUNTDOWN            = 1,
        RACE                 = 2,
        FINISHED             = 3,
        FINISHED_TIME_ATTACK = 4,
    };

    FILL(0x00, 0x0C);

    // Array of per-player race data pointers (length = player count).
    /* 0x0C */ Player** m_players;

    FILL(0x10, 0x14);

    /* 0x14 */ Timer* m_timer;

    // Array of player ids ordered by finishing position: [0] is 1st place,
    // [playerCount-1] is last. mkw-structures Raceinfo.playerIdInEachPosition.
    /* 0x18 */ u8* m_playerIdInEachPosition;

    FILL(0x1C, 0x28);

    /* 0x28 */ EState m_state;
};

} // namespace wwfc::mkw

#endif // RMC
