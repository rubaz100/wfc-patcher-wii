#pragma once

#include "wwfcBase.hpp"

#if RMCPD00

namespace wwfc::mkw
{

struct SpectatorInput {
    enum Flags : u32 {
        PREVIOUS = 1u << 0,
        NEXT     = 1u << 1,
        LEADER   = 1u << 2,
    };

    bool available;
    u32  held;
};

namespace SpectatorInputDetail
{

// PAL Input::Manager and ControllerHolder views. Native Holder::Update
// (0x80521198) copies UI-controller+0x20 to holder+0x5C every frame, even
// when the separate gameplay controller is AI-controlled or input-blocked.
struct Controller {
    FILL(0x00, 0x50);
    // Retail code uses nonzero as connected, despite some headers naming
    // this byte hasError. Dummy/ghost/AI devices are excluded below.
    /* 0x50 */ u8 connected;
};

struct Holder {
    FILL(0x00, 0x08);
    /* 0x08 */ const Controller* uiController;
    FILL(0x0C, 0x5C);
    /* 0x5C */ u16 menuActions;
    FILL(0x5E, 0xEC);
};

struct Manager {
    FILL(0x00, 0x04);
    /* 0x04 */ Holder realControllers[4];
    FILL(0x3B4, 0x415C);
};

static_assert(__builtin_offsetof(Controller, connected) == 0x50);
static_assert(__builtin_offsetof(Holder, uiController) == 0x08);
static_assert(__builtin_offsetof(Holder, menuActions) == 0x5C);
static_assert(sizeof(Holder) == 0xEC);
static_assert(__builtin_offsetof(Manager, realControllers) == 0x04);
static_assert(sizeof(Manager) == 0x415C);

inline bool Readable(const volatile void* pointer, u32 bytes)
{
    const u32 address = reinterpret_cast<u32>(pointer);
    return !(address & 3) && bytes <= 0x04000000u &&
           ((address >= 0x80000000u && address <= 0x81800000u - bytes) ||
            (address >= 0x90000000u && address <= 0x94000000u - bytes));
}

} // namespace SpectatorInputDetail

// controllerIndex is RaceConfigPlayer::m_inputIndex (+0x06), not the
// controller-type field at +0xD0. The caller owns press-edge/repeat handling.
// Only normalized menu D-pad actions are returned; sticks and race inputs
// remain untouched. Wii Remote alone follows the native sideways menu layout.
inline SpectatorInput ReadSpectatorInput(u32 controllerIndex)
{
    using namespace SpectatorInputDetail;
    if (controllerIndex >= 4)
        return {};
    extern const volatile Manager* volatile spectatorInputManager AT(0x809BD70C);
    const auto*                                                   manager = spectatorInputManager;
    if (!Readable(manager, sizeof(Manager)))
        return {};
    const auto& holder         = manager->realControllers[controllerIndex];
    const auto* device         = holder.uiController;
    const u32   managerAddress = reinterpret_cast<u32>(manager);
    const u32   deviceAddress  = reinterpret_cast<u32>(device);
    bool        physical       = false;
    // Native Manager constructor 0x805232F0 embeds exactly these four Wii
    // devices and four GCN devices. Check membership before dereferencing.
    for (u32 channel = 0; channel < 4; ++channel) {
        if (deviceAddress == managerAddress + 0x1720 + channel * 0x920) {
            // WiiController::GetType (0x8052292C) reads this field: wheel,
            // Nunchuk, Classic are 0..2; absent/unsupported types are rejected.
            const u32 type = *reinterpret_cast<const volatile u32*>(deviceAddress + 0x8DC);
            if (type > 2)
                return {};
            physical = true;
            break;
        }
        if (deviceAddress == managerAddress + 0x3BA0 + channel * 0xB0) {
            physical = true;
            break;
        }
    }
    if (!physical || !static_cast<const volatile Controller*>(device)->connected)
        return {};
    // UIState::buttonActions: down=0x10, left=0x20, right=0x40, normalized
    // by retail Wii/Classic/GCN adapters before ControllerHolder copies them.
    const u16 actions = holder.menuActions;
    u32       held    = 0;
    if (actions & 0x20)
        held |= SpectatorInput::PREVIOUS;
    if (actions & 0x40)
        held |= SpectatorInput::NEXT;
    if (actions & 0x10)
        held |= SpectatorInput::LEADER;
    return { true, held };
}

} // namespace wwfc::mkw

#endif
