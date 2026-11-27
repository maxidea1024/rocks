#pragma once

#include <Base/TypeTraits.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Meta/LaneTransfer.h>
#include <UiCore/Rendering/UiSceneRecord.h>

namespace rocks::ui {

// 장면 pass 기록 함수다. 캡처가 없는 함수만 받는다(게임 lane 주소를 캡처로 넘기지 못하게 한다).
template <class T>
using UiScenePassFunction = void (*)(UiScenePassContext& context, T& packet) noexcept;

// 게임 lane에서 만든 장면 packet이다. 형식을 지운 값 하나와 그 기록 함수를 allocator 메모리에
// 소유한다. 렌더러가 프레임 저장소로 MoveTo하면 빈다. 이동 전용이다.
//
// 사용 예(ViewportSource::Extract 안 — UiSceneExtract::Post가 이것을 만든다):
//   struct ScenePacket { Mat4 viewProjection; uint32 meshCount; };
//   ROCKS_LANE_TRANSFERABLE(ScenePacket);   // 전역 이름공간에서, 첫 사용 전에 등록
//   extract.Post(ScenePacket{view, count}, &RecordScene);
class ROCKS_UICORE_API UiScenePacket {
public:
    UiScenePacket() noexcept = default;
    ~UiScenePacket() noexcept;

    UiScenePacket(UiScenePacket&& other) noexcept;
    UiScenePacket& operator=(UiScenePacket&& other) noexcept;
    UiScenePacket(const UiScenePacket&) = delete;
    UiScenePacket& operator=(const UiScenePacket&) = delete;

    // value를 allocator 메모리로 옮겨 담는다. T는 렌더 lane packet과 같은 전달 조건이다
    // (LaneTransferable — ROCKS_LANE_TRANSFERABLE 등록, 참조·포인터 아님, 예외 없는 이동·소멸)이고,
    // 리플렉션 struct면 객체 참조·약한 참조·이벤트 필드가 없어야 한다(Development 검사
    // CheckLaneTransfer — Docs/Design/RenderBase 5.2절). 실패: 기록 함수 없음·스키마 검사 실패
    // InvalidArgument, 할당 실패는 그 오류(실패하면 value는 옮기지 않는다).
    template <LaneTransferable T>
    static Result<UiScenePacket> TryMake(Allocator& allocator, T&& value,
                                         UiScenePassFunction<T> record) noexcept {
        if (!record) {
            return Result<UiScenePacket>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto checked = CheckLaneTransfer<T>(); !checked) {
            return Result<UiScenePacket>::Failure(checked.Error());
        }

        auto memory = allocator.Allocate(sizeof(T), alignof(T));
        if (!memory) {
            return Result<UiScenePacket>::Failure(memory.Error());
        }

        UiScenePacket packet;
        packet.allocator_ = &allocator;
        packet.object_ = InitializeAt<T>(memory.Value(), Move(value));
        packet.size_ = sizeof(T);
        packet.alignment_ = alignof(T);
        packet.move_ = &MoveObject<T>;
        packet.invoke_ = &InvokeRecord<T>;
        packet.function_ = reinterpret_cast<void (*)()>(record);
        packet.destroy_ = &DestroyObject<T>;
        return Result<UiScenePacket>::Success(Move(packet));
    }

    bool IsEmpty() const noexcept {
        return object_ == nullptr;
    }

    usize Size() const noexcept {
        return size_;
    }

    usize Alignment() const noexcept {
        return alignment_;
    }

    // 값을 destination(Size·Alignment를 만족하는 저장소)으로 이동 생성하고, 원본을 부숴 메모리를
    // 돌려준다. 이 객체는 빈다. 비어 있으면 빈 기록을 돌려준다.
    UiSceneRecord MoveTo(void* destination) noexcept;

private:
    template <class T>
    static void MoveObject(void* destination, void* source) noexcept {
        InitializeAt<T>(destination, Move(*static_cast<T*>(source)));
    }

    template <class T>
    static void InvokeRecord(UiScenePassContext& context, void* object,
                             void (*function)()) noexcept {
        reinterpret_cast<UiScenePassFunction<T>>(function)(context, *static_cast<T*>(object));
    }

    template <class T>
    static void DestroyObject(void* object) noexcept {
        static_cast<T*>(object)->~T();
    }

    void Reset() noexcept;

    Allocator* allocator_ = nullptr;
    void* object_ = nullptr;
    usize size_ = 0;
    usize alignment_ = 0;
    void (*move_)(void* destination, void* source) noexcept = nullptr;
    UiSceneRecord::InvokeFunction invoke_ = nullptr;
    void (*function_)() = nullptr;
    UiSceneRecord::DestroyFunction destroy_ = nullptr;
};

} // namespace rocks::ui
