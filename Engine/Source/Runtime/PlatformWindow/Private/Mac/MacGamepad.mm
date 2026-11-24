#include <Core/Memory/UniquePtr.h>
#include <Platform/Sync.h>
#include <PlatformWindow/Window.h>

// AssertMacros.h의 소문자 매크로(check·verify·require)를 만들지 않게 한다(UE와 같다).
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <Foundation/Foundation.h>
#include <GameController/GameController.h>
#undef __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES

// macOS 게임패드 backend다(Docs/Design/MacPlatform.ko.md 5.4절, UE AppleControllerInterface).
// 연결·해제 알림은 메인 스레드로 오고, Poll(게임 스레드)이 슬롯마다 extendedGamepad 값을 읽어 바뀐
// 것만 이벤트로 넣는다(Windows XInput과 같은 계약). 슬롯은 연결 순서로 가장 작은 빈 자리를 쓴다.
namespace rocks::platform {

namespace {

// 버튼 하나의 대응이다.
uint32 ButtonBit(input::GamepadButton button) noexcept {
    return 1u << static_cast<uint32>(button);
}

uint32 PressedBit(GCControllerButtonInput* input, input::GamepadButton button) noexcept {
    return input != nil && [input isPressed] ? ButtonBit(button) : 0u;
}

input::GamepadSnapshot Snapshot(GCExtendedGamepad* pad) noexcept {
    input::GamepadSnapshot snapshot;
    snapshot.connected = true;
    snapshot.buttons = PressedBit([pad buttonA], input::GamepadButton::South) |
                       PressedBit([pad buttonB], input::GamepadButton::East) |
                       PressedBit([pad buttonX], input::GamepadButton::West) |
                       PressedBit([pad buttonY], input::GamepadButton::North) |
                       PressedBit([pad leftShoulder], input::GamepadButton::LeftShoulder) |
                       PressedBit([pad rightShoulder], input::GamepadButton::RightShoulder) |
                       PressedBit([pad buttonOptions], input::GamepadButton::Back) |
                       PressedBit([pad buttonMenu], input::GamepadButton::Start) |
                       PressedBit([pad leftThumbstickButton], input::GamepadButton::LeftThumb) |
                       PressedBit([pad rightThumbstickButton], input::GamepadButton::RightThumb) |
                       PressedBit([[pad dpad] up], input::GamepadButton::DPadUp) |
                       PressedBit([[pad dpad] down], input::GamepadButton::DPadDown) |
                       PressedBit([[pad dpad] left], input::GamepadButton::DPadLeft) |
                       PressedBit([[pad dpad] right], input::GamepadButton::DPadRight);
    // 스틱은 [-1, 1](오른쪽·위가 +), 트리거는 [0, 1]이다. GameController 값이 같은 범위·방향이다.
    snapshot.axes[static_cast<uint32>(input::GamepadAxis::LeftX)] =
        [[[pad leftThumbstick] xAxis] value];
    snapshot.axes[static_cast<uint32>(input::GamepadAxis::LeftY)] =
        [[[pad leftThumbstick] yAxis] value];
    snapshot.axes[static_cast<uint32>(input::GamepadAxis::RightX)] =
        [[[pad rightThumbstick] xAxis] value];
    snapshot.axes[static_cast<uint32>(input::GamepadAxis::RightY)] =
        [[[pad rightThumbstick] yAxis] value];
    snapshot.axes[static_cast<uint32>(input::GamepadAxis::LeftTrigger)] = [[pad leftTrigger] value];
    snapshot.axes[static_cast<uint32>(input::GamepadAxis::RightTrigger)] =
        [[pad rightTrigger] value];
    return snapshot;
}

bool SameSnapshot(const input::GamepadSnapshot& left,
                  const input::GamepadSnapshot& right) noexcept {
    if (left.connected != right.connected || left.buttons != right.buttons) {
        return false;
    }

    for (uint32 axis = 0; axis < input::GamepadAxisCount; ++axis) {
        if (left.axes[axis] != right.axes[axis]) {
            return false;
        }
    }

    return true;
}

class GameControllerGamepads final : public GamepadBackend {
public:
    GameControllerGamepads() noexcept = default;

