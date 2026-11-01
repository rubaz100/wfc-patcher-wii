#pragma once

#if RMCPD00

namespace wwfc::mkw
{

inline void KeepSpectatorInputActive()
{
    extern void* spectatorInputManager AT(0x809BD70C);
    if (!spectatorInputManager)
        return;
    // Native leaf: resets only the four controller idle-counter pairs. KO
    // permits one local human; callers invoke this only for an eliminated
    // unfinished local racer, preserving ordinary racing AFK/race-cap checks.
    [[gnu::longcall]] void resetIdle(void*) AT(0x805235AC);
    resetIdle(spectatorInputManager);
}

// Minimal PAL layouts corroborated against StaticR.rel. These are views of
// existing race objects, never allocations or scene-level spectator switches.
struct KartPointersForRetirement {
    FILL(0x00, 0x04);
    /* 0x04 */ void* status;
    FILL(0x08, 0x28);
    /* 0x28 */ KartPointersForRetirement** movement;
};

struct KartManagerForRetirement {
    FILL(0x00, 0x20);
    /* 0x20 */ KartPointersForRetirement*** players;
    /* 0x24 */ u8                           count;

    static bool CanRetire(u8 id)
    {
        extern KartManagerForRetirement* kartManagerForRetirement AT(0x809C18F8);
        if (!kartManagerForRetirement || !kartManagerForRetirement->players ||
            kartManagerForRetirement->count > 12 || id >= kartManagerForRetirement->count) {
            return false;
        }
        auto kart = kartManagerForRetirement->players[id];
        if (!kart || !*kart || !(*kart)->movement || !*(*kart)->movement) {
            return false;
        }
        return (*(*kart)->movement)->status != nullptr;
    }
};

struct ExistingRaceCamera {
    FILL(0x00, 0x9C);
    /* 0x09C */ u8 playerId;
    FILL(0x9D, 0x342);
    /* 0x342 */ u8 indexesSortedCameras;
};

struct ExistingCameraManager {
    FILL(0x00, 0x14);
    /* 0x14 */ ExistingRaceCamera** cameras;
    FILL(0x18, 0x1C);
    /* 0x1C */ u32 cameraCount;
    FILL(0x20, 0x27);
    /* 0x27 */ u8 focusedIndex;

    static bool Follow(u8 playerId)
    {
        extern ExistingCameraManager* cameraManagerForSpectating AT(0x809C19B8);
        if (!cameraManagerForSpectating || !cameraManagerForSpectating->cameras ||
            cameraManagerForSpectating->cameraCount != 1 ||
            cameraManagerForSpectating->focusedIndex != 0 || playerId >= 12) {
            return false;
        }
        auto camera = cameraManagerForSpectating->cameras[0];
        if (!camera || camera->indexesSortedCameras) {
            return false;
        }
        camera->playerId = playerId;
        return true;
    }
};

static_assert(__builtin_offsetof(KartPointersForRetirement, status) == 0x04);
static_assert(__builtin_offsetof(KartPointersForRetirement, movement) == 0x28);
static_assert(__builtin_offsetof(KartManagerForRetirement, players) == 0x20);
static_assert(__builtin_offsetof(KartManagerForRetirement, count) == 0x24);
static_assert(__builtin_offsetof(ExistingRaceCamera, playerId) == 0x9C);
static_assert(__builtin_offsetof(ExistingRaceCamera, indexesSortedCameras) == 0x342);
static_assert(__builtin_offsetof(ExistingCameraManager, cameras) == 0x14);
static_assert(__builtin_offsetof(ExistingCameraManager, cameraCount) == 0x1C);
static_assert(__builtin_offsetof(ExistingCameraManager, focusedIndex) == 0x27);

} // namespace wwfc::mkw

#endif
