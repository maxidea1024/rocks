#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <TraceCapture/TraceChannel.h>
#include <TraceCapture/TraceFormat.h>

namespace rocks {

namespace detail {
struct TraceEventTypeAccess;
} // namespace detail

// 이벤트 형의 필드 하나다(결정 T6: 직접 constexpr 표로 쓴다).
// - 고정 타입(Bool … Id)은 고정부 struct 안의 byte offset을 offset에 쓴다
//   (Base/Compiler.h의 ROCKS_OFFSET_OF).
// - 가변 타입(String·배열)은 offset에 TraceField::Variable을 쓰고, 기록 때 필드 표에 나온 순서대로
//   TraceVariablePart를 넘긴다.
// name은 정적 수명 문자열이다. 세션이 등록 때 스키마로 복사한다.
struct TraceField {
    static constexpr uint16 Variable = TraceFormat::VariableFieldOffset;

    const char* name;
    TraceFieldType type;
    uint16 offset;
};

// 이벤트 형의 성질이다. 비트 OR로 함께 쓴다.
enum class TraceEventFlags : uint8 {
    None = 0,
    // 스레드 사이 순서가 필요한 기록이다. 기록마다 세션 전역 순서 번호(u64)를 받는다(4.5절).
    Ordered = TraceFormat::TypeFlagOrdered,
    // 정의 기록이다. 스레드 블록이 아니라 세션의 정의 버퍼에 쓰고, 정의 저장소에 보관해 늦게 붙은
    // 출력에도 다시 보낸다(4.3절).
    Definition = TraceFormat::TypeFlagDefinition,
};

constexpr TraceEventFlags operator|(TraceEventFlags left, TraceEventFlags right) noexcept {
    return static_cast<TraceEventFlags>(static_cast<uint8>(left) | static_cast<uint8>(right));
}

constexpr bool HasTraceEventFlag(TraceEventFlags flags, TraceEventFlags flag) noexcept {
    return (static_cast<uint8>(flags) & static_cast<uint8>(flag)) != 0;
}

// 이벤트 형 선언이다(UE UE_TRACE_EVENT_BEGIN 대응, Docs/Design/TraceSystem.ko.md 4.2절).
//
// 객체는 기록하는 모듈이 정적 저장소에 둔다(constexpr 생성자라 상수 초기화된다). 모듈 등록 함수가
// TraceSession::RegisterEventType으로 등록하면 형 id가 발급된다. 등록 전·해제 뒤·세션 종료 뒤에는
// 기록해도 아무것도 쓰지 않는다(TraceWriteResult::Disabled).
//
// 사용 예:
//   struct FrameBegin {
//       uint64 tick;
//       uint32 frame;
//   };
//   constexpr TraceField FrameBeginFields[] = {
//       {"tick", TraceFieldType::UInt64, ROCKS_OFFSET_OF(FrameBegin, tick)},
//       {"frame", TraceFieldType::UInt32, ROCKS_OFFSET_OF(FrameBegin, frame)},
//       {"label", TraceFieldType::String, TraceField::Variable},
//   };
//   TraceEventType FrameBeginType{"Frame.Begin", FrameChannel, sizeof(FrameBegin),
//   FrameBeginFields};
//
// 고정부는 trivially copyable struct이고 기록은 그 byte를 그대로 복사한다. padding byte도
// 복사되므로 결정적인 byte가 필요하면 값 초기화(`FrameBegin event{}`)한 뒤 채운다.
//
// 스레드: 접근자는 어느 스레드에서나 부를 수 있다. id는 TraceSession만 바꾼다.
// 수명: 이름·필드 표·채널은 등록부터 해제 또는 세션 Stop까지 살아 있어야 한다.
class TraceEventType {
public:
    static constexpr uint16 NoId = TraceFormat::InvalidTypeId;

    template <usize FieldCount>
    constexpr TraceEventType(const char* name, TraceChannel& channel, usize fixedBytes,
                             const TraceField (&fields)[FieldCount],
                             TraceEventFlags flags = TraceEventFlags::None) noexcept
        : name_(name), channel_(&channel), fields_(fields), fieldCount_(FieldCount),
          fixedBytes_(fixedBytes), flags_(flags) {
    }

    // 필드가 없는 형(고정부 없음, 가변부 없음)이다. 사건이 일어났다는 사실만 남긴다.
    constexpr TraceEventType(const char* name, TraceChannel& channel,
                             TraceEventFlags flags = TraceEventFlags::None) noexcept
        : name_(name), channel_(&channel), fields_(nullptr), fieldCount_(0), fixedBytes_(0),
          flags_(flags) {
    }

    TraceEventType(const TraceEventType&) = delete;
    TraceEventType& operator=(const TraceEventType&) = delete;

    // 세션이 발급한 형 id다. 등록 전이면 NoId다.
    uint16 Id() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&id_);
    }

    const char* Name() const noexcept {
        return name_;
    }

    TraceChannel& Channel() const noexcept {
        return *channel_;
    }

    const TraceField* Fields() const noexcept {
        return fields_;
    }

    usize FieldCount() const noexcept {
        return fieldCount_;
    }

    usize FixedBytes() const noexcept {
        return fixedBytes_;
    }

    TraceEventFlags Flags() const noexcept {
        return flags_;
    }

private:
    friend struct detail::TraceEventTypeAccess;

    const char* name_;
    TraceChannel* channel_;
    const TraceField* fields_;
    usize fieldCount_;
    usize fixedBytes_;
    TraceEventFlags flags_;
    uint16 id_ = NoId;
};

} // namespace rocks