    ~GameControllerGamepads() override {
        RunOnMainThread(&StopOnMainThread, this);
        LockGuard guard(mutex_);
        for (Slot& slot : slots_) {
            [slot.controller release];
            slot.controller = nil;
        }
    }

    // 알림을 등록하고 이미 연결된 패드를 앉힌다(메인 스레드).
    void Start() noexcept {
        RunOnMainThread(&StartOnMainThread, this);
    }

    void Poll(input::InputQueue& queue) noexcept override {
        LockGuard guard(mutex_);
        for (uint32 index = 0; index < input::MaxGamepads; ++index) {
            Slot& slot = slots_[index];
            input::GamepadSnapshot current;
            GCExtendedGamepad* pad =
                slot.controller != nil ? [slot.controller extendedGamepad] : nil;
            if (pad != nil) {
                current = Snapshot(pad);
            }

            if (!SameSnapshot(current, slot.last)) {
                slot.last = current;
                (void)queue.Push(input::InputEvent::Gamepad(static_cast<uint8>(index), current));
            }
        }
    }

private:
    struct Slot {
        GCController* controller = nil;
        input::GamepadSnapshot last;
    };

    // extendedGamepad가 있는 컨트롤러만 가장 작은 빈 슬롯에 앉힌다. 이미 있으면 그대로다.
    void Attach(GCController* controller) noexcept {
        if ([controller extendedGamepad] == nil) {
            return;
        }

        LockGuard guard(mutex_);
        for (Slot& slot : slots_) {
            if (slot.controller == controller) {
                return;
            }
        }

        for (Slot& slot : slots_) {
            if (slot.controller == nil) {
                slot.controller = [controller retain];
                return;
            }
        }
    }

    void Detach(GCController* controller) noexcept {
        LockGuard guard(mutex_);
        for (Slot& slot : slots_) {
            if (slot.controller == controller) {
                [slot.controller release];
                slot.controller = nil;
            }
        }
    }

    static void StartOnMainThread(void* raw) noexcept {
        auto& self = *static_cast<GameControllerGamepads*>(raw);
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        GameControllerGamepads* owner = &self;
        self.connectObserver_ = [[center addObserverForName:GCControllerDidConnectNotification
                                                     object:nil
                                                      queue:nil
                                                 usingBlock:^(NSNotification* notification) {
                                                   owner->Attach([notification object]);
                                                 }] retain];
        self.disconnectObserver_ = [[center addObserverForName:GCControllerDidDisconnectNotification
                                                        object:nil
                                                         queue:nil
                                                    usingBlock:^(NSNotification* notification) {
                                                      owner->Detach([notification object]);
                                                    }] retain];
        for (GCController* controller in [GCController controllers]) {
            self.Attach(controller);
        }
    }

    static void StopOnMainThread(void* raw) noexcept {
        auto& self = *static_cast<GameControllerGamepads*>(raw);
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        if (self.connectObserver_ != nil) {
            [center removeObserver:self.connectObserver_];
            [self.connectObserver_ release];
            self.connectObserver_ = nil;
        }

        if (self.disconnectObserver_ != nil) {
            [center removeObserver:self.disconnectObserver_];
            [self.disconnectObserver_ release];
            self.disconnectObserver_ = nil;
        }
    }

    // 슬롯을 지킨다(알림은 메인 스레드, Poll은 게임 스레드).
    Mutex mutex_;
    Slot slots_[input::MaxGamepads];
    // 메인 스레드만 만진다.
    id connectObserver_ = nil;
    id disconnectObserver_ = nil;
};

} // namespace

Result<UniquePtr<GamepadBackend>> GamepadBackend::Create(Allocator& allocator) noexcept {
    auto created = UniquePtr<GameControllerGamepads>::TryMake(allocator);
    if (!created) {
        return Result<UniquePtr<GamepadBackend>>::Failure(created.Error());
    }

    created.Value()->Start();
    return Result<UniquePtr<GamepadBackend>>::Success(
        UniquePtr<GamepadBackend>(Move(created.Value())));
}

} // namespace rocks::platform
