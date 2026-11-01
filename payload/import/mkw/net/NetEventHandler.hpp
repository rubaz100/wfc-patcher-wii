#pragma once

#if RMC

#  include "import/mkw/item/ItemType.hpp"
#  include "mkwOnlineCPU.hpp"
#  include "wwfcLibC.hpp"

namespace wwfc::mkw
{

// https://github.com/SeekyCt/mkw-structures/blob/master/eventhandler.h
class NetEventHandler
{
public:
    struct __attribute__((packed)) Packet {
        struct EventInfo {
            enum class EEventType : u8 {
                NO_EVENT        = 0,
                ITEM_USED       = 1,
                ITEM_THROWN     = 2,
                ITEM_OBJECT_HIT = 3,
                ITEM_LOCKED_ON  = 5,
                ITEM_DROPPED    = 7,
            };

            bool isItemObjectValid() const
            {
                EItemGeoObjType item = static_cast<EItemGeoObjType>(itemObject);

                if (item == EItemGeoObjType::EMPTY) {
                    return true;
                }

                return ItemDefaults::isObjectValid(item);
            }

            bool isValid() const
            {
                if (!isItemObjectValid()) {
                    return false;
                }

                EItemGeoObjType item = static_cast<EItemGeoObjType>(itemObject);

                switch (eventType) {
                case EEventType::NO_EVENT:
                    return item == EItemGeoObjType::EMPTY;
                case EEventType::ITEM_USED:
                    return ItemDefaults::canUse(ItemDefaults::ItemTypeFromGeoObjType(item));
                case EEventType::ITEM_THROWN:
                    return ItemDefaults::canThrow(ItemDefaults::ItemTypeFromGeoObjType(item));
                case EEventType::ITEM_OBJECT_HIT:
                    return ItemDefaults::canHitObject(item);
                case EEventType::ITEM_LOCKED_ON:
                    return ItemDefaults::canObjectLockOn(item);
                case EEventType::ITEM_DROPPED:
                    return ItemDefaults::canDropObject(item);
                default:
                    return true;
                }
            }

            u8 getEventDataSize() const
            {
                return GetEventDataSize(itemObject, eventType);
            }

            /* 0x00 */ EEventType eventType : 3;
            /* 0x00 */ u8         itemObject : 5;
        };

        static_assert(sizeof(EventInfo) == 0x01);

        struct ItemUsedEvent {
            bool isValid(u8 itemObject, u8 playerAid) const
            {
                EItemGeoObjType item = static_cast<EItemGeoObjType>(itemObject);

                switch (item) {
                case EItemGeoObjType::KUMO: {
                    // USE stores a global racer ID, including the second
                    // local human or an agreed CPU. Validate its actual
                    // console owner instead of comparing unlike ID spaces.
                    return OnlineCPU::EventActorOwnedByAid(this->playerAid, playerAid);
                }
                default: {
                    return true;
                }
                }
            }

            /* 0x00 */ u8 _00[0x02 - 0x00];
            /* 0x02 */ u8 playerAid;
        };

        static_assert(sizeof(ItemUsedEvent) == 0x03);

        bool containsInvalidItemObject() const
        {
            for (std::size_t n = 0; n < sizeof(eventInfo); n++) {
                if (!eventInfo[n].isItemObjectValid()) {
                    return true;
                }
            }

            return false;
        }

