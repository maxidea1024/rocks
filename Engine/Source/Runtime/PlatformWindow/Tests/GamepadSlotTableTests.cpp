#include "../Private/Common/GamepadSlotTable.h"

#include <TestSupport/Test.h>

// 패드 슬롯 배정 시험이다(Docs/Design/AndroidPlatform.ko.md 5절 — AD12): 새 장치의 자리 순서,
// 같은 descriptor의 다시 연결, 끊기 비트, 꽉 찬 표.
using namespace rocks;
using namespace rocks::platform::gamepad;
namespace gamepad_slot_table_tests_file {

// 아래 시험은 슬롯 넷을 채우고 넘치는 경우를 센다.
static_assert(input::MaxGamepads == 4, "GamepadSlotTable tests assume four gamepad slots");

namespace {

constexpr uint64 PadA = 0xA1;
constexpr uint64 PadB = 0xB2;
constexpr uint64 PadC = 0xC3;
constexpr uint64 PadD = 0xD4;
constexpr uint64 PadE = 0xE5;

} // namespace

// 새 장치는 차례로 빈 슬롯에 앉고, 연결된 장치는 같은 슬롯을 돌려준다. 꽉 차면 NoGamepadSlot.
TEST(GamepadSlotTable_SeatsInOrderAndFindsConnected) {
    GamepadSlotTable table;
    CHECK(!table.AnyConnected());
    CHECK(table.Find(10) == NoGamepadSlot);
    CHECK(table.Seat(10, PadA) == 0);
    CHECK(table.Seat(11, PadB) == 1);
    CHECK(table.Seat(10, PadA) == 0);
    CHECK(table.Find(11) == 1);
    CHECK(table.DeviceId(1) == 11);
    CHECK(table.IsConnected(0) && !table.IsConnected(2));
    CHECK(!table.IsConnected(NoGamepadSlot));
    CHECK(table.DeviceId(NoGamepadSlot) == 0);
    CHECK(table.Seat(12, PadC) == 2);
    CHECK(table.Seat(13, PadD) == 3);
    CHECK(table.Seat(14, PadE) == NoGamepadSlot);
    CHECK(table.AnyConnected());
}

// 목록에 없는 연결된 슬롯만 끊고 그 비트를 돌려준다. 끊긴 슬롯은 Find에 보이지 않는다.
TEST(GamepadSlotTable_DisconnectMissing) {
    GamepadSlotTable table;
    REQUIRE(table.Seat(10, PadA) == 0);
    REQUIRE(table.Seat(11, PadB) == 1);
    REQUIRE(table.Seat(12, PadC) == 2);
    const int32 present[] = {3, 11, 99};
    CHECK(table.DisconnectMissing(Span<const int32>(present)) == ((1u << 0) | (1u << 2)));
    CHECK(table.Find(10) == NoGamepadSlot);
    CHECK(!table.IsConnected(0) && table.IsConnected(1) && !table.IsConnected(2));
    CHECK(table.DeviceId(0) == 0);
    // 이미 끊긴 슬롯은 다시 세지 않는다. 빈 목록이면 남은 것이 모두 끊긴다.
    CHECK(table.DisconnectMissing(Span<const int32>(present)) == 0);
    CHECK(table.DisconnectMissing(Span<const int32>()) == (1u << 1));
    CHECK(!table.AnyConnected());
}

// 같은 descriptor는 새 장치 ID로 와도 원래 슬롯에 앉는다. 새 패드는 한 번도 쓰지 않은 슬롯을 먼저
// 쓰고, 그것이 없을 때만 가장 작은 끊긴 슬롯을 쓴다(그 자리의 옛 descriptor는 잊는다).
TEST(GamepadSlotTable_ReconnectKeepsSlot) {
    GamepadSlotTable table;
    REQUIRE(table.Seat(10, PadA) == 0);
    REQUIRE(table.Seat(11, PadB) == 1);
    const int32 onlyB[] = {11};
    REQUIRE(table.DisconnectMissing(Span<const int32>(onlyB)) == (1u << 0));
    // 다른 패드는 끊긴 0번이 아니라 빈 2번에 앉는다.
    CHECK(table.Seat(20, PadC) == 2);
    // A가 새 ID로 돌아오면 0번이다.
    CHECK(table.Seat(21, PadA) == 0);
    CHECK(table.DeviceId(0) == 21);
    // 표를 채운 뒤 0번(A)을 끊으면, 새 패드 E는 끊긴 0번을 쓴다.
    REQUIRE(table.Seat(22, PadD) == 3);
    const int32 withoutA[] = {11, 20, 22};
    REQUIRE(table.DisconnectMissing(Span<const int32>(withoutA)) == (1u << 0));
    CHECK(table.Seat(23, PadE) == 0);
    // 0번은 이제 E 자리다 — A가 돌아와도 0번을 되찾지 못한다(자리가 없다).
    CHECK(table.Seat(24, PadA) == NoGamepadSlot);
}

// descriptor를 모르면 어느 끊긴 슬롯과도 짝짓지 않는다(같은 "모름"끼리도).
TEST(GamepadSlotTable_UnknownDescriptorNeverMatches) {
    GamepadSlotTable table;
    REQUIRE(table.Seat(10, UnknownDescriptorHash) == 0);
    REQUIRE(table.Seat(11, PadB) == 1);
    const int32 onlyB[] = {11};
    REQUIRE(table.DisconnectMissing(Span<const int32>(onlyB)) == (1u << 0));
    // 모르는 새 장치는 끊긴 0번이 아니라 빈 2번이다.
    CHECK(table.Seat(12, UnknownDescriptorHash) == 2);
    // 빈 슬롯이 없으면 끊긴 0번을 쓴다.
    REQUIRE(table.Seat(13, PadC) == 3);
    CHECK(table.Seat(14, UnknownDescriptorHash) == 0);
}

// 옛 장치가 빠졌는데 장치 확인 전에 같은 패드가 새 ID로 오면 연결 중인 그 슬롯을 이어받는다(다른
// 빈 슬롯으로 가지 않는다 — UE도 같은 descriptor의 옛 장치를 지운다). 모르는 descriptor는 이어받지
// 않는다.
TEST(GamepadSlotTable_SameDescriptorTakesOverConnectedSlot) {
    GamepadSlotTable table;
    REQUIRE(table.Seat(10, PadA) == 0);
    REQUIRE(table.Seat(11, UnknownDescriptorHash) == 1);
    CHECK(table.Seat(20, PadA) == 0);
    CHECK(table.DeviceId(0) == 20);
    CHECK(table.Find(10) == NoGamepadSlot);
    // 옛 ID는 목록에 없으니 확인해도 새 장치는 끊기지 않는다.
    const int32 present[] = {20, 11};
    CHECK(table.DisconnectMissing(Span<const int32>(present)) == 0);
    CHECK(table.Seat(21, UnknownDescriptorHash) == 2);
}

// 음수 장치 ID(가상 장치 -1)도 목록의 값으로 똑같이 본다.
TEST(GamepadSlotTable_NegativeDeviceIds) {
    GamepadSlotTable table;
    REQUIRE(table.Seat(-1, PadA) == 0);
    REQUIRE(table.Seat(7, PadB) == 1);
    const int32 virtualOnly[] = {-1};
    CHECK(table.DisconnectMissing(Span<const int32>(virtualOnly)) == (1u << 1));
    CHECK(table.Find(-1) == 0);
    const int32 other[] = {7};
    CHECK(table.DisconnectMissing(Span<const int32>(other)) == (1u << 0));
    CHECK(table.Find(-1) == NoGamepadSlot);
}

} // namespace gamepad_slot_table_tests_file
