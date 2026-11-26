#include "GamepadSlotTable.h"

namespace rocks::platform::gamepad {

uint32 GamepadSlotTable::Find(int32 deviceId) const noexcept {
    for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
        if (slots_[slot].connected && slots_[slot].deviceId == deviceId) {
            return slot;
        }
    }

    return NoGamepadSlot;
}

uint32 GamepadSlotTable::Seat(int32 deviceId, uint64 descriptorHash) noexcept {
    if (const uint32 seated = Find(deviceId); seated != NoGamepadSlot) {
        return seated;
    }

    // 1. 같은 패드가 다시 연결됐다(descriptor가 같은 슬롯). 연결 중인 슬롯이 먼저다 — 옛 장치가
    // 빠졌는데 장치 확인 전에 새 장치 ID의 이벤트가 왔다(UE도 같은 descriptor의 옛 장치를 지운다).
    if (descriptorHash != UnknownDescriptorHash) {
        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            const Slot& entry = slots_[slot];
            if (entry.connected && entry.descriptorHash == descriptorHash) {
                return Occupy(slot, deviceId, descriptorHash);
            }
        }

        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            const Slot& entry = slots_[slot];
            if (entry.seated && !entry.connected && entry.descriptorHash == descriptorHash) {
                return Occupy(slot, deviceId, descriptorHash);
            }
        }
    }

    // 2. 한 번도 쓰지 않은 슬롯(다른 패드가 돌아올 자리를 남긴다).
    for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
        if (!slots_[slot].seated) {
            return Occupy(slot, deviceId, descriptorHash);
        }
    }

    // 3. 가장 작은 끊긴 슬롯(그 자리의 옛 descriptor는 잊는다).
    for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
        if (!slots_[slot].connected) {
            return Occupy(slot, deviceId, descriptorHash);
        }
    }

    return NoGamepadSlot;
}

uint32 GamepadSlotTable::DisconnectMissing(Span<const int32> connectedIds) noexcept {
    uint32 lost = 0;
    for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
        Slot& entry = slots_[slot];
        if (!entry.connected) {
            continue;
        }

        bool present = false;
        for (const int32 id : connectedIds) {
            if (id == entry.deviceId) {
                present = true;
                break;
            }
        }

        if (!present) {
            entry.connected = false;
            entry.deviceId = 0;
            lost |= 1u << slot;
        }
    }

    return lost;
}

bool GamepadSlotTable::IsConnected(uint32 slot) const noexcept {
    return slot < input::MaxGamepads && slots_[slot].connected;
}

int32 GamepadSlotTable::DeviceId(uint32 slot) const noexcept {
    return IsConnected(slot) ? slots_[slot].deviceId : 0;
}

bool GamepadSlotTable::AnyConnected() const noexcept {
    for (const Slot& entry : slots_) {
        if (entry.connected) {
            return true;
        }
    }

    return false;
}

uint32 GamepadSlotTable::Occupy(uint32 slot, int32 deviceId, uint64 descriptorHash) noexcept {
    Slot& entry = slots_[slot];
    entry.seated = true;
    entry.connected = true;
    entry.deviceId = deviceId;
    entry.descriptorHash = descriptorHash;
    return slot;
}

} // namespace rocks::platform::gamepad