        bool isCPUActorOwnershipValid(u8 packetSize, u8 senderAid) const
        {
#  if WWFC_ONLINE_CPU_APPLY && RMCPD00
            const auto* plan = OnlineCPU::CurrentPlan();
            if (!plan || !plan->cpuCount || !OnlineCPU::IsCPUSlot(plan->humanCount)) {
                return true;
            }

            if (packetSize < sizeof(eventInfo) || packetSize > sizeof(Packet)) {
                return false;
            }
            const std::size_t dataSize = packetSize - sizeof(eventInfo);
            std::size_t       offset   = 0;
            for (const EventInfo& info : eventInfo) {
                // SHOOT size comes from a native item-object table. Reject an
                // unknown object before asking that table for its record size.
                // The empty slot has no record and is exported as action zero.
                if (!info.isItemObjectValid()) {
                    return false;
                }
                if (info.itemObject == u8(EItemGeoObjType::EMPTY)) {
                    if (info.eventType != EventInfo::EEventType::NO_EVENT) {
                        return false;
                    }
                    continue;
                }

                const std::size_t size = info.getEventDataSize();
                if (size > dataSize - offset) {
                    return false;
                }
                const u8* data  = eventData + offset;
                u8        actor = OnlineCPU::MAX_PLAYERS;
                switch (info.eventType) {
                case EventInfo::EEventType::ITEM_USED:
                    if (size < 3) {
                        return false;
                    }
                    actor = data[2];
                    break;
                case EventInfo::EEventType::ITEM_THROWN:
                case EventInfo::EEventType::ITEM_DROPPED:
                    if (size < 4) {
                        return false;
                    }
                    // BE16(data+2) carries global racer ID in bits 8..11.
                    actor = data[2] & 0x0F;
                    break;
                default:
                    // Hit and TC transfer events can legitimately be authored
                    // by a victim/recipient on another console. Keep those on
                    // their native validation and collision authority paths.
                    break;
                }
                if (OnlineCPU::IsCPUSlot(actor) &&
                    !OnlineCPU::EventActorOwnedByAid(actor, senderAid)) {
                    return false;
                }
                offset += size;
            }
            return offset == dataSize;
#  else
            (void)packetSize;
            (void)senderAid;
            return true;
#  endif
        }

        bool isValid(u8 packetSize, u8 playerAid) const
        {
            std::size_t expectedPacketSize = sizeof(eventInfo);

            for (std::size_t n = 0; n < sizeof(eventInfo); n++) {
                if (!eventInfo[n].isValid()) {
                    return false;
                }

                expectedPacketSize += eventInfo[n].getEventDataSize();
            }

            if (expectedPacketSize != packetSize) {
                return false;
            }

            expectedPacketSize = 0;
            for (std::size_t n = 0; n < sizeof(eventInfo); n++) {
                EventInfo info       = eventInfo[n];
                u8        itemObject = info.itemObject;
                const u8* data       = eventData + expectedPacketSize;

                switch (info.eventType) {
                case EventInfo::EEventType::ITEM_USED: {
                    const ItemUsedEvent* itemUsedEvent =
                        reinterpret_cast<const ItemUsedEvent*>(data);

                    if (!itemUsedEvent->isValid(itemObject, playerAid)) {
                        return false;
                    }
                }
                default: {
                }
                }

                expectedPacketSize += info.getEventDataSize();
            }

            return true;
        }

        /* 0x00 */ EventInfo eventInfo[0x18];
        /* 0x18 */ u8        eventData[0xF8 - 0x18];
    };

    static_assert(sizeof(Packet) == 0xF8);

    static u8 GetEventDataSize(u8 itemObject, Packet::EventInfo::EEventType eventType)
    {
        [[gnu::longcall]] u8 GetEventDataSize(
            u8 itemObject, Packet::EventInfo::EEventType eventType
        ) AT(RMCXD_PORT(0x8079D76C, 0x80794760, 0x8079CDD8, 0x8078BB2C, 0x8079DD00));

        return GetEventDataSize(itemObject, eventType);
    }

    static NetEventHandler* Instance()
    {
        return s_instance;
    }

private:
    /* 0x0000 */ u8 _0000[0x2B88 - 0x0000];

    static NetEventHandler*
        s_instance AT(RMCXD_PORT(0x809C20F0, 0x809BD928, 0x809C1150, 0x809B0730, 0x809C2988));
};

static_assert(sizeof(NetEventHandler) == 0x2B88);

} // namespace wwfc::mkw

#endif // RMC
