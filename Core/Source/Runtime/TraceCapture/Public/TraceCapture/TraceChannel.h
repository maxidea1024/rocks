#pragma once

#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <TraceCapture/TraceFormat.h>

namespace rocks {

namespace detail {
struct TraceChannelAccess;
} // namespace detail

// trace 기록의 켜기·끄기 단위다(UE FChannel 대응, Docs/Design/TraceSystem.ko.md 5절).
//
// 채널 객체는 기록하는 모듈이 정적 저장소에 둔다. 생성자가 constexpr이라 전역 생성자 없이 상수
// 초기화되며, 처음에는 꺼져 있다. 모듈 등록 함수가 TraceSession::RegisterChannel로 세션에 등록해야
// 켤 수 있다(결정 T3: 등록 전에는 꺼져 있다).
//
// 사용 예:
//   // 모듈 cpp의 정적 저장소(상수 초기화)
//   TraceChannel FrameChannel{"frame", "Frame boundaries"};
//   // 모듈 등록 함수
//   session.RegisterChannel(FrameChannel, true);
//   // 기록 지점: 꺼져 있으면 relaxed 읽기와 분기 하나로 끝난다
//   if (FrameChannel.IsEnabled()) { ... }
//
// 이름: 소문자 점 경로다(`cpu`, `memory.alloc`). 조각은 [a-z][a-z0-9_]*이고 길이는
// TraceFormat::MaxNameBytes 이하다. 이름은 정확히 일치해야 같은 채널이다(대소문자·접미사 무시
// 없음).
//
// 스레드: IsEnabled·Id·Name은 어느 스레드에서나 부를 수 있다. 상태 변경은 TraceSession만 한다.
// 수명: name·description 문자열과 객체는 등록부터 TraceSession::UnregisterChannel 또는 세션
// Stop까지 살아 있어야 한다. 세션은 등록 때 이름·설명을 정의 기록으로 복사하므로 그 뒤의 분석은
// 원본 문자열을 다시 읽지 않는다.
class TraceChannel {
public:
    // 등록되지 않은 채널의 Id다.
    static constexpr uint16 NoId = TraceFormat::NoChannelId;

    // readOnly는 세션 시작 뒤 켜짐 상태를 바꿀 수 없는 채널이다. 등록 때 정한 상태로 고정된다.
    constexpr TraceChannel(const char* name, const char* description,
                           bool readOnly = false) noexcept
        : name_(name), description_(description), readOnly_(readOnly) {
    }

    TraceChannel(const TraceChannel&) = delete;
    TraceChannel& operator=(const TraceChannel&) = delete;

    // 현재 켜져 있는지다. 켜짐은 등록된 세션이 실행 중이라는 뜻도 포함한다. relaxed 읽기이므로 다른
    // 스레드의 켜기·끄기는 조금 늦게 보일 수 있다(기록 하나가 더 나가거나 덜 나간다).
    bool IsEnabled() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&enabled_) != 0;
    }

    // 세션이 발급한 채널 id다. 등록 전·해제 뒤·세션 종료 뒤에는 NoId다.
    uint16 Id() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&id_);
    }

    const char* Name() const noexcept {
        return name_;
    }

    const char* Description() const noexcept {
        return description_;
    }

    bool IsReadOnly() const noexcept {
        return readOnly_;
    }

private:
    friend struct detail::TraceChannelAccess;

    const char* name_;
    const char* description_;
    bool readOnly_;
    uint32 enabled_ = 0;
    uint16 id_ = NoId;
};

} // namespace rocks
