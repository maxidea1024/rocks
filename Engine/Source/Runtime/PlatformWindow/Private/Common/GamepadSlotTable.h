#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Input/InputTypes.h>

// 입력 장치 ID를 패드 슬롯에 앉히는 표다(Docs/Design/AndroidPlatform.ko.md 5절 — AD12). 장치가
// 끊겨도 슬롯은 그 장치의 descriptor 해시를 기억해, 같은 패드가 새 장치 ID로 다시 연결되면 같은
// 슬롯(같은 플레이어)에 앉힌다(UE AndroidInputInterface RemoveInputDevice — descriptor로 장치 ID를
// 유지한다). 플랫폼 헤더 없이 값만 다뤄 EngineTests로 시험한다. Android 패드 저장소가 잠금 아래에서
// 쓴다(스레드 안전하지 않다).
//
// 사용 예:
//   GamepadSlotTable table;
//   uint32 slot = table.Find(deviceId);
//   if (slot == NoGamepadSlot) {
//       slot = table.Seat(deviceId, descriptorHash);   // 자리가 없으면 NoGamepadSlot
//   }
//
//   const uint32 lost = table.DisconnectMissing(connectedIds);   // 끊긴 슬롯 비트
namespace rocks::platform::gamepad {

// 슬롯 없음이다(Find·Seat 실패).
inline constexpr uint32 NoGamepadSlot = input::MaxGamepads;

// descriptor를 읽지 못했다는 해시다(어느 슬롯과도 짝짓지 않는다).
inline constexpr uint64 UnknownDescriptorHash = 0;

// 시험(GamepadSlotTableTests)이 부르므로 export한다(Modular 링크).
class ROCKS_PLATFORMWINDOW_API GamepadSlotTable {
public:
    // 연결된 그 장치의 슬롯이다(없거나 끊겼으면 NoGamepadSlot).
    uint32 Find(int32 deviceId) const noexcept;

    // 장치를 앉힌다. 이미 연결돼 있으면 그 슬롯이다. 새 장치는 같은 descriptor의 슬롯(연결 중이면
    // 옛 장치가 빠졌는데 아직 확인 전인 것이다 — 이어받는다, UE AndroidInputInterface "Found input
    // device with same descriptor"가 옛 장치를 지우는 것과 같다.) →
    // 한 번도 쓰지 않은 슬롯 → 가장 작은 끊긴 슬롯 순이다(descriptorHash가 UnknownDescriptorHash면
    // 첫째를 건너뛴다). 자리가 없으면(모두 연결) NoGamepadSlot이다.
    uint32 Seat(int32 deviceId, uint64 descriptorHash) noexcept;

    // connectedIds에 없는 연결된 슬롯을 끊는다(descriptor는 남긴다). 끊은 슬롯 비트(1 << slot)다.
    uint32 DisconnectMissing(Span<const int32> connectedIds) noexcept;

    // 슬롯이 연결돼 있는지다(범위 밖은 false).
    bool IsConnected(uint32 slot) const noexcept;

    // 연결된 슬롯의 장치 ID다(끊긴·범위 밖 슬롯은 0 — IsConnected로 먼저 본다).
    int32 DeviceId(uint32 slot) const noexcept;

    // 연결된 슬롯이 하나라도 있는지다.
    bool AnyConnected() const noexcept;

private:
    struct Slot {
        // 한 번이라도 앉은 적이 있다(끊겨도 참 — descriptor를 기억한다).
        bool seated = false;
        bool connected = false;
        int32 deviceId = 0;
        uint64 descriptorHash = UnknownDescriptorHash;
    };

    uint32 Occupy(uint32 slot, int32 deviceId, uint64 descriptorHash) noexcept;

    Slot slots_[input::MaxGamepads];
};

} // namespace rocks::platform::gamepad
